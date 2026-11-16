/* Copyright (c) 2018, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   Copyright (c) 2023, Shannon Data AI and/or its affiliates. */

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <onnxruntime_cxx_api.h>

#include "ml/infra_component/llm_kv_cache.h"

namespace {
using ShannonBase::ML::LLM_Generate::KVCacheManager;

std::vector<int64_t> Shape(const std::string &layout, int seq) {
  if (layout == "bshd") return {1, seq, 8, 64};
  if (layout == "bhsd") return {1, 8, seq, 64};
  return {1, 8, 64, seq};
}

template <typename T>
T Value(int value) {
  return static_cast<T>(value);
}
template <>
Ort::Float16_t Value<Ort::Float16_t>(int value) {
  return Ort::Float16_t(static_cast<float>(value));
}

class LlmKVCacheTest : public ::testing::TestWithParam<std::string> {
 protected:
  template <typename T>
  void Decode(const char *dtype) {
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "kv-cache-test");
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    const std::filesystem::path path = std::string(LLM_KV_TEST_MODEL_DIR) + "/" + GetParam() + "_" + dtype + ".onnx";
    Ort::Session session(env, path.c_str(), options);
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    KVCacheManager<T> cache;
    cache.init(1, 8, 64, 64);
    const char *inputNames[] = {"past_key", "new_key", "past_value", "new_value"};
    const char *outputNames[] = {"present_key", "present_value"};
    std::vector<T> expectedKey(1), expectedValue(1);

    // Repeat prefill after reset. A stale staging allocation must not become
    // past, and a long prompt exercises the overlapping-copy regression.
    for (int generation = 0; generation < 2; ++generation) {
      cache.clear();
      ASSERT_EQ(0, cache.seq_len());
      for (int inputLen : {17, 1, 3, 1, 1, 1, 40}) {
        const int past = cache.seq_len();
        const int total = past + inputLen;
        auto pastShape = Shape(GetParam(), past);
        auto newShape = Shape(GetParam(), inputLen);
        auto outputShape = Shape(GetParam(), total);
        const size_t pastCount = past * 8 * 64;
        const size_t newCount = inputLen * 8 * 64;
        const size_t outputCount = total * 8 * 64;
        std::vector<T> key(newCount), value(newCount);
        for (size_t i = 0; i < newCount; ++i) {
          key[i] = Value<T>(static_cast<int>((i + past + generation) % 101) - 50);
          value[i] = Value<T>(static_cast<int>((i * 3 + past + generation) % 101) - 50);
        }
        std::array<Ort::Value, 4> referenceInputs = {
            Ort::Value::CreateTensor<T>(memory, expectedKey.data(), pastCount, pastShape.data(), 4),
            Ort::Value::CreateTensor<T>(memory, key.data(), newCount, newShape.data(), 4),
            Ort::Value::CreateTensor<T>(memory, expectedValue.data(), pastCount, pastShape.data(), 4),
            Ort::Value::CreateTensor<T>(memory, value.data(), newCount, newShape.data(), 4)};
        auto reference = session.Run(Ort::RunOptions{nullptr}, inputNames, referenceInputs.data(), 4, outputNames, 2);

        Ort::IoBinding binding(session);
        auto pastKey = Ort::Value::CreateTensor<T>(memory, cache.key_data(0), pastCount, pastShape.data(), 4);
        auto pastValue = Ort::Value::CreateTensor<T>(memory, cache.value_data(0), pastCount, pastShape.data(), 4);
        auto newKey = Ort::Value::CreateTensor<T>(memory, key.data(), newCount, newShape.data(), 4);
        auto newValue = Ort::Value::CreateTensor<T>(memory, value.data(), newCount, newShape.data(), 4);
        auto presentKey =
            Ort::Value::CreateTensor<T>(memory, cache.present_key_data(0), outputCount, outputShape.data(), 4);
        auto presentValue =
            Ort::Value::CreateTensor<T>(memory, cache.present_value_data(0), outputCount, outputShape.data(), 4);
        ASSERT_NE(cache.key_data(0), cache.present_key_data(0));
        ASSERT_NE(cache.value_data(0), cache.present_value_data(0));
        binding.BindInput("past_key", pastKey);
        binding.BindInput("past_value", pastValue);
        binding.BindInput("new_key", newKey);
        binding.BindInput("new_value", newValue);
        binding.BindOutput("present_key", presentKey);
        binding.BindOutput("present_value", presentValue);
        session.Run(Ort::RunOptions{nullptr}, binding);
        // Run alone does not publish output or change the active cache length.
        ASSERT_EQ(past, cache.seq_len());
        cache.commit_present(total);
        ASSERT_EQ(total, cache.seq_len());
        expectedKey.assign(reference[0].template GetTensorData<T>(),
                           reference[0].template GetTensorData<T>() + outputCount);
        expectedValue.assign(reference[1].template GetTensorData<T>(),
                             reference[1].template GetTensorData<T>() + outputCount);
        EXPECT_EQ(0, std::memcmp(expectedKey.data(), cache.key_data(0), outputCount * sizeof(T)));
        EXPECT_EQ(0, std::memcmp(expectedValue.data(), cache.value_data(0), outputCount * sizeof(T)));
      }

      // A bad input makes ORT reject the run. Staging may contain partial
      // writes in other failures, but active data and counters remain intact.
      const int past = cache.seq_len();
      const size_t pastCount = past * 8 * 64;
      auto pastShape = Shape(GetParam(), past);
      auto badShape = Shape(GetParam(), 1);
      badShape[0] = 2;
      std::vector<T> badInput(2 * 8 * 64);
      auto badTensor = Ort::Value::CreateTensor<T>(memory, badInput.data(), badInput.size(), badShape.data(), 4);
      auto pastTensor = Ort::Value::CreateTensor<T>(memory, cache.key_data(0), pastCount, pastShape.data(), 4);
      Ort::IoBinding invalid(session);
      invalid.BindInput("past_key", pastTensor);
      invalid.BindInput("past_value", pastTensor);
      invalid.BindInput("new_key", badTensor);
      invalid.BindInput("new_value", badTensor);
      invalid.BindOutput("present_key", memory);
      invalid.BindOutput("present_value", memory);
      std::fill(cache.layers[0].present_key.begin(), cache.layers[0].present_key.end(), Value<T>(99));
      EXPECT_THROW(session.Run(Ort::RunOptions{nullptr}, invalid), Ort::Exception);
      EXPECT_EQ(past, cache.seq_len());
      EXPECT_EQ(0, std::memcmp(expectedKey.data(), cache.key_data(0), pastCount * sizeof(T)));
      EXPECT_EQ(0, std::memcmp(expectedValue.data(), cache.value_data(0), pastCount * sizeof(T)));
    }
  }
};

