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

   The fundmental code for imcs. Using `Llama-3.2-3B-Instruct` to generate
   response text.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/

#ifndef __SHANNONBASE_RAPID_LLM_GENERATE_H__
#define __SHANNONBASE_RAPID_LLM_GENERATE_H__

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
#include <thread>
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
namespace LLM_Generate {
namespace fs = std::filesystem;

enum class Precision { FP32, FP16, INT8, QINT4, UNKNOWN };
enum class Device { CPU, GPU };

struct ModelSelection {
  std::string filename;
  Precision precision;
  Device device;
  std::string variant;
  double estimated_memory_gb;
};

class CPUDetector {
 public:
  enum class ModelType {
    ARM64_QINT8,
    AVX512_VNNI_QINT8,
    AVX512F_QINT8,
    AVX2_QUINT8,
    BASELINE_FP32,
    FP16_OPTIMIZED,
    QINT4_OPTIMIZED,
    QINT4_FP16_HYBRID
  };

  CPUDetector() noexcept { detectFeatures(); }

  bool hasAVX2() const noexcept { return m_features.avx2; }
  bool hasAVX512F() const noexcept { return m_features.avx512f; }
  bool hasAVX512VNNI() const noexcept { return m_features.avx512vnni; }
  bool hasAVX512BW() const noexcept { return m_features.avx512bw; }
  bool hasAVX512DQ() const noexcept { return m_features.avx512dq; }
  bool isARM64() const noexcept { return m_features.arm64; }
  bool hasFMA() const noexcept { return m_features.fma; }
  bool hasFP16() const noexcept { return m_features.fp16; }

  std::string getCPUInfo() const;

 private:
  struct CPUFeatures {
    bool avx2 = false;
    bool avx512f = false;
    bool avx512vnni = false;
    bool avx512bw = false;
    bool avx512dq = false;
    bool arm64 = false;
    bool fma = false;
    bool fp16 = false;
  } m_features;

  void cpuid(std::array<int, 4> &regs, int funcId, int subFuncId = 0) const noexcept;
  void detectFeatures() noexcept;
};

ModelSelection select_model_variant(const std::string &model_dir, const std::string &user_precision = "",
                                    const std::string &user_variant = "");

typedef struct {
  std::string task = "generation";  // 'generation' or 'summarization'
  std::string model_id = "";        // Model identifier, llma, mistral, phi, etc.
  std::string context = "";         // Additional context
  std::string language = "en";      // Language setting

  float temperature = 1.0f;  // Sampling temperature (0.0-2.0)
  int max_tokens = 100;      // Maximum tokens to generate
  int top_k = 50;            // Top-k sampling
  float top_p = 0.9f;        // Nucleus sampling

  float repeat_penalty = 1.0f;     // Repetition penalty
  float frequency_penalty = 0.0f;  // Frequency penalty
  float presence_penalty = 0.0f;   // Presence penalty

  std::vector<std::string> stop_sequences;  // Stop sequences
  bool speculative_decoding = false;        // Speculative decoding
  std::string image_base64 = "";            // Base64 encoded image

  bool validate() const {
    return temperature >= 0.0f && temperature <= 5.0f && max_tokens > 0 && max_tokens <= 4096 && top_k >= 0 &&
           top_p >= 0.0f && top_p <= 1.0f && repeat_penalty > 0.0f && (task == "generation" || task == "summarization");
  }

} GenerationOptions;

struct ModelConfig {
  size_t num_layers = 0;       // 0 means auto dective
  size_t num_query_heads = 0;  // 0 means auto dective
  size_t num_kv_heads = 0;     // 0 means auto dective
  size_t head_dim = 0;         // 0 means auto dective
  std::string attention_type;  // "standard" or "gqa"
};

class TextGenerator {
 public:
  struct Result {
    std::string output;
    std::vector<int64_t> tokens;
  };

  TextGenerator(const std::string &modelPath, const std::string &tokenizerPath, const GenerationOptions &option);
  virtual ~TextGenerator();

  inline bool Initialized() const { return m_initialized; }

  inline void Reset() {
    ClearKVCache();
    m_shouldClearKVCache = true;
  }

  /**
   * Generate text based on user prompt
   * @param userPrompt The input prompt from user
   * @param maxNewTokens Maximum number of new tokens to generate
   * @return Result containing generated text and token sequence
   */
  Result Generate(const std::string &userPrompt, int maxNewTokens = 128);

  void SetVocabularySize(size_t size) { m_vocabularySize = size; }

  size_t GetMaxSequenceLength() const;

