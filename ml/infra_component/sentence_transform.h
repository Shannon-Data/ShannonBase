/**
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/

#ifndef __SHANNONBASE_RAPID_SENTENCE_TRANSFORM_H__
#define __SHANNONBASE_RAPID_SENTENCE_TRANSFORM_H__

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numeric>
#include <regex>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "ml/infra_component/tokenizer.h"

#if defined(_WIN32)
#include <intrin.h>
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif

namespace ShannonBase {
namespace ML {
namespace SentenceTransform {

namespace fs = std::filesystem;
enum class Precision { FP32, INT8, UNKNOWN };
enum class Device { CPU, GPU };

struct ModelSelection {
  std::string filename;
  Precision precision;
  Device device;
  std::string opt_level;  // O1/O2/O3/O4/auto
};

class CPUDetector {
 public:
  enum class ModelType { ARM64_QINT8, AVX512_VNNI_QINT8, AVX512F_QINT8, AVX2_QUINT8, BASELINE_FP32 };

  CPUDetector() noexcept { detectFeatures(); }

  bool hasAVX2() const noexcept { return m_features.avx2; }
  bool hasAVX512F() const noexcept { return m_features.avx512f; }
  bool hasAVX512VNNI() const noexcept { return m_features.avx512vnni; }
  bool hasAVX512BW() const noexcept { return m_features.avx512bw; }
  bool hasAVX512DQ() const noexcept { return m_features.avx512dq; }
  bool isARM64() const noexcept { return m_features.arm64; }
  bool hasFMA() const noexcept { return m_features.fma; }

  std::string getCPUInfo() const {
    std::string info = "CPU Features: ";
    if (m_features.arm64) info += "ARM64 ";
    if (m_features.avx2) info += "AVX2 ";
    if (m_features.avx512f) info += "AVX512F ";
    if (m_features.avx512vnni) info += "AVX512-VNNI ";
    if (m_features.avx512bw) info += "AVX512BW ";
    if (m_features.avx512dq) info += "AVX512DQ ";
    if (m_features.fma) info += "FMA ";
    return info.empty() ? "CPU Features: None detected" : info;
  }

 private:
  struct CPUFeatures {
    bool avx2 = false;
    bool avx512f = false;
    bool avx512vnni = false;
    bool avx512bw = false;
    bool avx512dq = false;
    bool arm64 = false;
    bool fma = false;
  } m_features;

  void cpuid(std::array<int, 4> &regs, int funcId, int subFuncId = 0) const noexcept {
#if defined(_MSC_VER)
    __cpuidex(regs.data(), funcId, subFuncId);
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    unsigned int eax, ebx, ecx, edx;
    __cpuid_count(funcId, subFuncId, eax, ebx, ecx, edx);
    regs[0] = static_cast<int>(eax);
    regs[1] = static_cast<int>(ebx);
    regs[2] = static_cast<int>(ecx);
    regs[3] = static_cast<int>(edx);
#else
    regs.fill(0);
#endif
  }

  void detectFeatures() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    m_features.arm64 = true;
    return;
#endif

    std::array<int, 4> regs{};

    cpuid(regs, 0, 0);
    const int maxLeaf = regs[0];
    if (maxLeaf < 1) return;

    cpuid(regs, 1, 0);
    const int ecx1 = regs[2];

    const bool avx = (ecx1 & (1 << 28)) != 0;
    m_features.fma = (ecx1 & (1 << 12)) != 0;

    if (maxLeaf >= 7) {
      cpuid(regs, 7, 0);
      const int ebx7 = regs[1];
      const int ecx7 = regs[2];

      m_features.avx2 = avx && ((ebx7 & (1 << 5)) != 0);
      m_features.avx512f = (ebx7 & (1 << 16)) != 0;
      m_features.avx512bw = (ebx7 & (1 << 30)) != 0;
      m_features.avx512dq = (ebx7 & (1 << 17)) != 0;
      m_features.avx512vnni = (ecx7 & (1 << 11)) != 0;
    }
  }
};

ModelSelection select_model_variant(const std::string &model_dir, const std::string &user_precision = "",
                                    const std::string &user_opt = "");

enum class STATUS_T {
  OK = 0,
  ERROR_INVALID_INPUT = 1,         // invalid input（such as blank text or tokens）
  ERROR_MODEL_NOT_INIT = 2,        // model /session not initialized
  ERROR_ONNX_INFERENCE_FAIL = 3,   // ONNX Runtime inference failed.
  ERROR_OUTPUT_TENSOR_EMPTY = 4,   // ONNX result empty.
  ERROR_OUTPUT_SHAPE_INVALID = 5,  // ONNX tensor shape invalid
  ERROR_TOKENIZER_FAIL = 6         // tokenizer init failed or emcoding failed.
};

class MiniLMEmbedding {
 public:
  // 384 dimensions，all-MiniLM-L6-v2 # of output demensions.
  using EmbeddingVector = std::vector<float>;

  struct EmbeddingResult {
    std::string text;
    EmbeddingVector embedding;
    double confidence = 0.0;
  };

  MiniLMEmbedding(const std::string &modelPath, const std::string &tokenizerPath = "");
  ~MiniLMEmbedding() = default;

  // single file embedding
  EmbeddingResult EmbedText(const std::string &text);

  // file embedding by segement.
  std::vector<EmbeddingResult> EmbedFile(const std::string &filePath, size_t maxChunkSize = 512);

  // batch embedding files
  std::vector<EmbeddingResult> EmbedBatch(const std::vector<std::string> &texts);

  // calculate cosin similarity.
  static double CosineSimilarity(const EmbeddingVector &a, const EmbeddingVector &b);

  std::vector<std::pair<size_t, double>> SemanticSearch(const EmbeddingVector &queryEmbedding,
                                                        const std::vector<EmbeddingResult> &corpus, size_t topK = 5);

 private:
  void InitializeONNX();

  STATUS_T Tokenize(const std::string &text, tokenizers::Tokenizer::Encoding &encoding_res) const;

  STATUS_T RunInference(const std::vector<int64_t> &input_ids, const std::vector<int64_t> &attention_mask,
                        const std::vector<int64_t> &token_type_ids, EmbeddingVector &embeded_res);

  void NormalizeL2(EmbeddingVector &vec) {
    double norm = std::sqrt(std::inner_product(vec.begin(), vec.end(), vec.begin(), 0.0));
    if (norm > 0) {
      for (float &val : vec) {
        val /= norm;
      }
    }
  }

  std::vector<std::string> ReadAndChunkFile(const std::string &filePath, size_t maxChunkSize);

 private:
  std::string m_modelPath;
  std::string m_tokenizerPath;
  std::unique_ptr<tokenizers::Tokenizer> m_tokenizer;

  std::unique_ptr<Ort::Env> m_ortEnv;
  std::unique_ptr<Ort::Session> m_ortSession;
  std::unique_ptr<Ort::SessionOptions> m_sessionOptions;
  std::vector<std::string> m_inputNames;
  std::vector<std::string> m_outputNames;
};

class DocumentEmbeddingManager {
 public:
  DocumentEmbeddingManager(const std::string &modelPath, const std::string &tokenizer)
      : m_embedder(modelPath, tokenizer) {}

  // process a whole document
  void ProcessDocument(const std::string &filePath);

  // process a string. success return false, error returns true.
  bool ProcessText(const std::string &text, size_t maxChunkSize = 512);

  // search a text, returns a vector of (text, similarity).
  std::vector<std::pair<std::string, double>> SemanticSearch(const std::string &query, size_t topK = 3);

  // saving the result to a file.
  void SaveEmbeddings(const std::string &outputPath);

  // gets the processing result, after `ProcessDocument` or `ProcessText` be done.
  inline std::vector<MiniLMEmbedding::EmbeddingResult> &Results() { return m_documentEmbeddings; }

 private:
  std::vector<std::string> SplitTextIntoChunks(const std::string &text, size_t maxChunkSize);

 private:
  MiniLMEmbedding m_embedder;
  std::vector<MiniLMEmbedding::EmbeddingResult> m_documentEmbeddings;
};

}  // namespace SentenceTransform
}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_SENTENCE_TRANSFORM_H__