TEST_P(LlmKVCacheTest, Float32MatchesIndependentReference) { Decode<float>("fp32"); }
TEST_P(LlmKVCacheTest, Float16MatchesIndependentReference) { Decode<Ort::Float16_t>("fp16"); }
TEST_P(LlmKVCacheTest, Int8MatchesIndependentReference) { Decode<int8_t>("int8"); }
INSTANTIATE_TEST_SUITE_P(Layouts, LlmKVCacheTest, ::testing::Values("bshd", "bhsd", "bhds"));

TEST(LlmKVCache, InvalidCapacityDoesNotPublishAnyLayer) {
  KVCacheManager<float> cache;
  cache.init(2, 8, 64, 32);
  cache.commit_present(17);
  float *key = cache.key_data(0);
  EXPECT_THROW(cache.commit_present(33), std::out_of_range);
  EXPECT_THROW(cache.commit_present(0), std::out_of_range);
  EXPECT_THROW(cache.commit_present(-1), std::out_of_range);
  // Validate every layer before swapping any of them.
  cache.layers[1].present_value.resize(1);
  EXPECT_THROW(cache.commit_present(18), std::out_of_range);
  EXPECT_EQ(key, cache.key_data(0));
  EXPECT_EQ(17, cache.layer_seq(0));
  EXPECT_EQ(17, cache.layer_seq(1));
  cache.init(2, 8, 64, 64);
  EXPECT_EQ(0, cache.seq_len());
  EXPECT_EQ(64u * 8 * 64, cache.layers[1].present_value.size());
}
}  // namespace
