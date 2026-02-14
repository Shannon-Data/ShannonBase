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
#include <variant>
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

  float temperature = 0.7f;  // Sampling temperature (0.0-2.0)
  int max_tokens = 100;      // Maximum tokens to generate
  int top_k = 50;            // Top-k sampling
  float top_p = 0.9f;        // Nucleus sampling

  float repeat_penalty = 1.0f;     // Repetition penalty
  float frequency_penalty = 0.0f;  // Frequency penalty
  float presence_penalty = 0.0f;   // Presence penalty

  std::vector<std::string> stop_sequences;  // Stop sequences
  bool speculative_decoding = false;        // Speculative decoding
  std::string image_base64 = "";            // Base64 encoded image

  void setLanguage(const std::string &lang) {
    std::string lower_lang = lang;
    std::transform(lower_lang.begin(), lower_lang.end(), lower_lang.begin(), ::tolower);

    static const std::unordered_map<std::string, std::string> languageMap = {
        {"en", "english"},
        {"english", "english"},
        {"zh", "chinese"},
        {"cn", "chinese"},
        {"chinese", "chinese"},
        {"es", "spanish"},
        {"spanish", "spanish"},
        {"fr", "french"},
        {"french", "french"},
        {"de", "german"},
        {"german", "german"},
        {"ja", "japanese"},
        {"japanese", "japanese"},
        {"ko", "korean"},
        {"korean", "korean"},
        {"ru", "russian"},
        {"russian", "russian"},
        {"ar", "arabic"},
        {"arabic", "arabic"},
        {"pt", "portuguese"},
        {"portuguese", "portuguese"},
        {"it", "italian"},
        {"italian", "italian"},
        {"nl", "dutch"},
        {"dutch", "dutch"},
        {"hi", "hindi"},
        {"hindi", "hindi"},
        {"tr", "turkish"},
        {"turkish", "turkish"},
        {"vi", "vietnamese"},
        {"vietnamese", "vietnamese"},
        {"th", "thai"},
        {"thai", "thai"},
        {"id", "indonesian"},
        {"indonesian", "indonesian"},
        {"pl", "polish"},
        {"polish", "polish"},
        {"uk", "ukrainian"},
        {"ukrainian", "ukrainian"},
        // more....
    };

    auto it = languageMap.find(lower_lang);
    if (it != languageMap.end()) {
      language = it->second;
    } else {
      language = lang;
    }
  }

  void setModelDefaults(const std::string &model_id) {
    this->model_id = model_id;

    std::string lower_model = model_id;
    std::transform(lower_model.begin(), lower_model.end(), lower_model.begin(), ::tolower);

    // default
    temperature = 0.7f;
    max_tokens = 512;
    top_k = 50;
    top_p = 0.9f;
    repeat_penalty = 1.1f;
    frequency_penalty = 0.0f;
    presence_penalty = 0.0f;
    stop_sequences.clear();

    // recommended params value.
    if (lower_model.find("qwen") != std::string::npos) {
      temperature = 0.8f;  // Qwen
      max_tokens = 50;
      top_k = 50;
      top_p = 0.95f;
      repeat_penalty = 1.05f;
      stop_sequences = {"<|im_end|>"};
      if (language == "chinese") {
        temperature = 0.6f;  // More stable for Chinese Gen.
        max_tokens = 100;
      }
    } else if (lower_model.find("llama") != std::string::npos) {
      temperature = 0.7f;
      max_tokens = 1024;
      top_k = 40;
      top_p = 0.95f;
      repeat_penalty = 1.1f;
      stop_sequences = {"</s>", "<|eot_id|>"};
    } else if (lower_model.find("mistral") != std::string::npos) {
      temperature = 0.5f;  // Mistral lower is better
      max_tokens = 1024;
      top_p = 0.9f;
      repeat_penalty = 1.15f;
      stop_sequences = {"</s>", "[INST]", "[/INST]"};
    } else if (lower_model.find("phi") != std::string::npos) {
      temperature = 0.6f;
      max_tokens = 512;
      top_p = 0.88f;
      repeat_penalty = 1.08f;
      stop_sequences = {"<|endoftext|>"};
    } else if (lower_model.find("gemma") != std::string::npos) {
      temperature = 0.7f;
      max_tokens = 1024;
      top_p = 0.9f;
      repeat_penalty = 1.1f;
      stop_sequences = {"<end_of_turn>", "<eos>"};
    } else if (lower_model.find("chatglm") != std::string::npos) {
      temperature = 0.5f;
      max_tokens = 1024;
      top_p = 0.85f;
      repeat_penalty = 1.05f;
      stop_sequences = {"<|endoftext|>"};
      if (language == "chinese") {
        temperature = 0.4f;
        max_tokens = 512;
      }
    } else if (lower_model.find("yi") != std::string::npos) {
      temperature = 0.3f;
      max_tokens = 2048;
      top_p = 0.85f;
      repeat_penalty = 1.05f;
      stop_sequences = {"<|im_end|>", "<|im_start|>"};
    }

    if (task == "summarization") {
      temperature = 0.3f;     // more stability
      max_tokens = 300;       // abstract is shorter
      repeat_penalty = 1.2f;  // strong punishment for abs.
      if (language == "chinese") {
        max_tokens = 200;  // more shorter for Chinese.
      } else if (language == "japanese") {
        max_tokens = 250;
      }
    }
  }

  void optimizeForModelSize() {
    std::string lower_model = model_id;
    std::transform(lower_model.begin(), lower_model.end(), lower_model.begin(), ::tolower);

    if (lower_model.find("0.5b") != std::string::npos || lower_model.find("1b") != std::string::npos ||
        lower_model.find("1.5b") != std::string::npos) {  // small model.
      temperature = std::min(temperature, 0.1f);
      max_tokens = std::max(max_tokens, 512);
      top_k = std::min(top_k, 10);
      repeat_penalty = std::min(repeat_penalty, 1.15f);
    } else if (lower_model.find("70b") != std::string::npos || lower_model.find("72b") != std::string::npos) {
      temperature = std::min(temperature + 0.1f, 1.0f);
      max_tokens = std::max(max_tokens, 2048);
    }
  }

  bool validate() const {
    return temperature >= 0.0f && temperature <= 5.0f && max_tokens > 0 && max_tokens <= 8192 && top_k >= 0 &&
           top_k <= 1000 && top_p >= 0.0f && top_p <= 1.0f && repeat_penalty > 0.0f && repeat_penalty <= 2.0f &&
           frequency_penalty >= -2.0f && frequency_penalty <= 2.0f && presence_penalty >= -2.0f &&
           presence_penalty <= 2.0f && (task == "generation" || task == "summarization");
  }

} GenerationOptions;

