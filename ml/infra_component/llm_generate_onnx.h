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

   The fundamental code for LLM inference.
   Copyright (c) 2023 - , Shannon Data AI and/or its affiliates.
*/

#ifndef __SHANNONBASE_RAPID_LLM_GENERATE_ONNX_H__
#define __SHANNONBASE_RAPID_LLM_GENERATE_ONNX_H__

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <onnxruntime_cxx_api.h>
#include "ml/infra_component/llm_generate.h"
#include "ml/infra_component/llm_kv_cache.h"
#include "ml/infra_component/tokenizer.h"

namespace ShannonBase {
namespace ML {
namespace LLM_Generate {

template <typename T>
using layer_cache_t = std::vector<T>;

template <typename T>
using full_cache_t = std::vector<layer_cache_t<T>>;

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

  enum class InputMode {
    INPUT_IDS,      // standard: int64 token ids  → Qwen2.5-0.5B, LLaMA etc.
    INPUTS_EMBEDS,  // hybrid : float embeddings  → Qwen3.5-9B hybrid etc.
  };

  enum class KVCacheLayout {
    UNKNOWN,
    BHSD,  // [batch, heads, seq, head_dim]  ← Optimum
    BSHD,  // [batch, seq, heads, head_dim]  ← ONNX GenAI / Phi-3
    BHDS,  // [batch, heads, head_dim, seq]  ← old version encoder-decoder
  };

  struct OpaqueState {
    std::string in_name;   // model input name
    std::string out_name;  // corresponding model output name
    ONNXTensorElementDataType dtype;
    std::vector<int64_t> shape;  // concrete shape (batch dim substituted to 1)
    std::vector<uint8_t> data;   // raw byte storage (zero-initialised)
  };

  struct KVShapeInfo {
    TextGenerator::KVCacheLayout layout = KVCacheLayout::UNKNOWN;
    int dim_heads{-1};
    int dim_seq{-1};
    int dim_head_dim{-1};
    size_t num_heads{0};
    size_t head_dim{0};
  };

  TextGenerator(const std::string &modelPath, const std::string &tokenizerPath, const GenerationOptions &option);
  virtual ~TextGenerator();

  inline bool Initialized() const { return m_initialized; }
  inline void Reset() { ClearKVCache(); }

  Result Generate(const std::string &userPrompt, int maxNewTokens = 128);

  void SetVocabularySize(size_t size) { m_vocabularySize = size; }
  size_t GetMaxSequenceLength() const;

 private:
  bool InitializeONNX();
  bool InitializeTokenizer();
  bool LoadTokenizerConfig();

  tokenizers::Tokenizer::Encoding BuildPromptEncoding(const std::string &userInput,
                                                      const std::string &systemPromptOverride);

  void ValidatePromptFormat(const std::vector<uint32_t> &token_ids);
  int64_t Argmax(const float *data, size_t size);

  void AnalyzeModelInputShapes();
  void GetModelMetadata();
  void DetectQueryHeadsFromOutputs(const std::vector<std::string> &outputNames);
  void DetectModelArchitecture(const std::vector<std::string> &inputNames, const std::vector<std::string> &outputNames);

  void InitializeKVCache(int max_seq);
  void ClearKVCache();
  KVShapeInfo DetectKVShapeLayout(const std::vector<int64_t> &shape);
  inline void KVCache_Reset() { ClearKVCache(); }

  void NamesInitialized();
  std::pair<size_t, bool> ParseKVCacheInputName(const std::string &name) const;
  std::pair<size_t, bool> ParseKVCacheOutputName(const std::string &name) const;

  void UpdateKVCache(const std::vector<Ort::Value> &outputTensors, const std::vector<std::string> &outputNames,
                     const Ort::MemoryInfo &memInfo);
  size_t GetElementCount(const std::vector<int64_t> &shape) const;
  bool ValidateTensorBufferSize(const Ort::Value &tensor, const void *buffer, size_t bufferSize);

  inline void InitializeTokenTracking(size_t vocabSize) {
    m_tokenFrequency.assign(vocabSize, 0);
    m_tokenPresence.assign(vocabSize, 0);

    m_samplePairBuf.resize(vocabSize);
    m_sampleProbBuf.resize(vocabSize);
    m_sampleTempBuf.resize(vocabSize);
  }
  void UpdateTokenTracking(int64_t token);

  int64_t SampleWithTemperature(const float *logits, size_t vocabSize, float temperature);
  int64_t SampleTopK(const float *logits, size_t vocabSize, int topK, float temperature);
  int64_t SampleTopKThenTopP(const float *logits, size_t vocabSize, int topK, float topP, float temperature);
  int64_t SampleTopP(const float *logits, size_t vocabSize, float topP, float temperature);

  void ApplyRepeatPenalty(float *logits, size_t vocabSize, const std::vector<int64_t> &generatedTokens, float penalty);
  void ApplyFrequencyPenalty(float *logits, size_t vocabSize, float penalty);
  void ApplyPresencePenalty(float *logits, size_t vocabSize, float penalty);