 private:
  /**
   * Initialize ONNX Runtime session
   */
  bool InitializeONNX();

  /**
   * Initialize the tokenizer from file
   */
  bool InitializeTokenizer();
  /**
   * Load tokenizer configuration to get special token IDs
   */

  bool LoadTokenizerConfig();

  /**
   * Apply chat template based on model type
   * @param userInput Raw user input
   * @return Formatted prompt with appropriate chat template
   */
  std::string ApplyChatTemplate(const std::string &userInput);

  /**
   * Find the index of maximum value in logits array (greedy sampling)
   * @param logits Array of logit values
   * @param vocabSize Size of vocabulary
   * @return Index of maximum value
   */
  int64_t Argmax(const float *data, size_t size);

  // tools to do model input analysis.
  void AnalyzeModelInputShapes();

  // normailze model type by model full name.
  std::string NormalizeModelType(const std::string &modelType) const;

  // the meta data of model.
  void GetModelMetadata();

  // get the heades info from output.
  void DetectQueryHeadsFromOutputs(const std::vector<std::string> &outputNames);

  // Get model params. from model configure from `m_session`, `m_sessionOptions`.
  void DetectModelArchitecture(const std::vector<std::string> &inputNames, const std::vector<std::string> &outputNames);

  void InitializeKVCache();
  void ClearKVCache();

  std::pair<size_t, bool> ParseKVCacheInputName(const std::string &name) const;
  std::pair<size_t, bool> ParseKVCacheOutputName(const std::string &name) const;

  void UpdateKVCache(const std::vector<Ort::Value> &outputTensors, const std::vector<std::string> &outputNames,
                     const Ort::MemoryInfo &memInfo);

  size_t GetElementCount(const std::vector<int64_t> &shape) const;

  bool ValidateTensorBufferSize(const Ort::Value &tensor, const void *buffer, size_t bufferSize);

  void InitializeTokenTracking(size_t vocabSize);
  void UpdateTokenTracking(int64_t token);

  int64_t SampleWithTemperature(const float *logits, size_t vocabSize, float temperature);
  int64_t SampleTopK(const float *logits, size_t vocabSize, int topK, float temperature);
  int64_t SampleTopP(const float *logits, size_t vocabSize, float topP, float temperature);

  // Penalty
  void ApplyRepeatPenalty(float *logits, size_t vocabSize, const std::vector<int64_t> &generatedTokens, float penalty);
  void ApplyFrequencyPenalty(float *logits, size_t vocabSize, float penalty);
  void ApplyPresencePenalty(float *logits, size_t vocabSize, float penalty);

  bool ShouldStop(const std::vector<int64_t> &tokens, const std::vector<std::string> &stopSequences);

 private:
  // Generation Options.
  GenerationOptions m_gen_option;

  // End-of-Sequence Token ID
  int64_t m_eosTokenId = -1;

  // Beginning-of-Sequence Token ID
  int64_t m_bosTokenId = -1;

  // Padding Token ID
  int64_t m_padTokenId = -1;

  // # of Transformer
  size_t m_numLayers = 0;

  //# of Query Attention Heads
  size_t m_numQueryHeads = 0;

  //# of Key/Value Attention Heads
  size_t m_numKVHeads = 0;

  // # of header dim.
  size_t m_headDim = 0;

  // model path
  std::string m_modelPath;

  // tokenizer.json path
  std::string m_tokenizerPath;

  // model type: llma-3b, mistral, phi, etc.
  std::string m_modelType;

  // token Frequency
  std::vector<int64_t> m_tokenFrequency;

  // token Presence
  std::vector<int64_t> m_tokenPresence;

  // Vocabulary Size
  size_t m_vocabularySize = 0;

  // initialized or not.
  bool m_initialized = false;

  std::unique_ptr<Ort::Env> m_env;
  std::unique_ptr<Ort::SessionOptions> m_sessionOptions;
  std::unique_ptr<Ort::Session> m_session;

  // tokenizer, tokens loaded from the `tokenizer.json`
  std::shared_ptr<tokenizers::Tokenizer> m_tokenizer;

  // kv-cache
  std::vector<std::vector<float>> m_keyCache;
  std::vector<std::vector<float>> m_valueCache;
  bool m_kvCacheInitialized = false;
  bool m_shouldClearKVCache = true;

  std::vector<std::vector<float>> m_stepFloatBuffers;
  std::vector<std::vector<int64_t>> m_stepInt64Buffers;
};

}  // namespace LLM_Generate
}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_LLM_GENERATE_H__