struct ModelConfig {
  size_t num_layers = 0;       // 0 means auto dective
  size_t num_query_heads = 0;  // 0 means auto dective
  size_t num_kv_heads = 0;     // 0 means auto dective
  size_t head_dim = 0;         // 0 means auto dective
  std::string attention_type;  // "standard" or "gqa"
};

// Key/Value a layer structure：[SeqLen, Heads * HeadDim]
template <typename T>
using layer_cache_t = std::vector<T>;

// KV entire structure：[NumLayers, [SeqLen, Heads * HeadDim]]
template <typename T>
using full_cache_t = std::vector<layer_cache_t<T>>;

// 2. using std::variant to support different types data.
using cache_data_t = std::variant<full_cache_t<float>,          /*FP32*/
                                  full_cache_t<Ort::Float16_t>, /*FP16*/
                                  full_cache_t<int8_t>,         /*INT8/QINT8*/
                                  full_cache_t<int64_t>         /*INT64/QINT64*/
                                  >;
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
   * @param systemPrompt Raw system prompt
   * @return Formatted prompt with appropriate chat template
   */
  std::string ApplyChatTemplate(const std::string &userInput, const std::string &);

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

  // About the KV cache.
  void InitializeKVCache();
  void ClearKVCache();

  inline void KVCache_Reset() {
    ClearKVCache();
    m_shouldClearKVCache = true;
  }
  /**
   * Parse KV cache input tensor name to extract layer index and key/value type
   * @param name Input tensor name (e.g., "past_key_values.5.key")
   * @return Pair containing (layer_index, is_key_tensor)
   *         - layer_index: Which transformer layer this cache belongs to (0-based)
   *         - is_key_tensor: true if this is a key tensor, false if value tensor
   * @note Critical for proper KV cache management - wrong parsing corrupts attention
   */
  std::pair<size_t, bool> ParseKVCacheInputName(const std::string &name) const;

  /**
   * Parse KV cache output tensor name to extract layer index and key/value type
   * @param name Output tensor name (e.g., "present_key_values.12.value")
   * @return Pair containing (layer_index, is_key_tensor)
   *         - layer_index: Which transformer layer this cache belongs to (0-based)
   *         - is_key_tensor: true if this is a key tensor, false if value tensor
   * @note Used to update internal cache from model outputs after each forward pass
   */
  std::pair<size_t, bool> ParseKVCacheOutputName(const std::string &name) const;

  /**
   * Update internal KV cache with new key/value states from model output
   * @param outputTensors Vector of output tensors from ONNX model inference
   * @param outputNames Corresponding names for each output tensor
   * @param memInfo Memory info for tensor operations
   * @note Extracts key/value tensors from outputs and stores them for next iteration
   *       This enables efficient incremental generation by reusing past computations
   */
  void UpdateKVCache(const std::vector<Ort::Value> &outputTensors, const std::vector<std::string> &outputNames,
                     const Ort::MemoryInfo &memInfo);

  /**
   * Calculate total number of elements in a tensor given its shape
   * @param shape Vector containing tensor dimensions [batch, seq_len, hidden_dim, ...]
   * @return Total number of elements (product of all dimensions)
   * @note Used for memory allocation and validation of tensor operations
   */
  size_t GetElementCount(const std::vector<int64_t> &shape) const;

  /**
   * Validate that tensor buffer size matches expected size based on tensor shape
   * @param tensor ONNX tensor to validate
   * @param buffer Pointer to data buffer
   * @param bufferSize Size of buffer in bytes
   * @return true if buffer size matches tensor requirements, false otherwise
   * @note Prevents buffer overflows and ensures memory safety during tensor operations
   */
  bool ValidateTensorBufferSize(const Ort::Value &tensor, const void *buffer, size_t bufferSize);

  /**
   * Initialize token frequency and presence tracking arrays
   * @param vocabSize Size of the model's vocabulary
   * @note Sets up internal arrays to track token usage for penalty calculations
   *       Must be called before generation starts
   */
  void InitializeTokenTracking(size_t vocabSize);

  /**
   * Update token frequency and presence counters for penalty calculations
   * @param token Token ID that was just generated
   * @note Increments frequency count and marks token as present
   *       Used by frequency/presence penalties to avoid repetition
   */
  void UpdateTokenTracking(int64_t token);

  /**
   * Sample next token using temperature-based probability distribution
   * @param logits Array of raw logit scores from model output
   * @param vocabSize Size of vocabulary (length of logits array)
   * @param temperature Sampling temperature (0.0 = greedy, higher = more random)
   * @return Selected token ID
   * @note Higher temperature increases randomness, lower temperature is more deterministic
   *       Temperature of 0.0 performs greedy selection (argmax)
   */
  int64_t SampleWithTemperature(const float *logits, size_t vocabSize, float temperature);

  /**
   * Sample next token using Top-K sampling with temperature
   * @param logits Array of raw logit scores from model output
   * @param vocabSize Size of vocabulary (length of logits array)
   * @param topK Number of highest probability tokens to consider
   * @param temperature Sampling temperature for selected top-K tokens
   * @return Selected token ID from top-K candidates
   * @note Restricts sampling to K most likely tokens, then applies temperature sampling
   *       Helps prevent selecting very unlikely tokens while maintaining diversity
   */
  int64_t SampleTopK(const float *logits, size_t vocabSize, int topK, float temperature);

  /**
   * Sample next token using Top-P (nucleus) sampling with temperature
   * @param logits Array of raw logit scores from model output
   * @param vocabSize Size of vocabulary (length of logits array)
   * @param topP Cumulative probability threshold (0.0-1.0)
   * @param temperature Sampling temperature for selected tokens
   * @return Selected token ID from nucleus set
   * @note Dynamically selects tokens whose cumulative probability reaches topP
   *       More flexible than top-K as it adapts to the actual probability distribution
   */
  int64_t SampleTopP(const float *logits, size_t vocabSize, float topP, float temperature);

  /**
   * Apply repetition penalty to logits based on previously generated tokens
   * @param logits Array of logit scores to modify (modified in-place)
   * @param vocabSize Size of vocabulary (length of logits array)
   * @param generatedTokens Vector of all tokens generated so far in this sequence
   * @param penalty Repetition penalty factor (>1.0 reduces repetition, <1.0 encourages it)
   * @note Penalizes tokens that appeared in the generated sequence
   *       If logit > 0: divide by penalty, if logit < 0: multiply by penalty
   */
  void ApplyRepeatPenalty(float *logits, size_t vocabSize, const std::vector<int64_t> &generatedTokens, float penalty);

  /**
   * Apply frequency penalty to logits based on token occurrence frequency
   * @param logits Array of logit scores to modify (modified in-place)
   * @param vocabSize Size of vocabulary (length of logits array)
   * @param penalty Frequency penalty factor (positive reduces frequent tokens)
   * @note Subtracts penalty * frequency_count from each token's logit
   *       More frequent tokens get larger penalties, promoting vocabulary diversity
   */
  void ApplyFrequencyPenalty(float *logits, size_t vocabSize, float penalty);

  /**
   * Apply presence penalty to logits based on whether tokens appeared before
   * @param logits Array of logit scores to modify (modified in-place)
   * @param vocabSize Size of vocabulary (length of logits array)
   * @param penalty Presence penalty factor (positive reduces repeated tokens)
   * @note Subtracts penalty from logits of any token that appeared at least once
   *       Binary penalty (unlike frequency) - same penalty regardless of count
   */
  void ApplyPresencePenalty(float *logits, size_t vocabSize, float penalty);

  // get the stopwords template by model type.
  std::vector<std::string> GetModelSpecificStopTokens(const std::string &modelType);

  // check stop words to decide we should stop inference or not.
  bool ShouldStop(const std::vector<int64_t> &tokens, const std::vector<std::string> &stopSequences);

  // Test function for tokenizer. Helper function.
  void TestTokenizerCompatibility();

  // A helper function.
  void PrintTopKLogits(const std::vector<float> &logits, int top_k) const;

  template <typename CacheT, typename SourceT>
  void updateLayerCache(full_cache_t<CacheT> &fullCache, size_t layerIdx, const SourceT *data, size_t elementCount);

  /**
   * @brief create a zero-elem(empty) ORT tensor.
   * @param type ONNX data type (m_cacheDataType).
   * @param shape shape of tensor（[1, num_heads, 0, head_dim]).
   * @param memInfo
   * @return Ort::Value zero-elem tesnor.
   */
  Ort::Value CreateZeroCacheTensor(ONNXTensorElementDataType type, const std::vector<int64_t> &shape,
                                   const Ort::MemoryInfo &memInfo);

  /**
   * @brief from cache_data_t get the Nth-layer data，and create ORT tensor as input.
   * @param cache (cache_data_t).
   * @param layerIdx layer index.
   * @param shape tensor shape（[1, num_heads, seq_len, head_dim]).
   * @param memInfo
   * @return Ort::Value ORT input tensor.
   */
  Ort::Value CreateInputCacheTensor(cache_data_t &cache, size_t layerIdx, const std::vector<int64_t> &shape,
                                    const Ort::MemoryInfo &memInfo);

 private:
  // system prompt string.
  std::string m_system_prompt{"You are an AI assistant that provides clear and concise explanations in "};

  // the last prompt string user input.
  std::string m_lastPrompt;

  // error message.
  std::string m_error_string;

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
  static constexpr size_t default_vocab_size = 5000;

  // initialized or not.
  bool m_initialized = false;

  std::unique_ptr<Ort::Env> m_env;
  std::unique_ptr<Ort::SessionOptions> m_sessionOptions;
  std::unique_ptr<Ort::Session> m_session;

  // tokenizer, tokens loaded from the `tokenizer.json`
  std::shared_ptr<tokenizers::Tokenizer> m_tokenizer;

  // kv-cache
  ONNXTensorElementDataType m_cacheDataType = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  cache_data_t m_keyCache;
  cache_data_t m_valueCache;

  bool m_kvCacheInitialized = false;
  bool m_shouldClearKVCache = true;

  std::vector<std::vector<float>> m_stepFloatBuffers;
  std::vector<std::vector<int64_t>> m_stepInt64Buffers;
};
}  // namespace LLM_Generate
}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_LLM_GENERATE_H__