  std::vector<std::string> GetModelSpecificStopTokens(const std::string &modelType);
  bool ShouldStop(const std::vector<int64_t> &tokens, const std::vector<std::string> &stopSequences);

  void TestTokenizerCompatibility();
  void PrintTopKLogits(const std::vector<float> &logits, int top_k) const;

  Ort::Value CreateZeroCacheTensor(ONNXTensorElementDataType type, const std::vector<int64_t> &shape,
                                   const Ort::MemoryInfo &memInfo);
  Ort::Value CreateInputCacheTensor(ONNXTensorElementDataType type, size_t layerIdx, bool isKey,
                                    const std::vector<int64_t> &shape, const Ort::MemoryInfo &memInfo);

  void DetectInputMode(const std::vector<std::string> &inputNames);
  bool LoadEmbeddingModel();
  std::vector<float> TokensToEmbeddings(const std::vector<int64_t> &ids);

  void InitOpaqueStates(const std::vector<std::string> &inputNames, const std::vector<std::string> &outputNames);
  Ort::Value BuildOpaqueStateTensor(const OpaqueState &st, const Ort::MemoryInfo &mem);
  void UpdateOpaqueStatesFromOutputs(Ort::IoBinding &binding, const std::vector<std::string> &outputNames,
                                     const Ort::MemoryInfo &mem);

 private:
  std::string m_system_prompt{"An AI assistant that provides clear and concise explanations."};
  std::string m_lastPrompt;
  std::string m_error_string;

  GenerationOptions m_gen_option;

  int64_t m_eosTokenId{-1};
  int64_t m_bosTokenId{-1};
  int64_t m_padTokenId{-1};

  size_t m_numLayers{0};
  size_t m_numQueryHeads{0};
  size_t m_numKVHeads{0};
  size_t m_headDim{0};

  std::string m_modelPath;
  std::string m_tokenizerPath;
  std::string m_modelType;

  std::vector<int64_t> m_stopTokenIds;
  std::vector<std::vector<int64_t>> m_stopTokenSequences;
  std::vector<int64_t> m_tokenFrequency;
  std::vector<int64_t> m_tokenPresence;

  size_t m_vocabularySize{0};
  static constexpr size_t default_vocab_size = 5000;

  bool m_initialized{false};

  std::unique_ptr<Ort::Env> m_env;
  std::unique_ptr<Ort::SessionOptions> m_sessionOptions;
  std::unique_ptr<Ort::Session> m_session;

  std::shared_ptr<tokenizers::Tokenizer> m_tokenizer;

  KVCacheLayout m_kvCacheLayout = KVCacheLayout::UNKNOWN;

  KVCacheManager<float> m_floatCache;
  KVCacheManager<Ort::Float16_t> m_fp16Cache;
  KVCacheManager<int8_t> m_int8Cache;
  ONNXTensorElementDataType m_cacheDataType{ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED};
  mutable std::unordered_map<std::string, std::pair<size_t, bool>> m_kvNameCache;

  template <typename T>
  KVCacheManager<T> *get_cache_manager();
  template <typename T>
  const KVCacheManager<T> *get_cache_manager() const;
  template <typename T>
  void debug_print_cache(const KVCacheManager<T> &cache, int layer, const char *name);

  bool m_kvCacheInitialized = false;

  std::vector<std::vector<float>> m_stepFloatBuffers;
  std::vector<std::vector<int64_t>> m_stepInt64Buffers;

  std::unique_ptr<Ort::IoBinding> m_ioBinding;

  std::vector<std::string> m_inputNames;
  std::vector<std::string> m_outputNames;
  bool m_namesInitialized{false};

  mutable std::vector<std::pair<float, int64_t>> m_samplePairBuf;
  mutable std::vector<float> m_sampleProbBuf;
  mutable std::vector<float> m_sampleTempBuf;
  mutable std::vector<float> m_logitsBuf;

  std::vector<int64_t> BuildKVShape(size_t seqLen) const;

  void BindKVCacheDirect(Ort::IoBinding &binding, const std::vector<std::string> &outputNames, size_t totalSeqLen,
                         const Ort::MemoryInfo &memInfo);
  void UpdateCacheSeqCounters(size_t totalSeqLen);
  int GetCurrentCacheSeq() const;
  int GetCurrentCacheMaxSeq() const;

  InputMode m_inputMode = InputMode::INPUT_IDS;
  size_t m_hiddenSize{0};

  std::unique_ptr<Ort::Session> m_embedSession;
  bool m_embedSessionLoaded{false};

  std::vector<OpaqueState> m_opaqueStates;

  std::set<size_t> m_attentionLayerIndices;

  int m_positionIdsDims{2};
};

}  // namespace LLM_Generate
}  // namespace ML
}  // namespace ShannonBase

#endif  // __SHANNONBASE_RAPID_LLM_GENERATE_ONNX_H__
