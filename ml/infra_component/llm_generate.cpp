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

#include "ml/infra_component/llm_generate.h"
#include "my_rapidjson_size_t.h"

#include <optional>
#include <random>
#include <set>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/rapidjson.h>
#include "include/my_dbug.h"
#include "include/mysql/components/services/log_builtins.h"  // LogErr
#include "include/mysqld_error.h"

namespace ShannonBase {
namespace ML {
namespace LLM_Generate {
std::mt19937 g_rng(std::random_device{}());

template <>
KVCacheManager<float> *TextGenerator::get_cache_manager<float>() {
  return (m_cacheDataType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) ? &m_floatCache : nullptr;
}

template <>
KVCacheManager<Ort::Float16_t> *TextGenerator::get_cache_manager<Ort::Float16_t>() {
  return (m_cacheDataType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) ? &m_fp16Cache : nullptr;
}

template <>
KVCacheManager<int8_t> *TextGenerator::get_cache_manager<int8_t>() {
  return (m_cacheDataType == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8) ? &m_int8Cache : nullptr;
}

template <>
const KVCacheManager<float> *TextGenerator::get_cache_manager<float>() const {
  return (m_cacheDataType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) ? &m_floatCache : nullptr;
}

template <>
const KVCacheManager<Ort::Float16_t> *TextGenerator::get_cache_manager<Ort::Float16_t>() const {
  return (m_cacheDataType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) ? &m_fp16Cache : nullptr;
}

template <>
const KVCacheManager<int8_t> *TextGenerator::get_cache_manager<int8_t>() const {
  return (m_cacheDataType == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8) ? &m_int8Cache : nullptr;
}

template <typename T>
void TextGenerator::debug_print_cache(const KVCacheManager<T> &cache, int layer, const char *name) {
  if (layer < cache.num_layers && cache.layer_seq(layer) > 0) {
    const T *data = cache.key_data(layer);
    size_t elements = cache.layer_elements(layer);
    if (elements >= 5) {
      fprintf(stderr, "[Debug] %s[%d] first 5: ", name, layer);
      for (int i = 0; i < 5; i++) {
        fprintf(stderr, "%f ", (float)data[i]);
      }
      fprintf(stderr, "\n");
    }
  }
}

// TextGenerator Implementation
TextGenerator::TextGenerator(const std::string &modelPath, const std::string &tokenizerPath,
                             const GenerationOptions &option)
    : m_gen_option(option), m_modelPath(modelPath), m_tokenizerPath(tokenizerPath), m_modelType(option.model_id) {
  try {
    auto ms = select_model_variant(m_modelPath);
    m_modelPath = (std::filesystem::path(m_modelPath) / ms.filename).string();
    bool failed = InitializeONNX() || InitializeTokenizer() || LoadTokenizerConfig();
    m_initialized = !failed;
  } catch (const Ort::Exception &e) {
    m_error_string = std::string("[ORT Exception] ") + e.what();
    DBUG_PRINT("error", ("ORT Exception during initialization: %s", e.what()));
    m_initialized = false;
  } catch (const std::exception &e) {
    m_error_string = std::string("[Exception] ") + e.what();
    DBUG_PRINT("error", ("Exception during initialization: %s", e.what()));
    m_initialized = false;
  } catch (...) {
    m_error_string = "[Unknown exception during initialization]";
    DBUG_PRINT("error", ("Unknown exception during initialization"));
    m_initialized = false;
  }
}

TextGenerator::~TextGenerator() {
  m_session.reset();
  m_sessionOptions.reset();
  m_env.reset();
}

bool TextGenerator::InitializeONNX() {
  m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "TextGenerator");
  m_sessionOptions = std::make_unique<Ort::SessionOptions>();

  int logicalCPUs = static_cast<int>(std::thread::hardware_concurrency());
  int intraThreads = std::max(1, logicalCPUs / 2);  // physical cores
  m_sessionOptions->SetIntraOpNumThreads(intraThreads);
  m_sessionOptions->SetInterOpNumThreads(1);  // decoder is sequential

  m_sessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  m_sessionOptions->SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

  m_sessionOptions->EnableMemPattern();
  m_sessionOptions->EnableCpuMemArena();
#ifdef SHANNON_WIN_PLATFORM
  int len = MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(), -1, nullptr, 0);
  std::wstring wmodel_path(len, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(), -1, wmodel_path.data(), len);
  m_session = std::make_unique<Ort::Session>(*m_env, wmodel_path.c_str(), *m_session_options);
#else
  m_session = std::make_unique<Ort::Session>(*m_env, m_modelPath.c_str(), *m_sessionOptions);
#endif
  if (!m_session) {
    m_error_string = "[ORT] Session creation returned null for: " + m_modelPath;
    return true;  // failed
  }
  return false;
}

bool TextGenerator::InitializeTokenizer() {
  auto token_path = std::filesystem::path(m_tokenizerPath) / "tokenizer.json";
  m_tokenizer = std::make_shared<tokenizers::Tokenizer>(token_path.string());
  if (!m_tokenizer->is_valid()) {
    m_error_string = m_tokenizer->get_last_error();
    m_error_string = "[ORT] Tokenizer creation returned null. " + token_path.string() + " Error: " + m_error_string;
    return true;
  }
  m_vocabularySize = m_tokenizer->vocab_size();
  return false;
}

bool TextGenerator::LoadTokenizerConfig() {
  std::filesystem::path configPath = std::filesystem::path(m_tokenizerPath) / "tokenizer_config.json";
  std::ifstream configFile(configPath);
  if (!configFile.is_open()) {
    m_eosTokenId = -1;
    m_bosTokenId = -1;
    m_padTokenId = -1;
  } else {
    std::string jsonStr((std::istreambuf_iterator<char>(configFile)), std::istreambuf_iterator<char>());
    configFile.close();

    rapidjson::Document config;
    if (config.Parse(jsonStr.c_str()).HasParseError()) {
      m_error_string = std::string("JSON parse error at offset ") + std::to_string(config.GetErrorOffset()) + ": " +
                       rapidjson::GetParseError_En(config.GetParseError());
      return true;
    }

    auto extractTokenId = [&](const char *tokenName) -> std::optional<int64_t> {
      if (!config.HasMember(tokenName)) return std::nullopt;
      const rapidjson::Value &token = config[tokenName];
      if (token.IsString()) {
        std::string tokenStr = token.GetString();
        if (!tokenStr.empty()) {
          auto encoding = m_tokenizer->encode(tokenStr, false);
          if (!encoding.ids().empty()) return static_cast<int64_t>(encoding.ids()[0]);
        }
      } else if (token.IsObject() && token.HasMember("id") && token["id"].IsInt64()) {
        return token["id"].GetInt64();
      } else if (token.IsInt64()) {
        return token.GetInt64();
      }
      return std::nullopt;
    };

    if (auto eosId = extractTokenId("eos_token")) m_eosTokenId = *eosId;
    if (auto bosId = extractTokenId("bos_token")) m_bosTokenId = *bosId;
    if (auto padId = extractTokenId("pad_token")) m_padTokenId = *padId;

    if (config.HasMember("eos_token_id") && config["eos_token_id"].IsInt64())
      m_eosTokenId = config["eos_token_id"].GetInt64();
    if (config.HasMember("bos_token_id") && config["bos_token_id"].IsInt64())
      m_bosTokenId = config["bos_token_id"].GetInt64();
    if (config.HasMember("pad_token_id") && config["pad_token_id"].IsInt64())
      m_padTokenId = config["pad_token_id"].GetInt64();
  }

  if (m_eosTokenId < 0) {
    auto enc = m_tokenizer->encode("<|endoftext|>", false);
    if (!enc.ids().empty()) m_eosTokenId = enc.ids()[0];
  }
  if (m_bosTokenId < 0) {
    auto enc = m_tokenizer->encode("<|im_start|>", false);
    if (!enc.ids().empty()) m_bosTokenId = enc.ids()[0];
  }
  if (m_padTokenId < 0) {
    auto enc = m_tokenizer->encode("<|im_end|>", false);
    if (!enc.ids().empty()) m_padTokenId = enc.ids()[0];
  }

  std::string modelTypeLower = m_modelType;
  std::transform(modelTypeLower.begin(), modelTypeLower.end(), modelTypeLower.begin(), ::tolower);
  if (modelTypeLower.find("qwen") != std::string::npos) {
    if (m_eosTokenId < 0) {
      auto enc = m_tokenizer->encode("<|endoftext|>", false);
      if (!enc.ids().empty()) m_eosTokenId = enc.ids()[0];
    }
    if (m_padTokenId < 0) {
      auto enc = m_tokenizer->encode("<|im_end|>", false);
      if (!enc.ids().empty()) m_padTokenId = enc.ids()[0];
    }
  }

  return false;
}

tokenizers::Tokenizer::Encoding TextGenerator::BuildPromptEncoding(const std::string &userInput,
                                                                   const std::string &systemPromptOverride) {
  auto lang = m_gen_option.language;
  std::string sys_prompt;
  if (!systemPromptOverride.empty()) sys_prompt = systemPromptOverride;

  if (lang == "chinese" || lang == "zh" || lang == "cn") {
    sys_prompt = "你是一个有用的中文助手。请用中文回答用户的问题。";
  } else if (lang == "english" || lang == "en") {
    sys_prompt = "You are a helpful English assistant. Please respond in English.";
  } else {
    sys_prompt = "You are a helpful assistant.";
  }

  std::vector<tokenizers::ChatMessage> messages = {{"system", sys_prompt}, {"user", userInput}};
  if (m_tokenizer->has_chat_template()) {
    auto enc = m_tokenizer->apply_chat_template_and_encode(messages, true, "", false);
    if (!enc.ids().empty()) {
      auto ids = enc.ids();
      DBUG_PRINT("info", ("Chat template encoded %zu tokens", ids.size()));
      return enc;
    }
  }

  std::string lowerModel = m_modelType;
  std::transform(lowerModel.begin(), lowerModel.end(), lowerModel.begin(), ::tolower);
  std::string fallback;

  if (lowerModel.find("qwen") != std::string::npos) {
    fallback = "<|im_start|>system\n" + sys_prompt + "<|im_end|>\n";
    fallback += "<|im_start|>user\n" + userInput + "<|im_end|>\n";
    fallback += "<|im_start|>assistant\n";
  } else if (lowerModel.find("llama") != std::string::npos) {
    fallback = "<|begin_of_text|><|start_header_id|>system<|end_header_id|>\n\n";
    fallback += sys_prompt + "<|eot_id|>";
    fallback += "<|start_header_id|>user<|end_header_id|>\n\n";
    fallback += userInput + "<|eot_id|>";
    fallback += "<|start_header_id|>assistant<|end_header_id|>\n\n";
  } else if (lowerModel.find("mistral") != std::string::npos) {
    fallback = "[INST] " + userInput + " [/INST]";
  } else if (lowerModel.find("phi") != std::string::npos) {
    fallback = "<|system|>\n" + sys_prompt + "<|end|>\n";
    fallback += "<|user|>\n" + userInput + "<|end|>\n";
    fallback += "<|assistant|>\n";
  } else {
    fallback = sys_prompt + "\n\n" + userInput + "\n";
  }

  auto encoding = m_tokenizer->encode(fallback, false);
  auto ids = encoding.ids();
  DBUG_PRINT("info", ("Manual format encoded %zu tokens", ids.size()));
  return encoding;
}

std::vector<std::string> TextGenerator::GetModelSpecificStopTokens(const std::string &modelType) {
  std::vector<std::string> stopTokens;

  std::string lowerModelType = modelType;
  std::transform(lowerModelType.begin(), lowerModelType.end(), lowerModelType.begin(), ::tolower);

  if (lowerModelType.find("qwen") != std::string::npos) {
    stopTokens = {"<|im_end|>", "<|im_start|>", "<|endoftext|>"};
  } else if (lowerModelType.find("llama") != std::string::npos) {
    stopTokens = {"</s>", "<|eot_id|>"};  // Llama 3<|eot_id|>
  } else if (lowerModelType.find("mistral") != std::string::npos) {
    stopTokens = {"</s>", "[INST]", "[/INST]"};
  } else if (lowerModelType.find("phi") != std::string::npos) {
    stopTokens = {"<|endoftext|>", "<|end|>"};
  } else if (lowerModelType.find("gemma") != std::string::npos) {
    stopTokens = {"<end_of_turn>", "<eos>", "<bos>"};
  } else if (lowerModelType.find("chatglm") != std::string::npos) {
    stopTokens = {"<|endoftext|>", "</s>"};
  } else if (lowerModelType.find("baichuan") != std::string::npos) {
    stopTokens = {"</s>", "<|endoftext|>"};
  } else if (lowerModelType.find("yi") != std::string::npos) {
    stopTokens = {"<|im_end|>", "<|im_start|>"};  // like Qwen
  } else {
    // fallback
    stopTokens = {"\n\n", "<|endoftext|>", "</s>", "<|im_end|>"};
  }

  return stopTokens;
}

void TextGenerator::ValidatePromptFormat(const std::vector<uint32_t> &token_ids) {
  fprintf(stderr, "\n=== Prompt Format Validation ===\n");

  bool has_im_start = false, has_im_end = false;
  for (auto id : token_ids) {
    if (m_bosTokenId > 0 && (int64_t)id == m_bosTokenId) has_im_start = true;
    if (m_eosTokenId > 0 && (int64_t)id == m_eosTokenId) has_im_end = true;
  }

  fprintf(stderr, "Contains <|im_start|>(id=%ld): %s\n", m_bosTokenId, has_im_start ? "YES" : "NO");
  fprintf(stderr, "Contains <|im_end|>(id=%ld): %s\n", m_eosTokenId, has_im_end ? "YES" : "NO");

  std::vector<uint32_t> sample_ids;
  size_t sample_size = std::min(size_t(50), token_ids.size());
  for (size_t i = 0; i < sample_size; i++) {
    sample_ids.push_back(token_ids[i]);
  }

  std::string sample_text = m_tokenizer->decode(sample_ids, false);
  fprintf(stderr, "Sample decoded text (first 50 tokens):\n%s\n", sample_text.c_str());
  fprintf(stderr, "================================\n");
}

void TextGenerator::TestTokenizerCompatibility() {
  if (!m_tokenizer || !m_tokenizer->is_valid()) {
    std::cout << "[ERROR] Tokenizer not initialized" << std::endl;
    return;
  }

  std::cout << "\n=== Tokenizer Compatibility Test ===" << std::endl;

  std::string testText = "Hello, how are you?";
  auto encoding = m_tokenizer->encode(testText, false);
  auto tokenIds = encoding.ids();

  std::vector<uint32_t> decodeIds(tokenIds.begin(), tokenIds.end());
  std::string decoded = m_tokenizer->decode(decodeIds, true);

  std::cout << "Original: " << testText << std::endl;
  std::cout << "Decoded:  " << decoded << std::endl;
  std::cout << "Match: " << (testText == decoded ? "YES" : "NO") << std::endl;

  std::vector<std::pair<std::string, std::string>> specialTokens = {{"<|im_start|>", "Chat template start token"},
                                                                    {"<|im_end|>", "Chat template end token"},
                                                                    {"<|endoftext|>", "End of text token"},
                                                                    {"<|extra_0|>", "Extra token (if exists)"}};

  for (const auto &[token, desc] : specialTokens) {
    auto enc = m_tokenizer->encode(token, false);
    if (!enc.ids().empty()) {
      auto dec = m_tokenizer->decode(enc.ids(), false);
      std::cout << desc << ": " << token << " -> ID(" << enc.ids()[0] << ") -> " << dec;
      std::cout << " [" << (token == dec ? "OK" : "MISMATCH") << "]" << std::endl;
    } else {
      std::cout << desc << ": " << token << " -> [NOT FOUND]" << std::endl;
    }
  }

  std::string userInput = "What is AI?";
  auto testEncoding = BuildPromptEncoding(userInput, "");
  std::vector<uint32_t> inputIds = testEncoding.ids();
  ValidatePromptFormat(inputIds);

  std::cout << "\nChat Template Test:" << std::endl;
  std::cout << "Token count: " << testEncoding.ids().size() << std::endl;

  std::string chatDecoded = m_tokenizer->decode(testEncoding.ids(), true);
  std::cout << "Decoded prompt: " << chatDecoded << std::endl;

  std::cout << "\nConfigured special tokens:" << std::endl;
  std::cout << "EOS token ID: " << m_eosTokenId << std::endl;
  std::cout << "BOS token ID: " << m_bosTokenId << std::endl;
  std::cout << "PAD token ID: " << m_padTokenId << std::endl;

  std::cout << "[Info] Generating with model: " << m_modelType << std::endl;
  auto enc = m_tokenizer->encode("What is AI?");
  auto ids = enc.ids();
  for (auto id : ids) printf("%d ", id);

  auto dec_str = m_tokenizer->decode(ids, true);
  std::cout << std::endl << "dec_str:" << dec_str << std::endl;
  std::cout << "=== Test Complete ===" << std::endl;
}

int64_t TextGenerator::Argmax(const float *data, size_t size) {
  if (size == 0) return -1;
  int64_t maxIndex = 0;
  float maxValue = data[0];
  for (size_t i = 1; i < size; ++i) {
    if (data[i] > maxValue) {
      maxValue = data[i];
      maxIndex = static_cast<int64_t>(i);
    }
  }
  return maxIndex;
}

void TextGenerator::AnalyzeModelInputShapes() {
  std::cout << "\n=== Analyze Model Input Shapes ===" << std::endl;

  Ort::AllocatorWithDefaultOptions allocator;

  for (size_t i = 0; i < m_session->GetInputCount(); ++i) {
    Ort::AllocatedStringPtr inputNamePtr = m_session->GetInputNameAllocated(i, allocator);
    if (!inputNamePtr) continue;
    std::string inputName = inputNamePtr.get();

    Ort::TypeInfo typeInfo = m_session->GetInputTypeInfo(i);
    auto shapeInfo = typeInfo.GetTensorTypeAndShapeInfo();
    auto shape = shapeInfo.GetShape();
    auto elemType = shapeInfo.GetElementType();

    std::cout << "input" << i << " : " << inputName << "\n";
    std::cout << "  shape: [";
    for (size_t j = 0; j < shape.size(); ++j) {
      if (j > 0) std::cout << ", ";
      std::cout << (shape[j] == -1 ? "dyn" : std::to_string(shape[j]));
    }
    std::cout << "]  dtype: " << elemType << "\n";

    if (inputName.find("past_key_values") != std::string::npos && shape.size() == 4) {
      auto [layerIdx, isKey] = ParseKVCacheInputName(inputName);
      KVShapeInfo si = DetectKVShapeLayout(shape);

      std::cout << "  KV Cache layer=" << layerIdx << " type=" << (isKey ? "Key" : "Value") << "\n";

      if (si.layout != KVCacheLayout::UNKNOWN) {
        const char *layout_names[] = {"", "BHSD", "BSHD", "BHDS"};
        std::cout << "  Layout: " << layout_names[static_cast<int>(si.layout)] << "\n";
        std::cout << "  dim" << si.dim_heads << " = heads      = " << shape[si.dim_heads] << "\n";
        std::cout << "  dim" << si.dim_seq << " = seq        = dyn\n";
        std::cout << "  dim" << si.dim_head_dim << " = head_dim   = " << shape[si.dim_head_dim] << "\n";
      } else {
        std::cout << "  [WARN] Layout could not be determined\n";
      }
    }
    std::cout << "\n";
  }
  std::cout << "=== Done ===" << std::endl;
}

void TextGenerator::GetModelMetadata() {
  Ort::AllocatorWithDefaultOptions allocator;
  Ort::ModelMetadata modelMetadata = m_session->GetModelMetadata();

  // all possible meta field.
  std::vector<std::pair<std::string, size_t *>> metadataFields = {// Query heads
                                                                  {"num_attention_heads", &m_numQueryHeads},
                                                                  {"num_heads", &m_numQueryHeads},
                                                                  {"n_head", &m_numQueryHeads},
                                                                  {"attention_heads", &m_numQueryHeads},

                                                                  // KV heads
                                                                  {"num_kv_heads", &m_numKVHeads},
                                                                  {"num_key_value_heads", &m_numKVHeads},
                                                                  {"n_kv_heads", &m_numKVHeads},
                                                                  {"kv_heads", &m_numKVHeads},

                                                                  // Head dimension
                                                                  {"head_dim", &m_headDim},
                                                                  {"d_head", &m_headDim},
                                                                  {"head_size", &m_headDim},

                                                                  // Layers
                                                                  {"num_layers", &m_numLayers},
                                                                  {"num_hidden_layers", &m_numLayers},
                                                                  {"n_layer", &m_numLayers},
                                                                  {"n_layers", &m_numLayers}};

  for (const auto &[fieldName, targetVar] : metadataFields) {
    if (*targetVar > 0) continue;

    Ort::AllocatedStringPtr value = modelMetadata.LookupCustomMetadataMapAllocated(fieldName.c_str(), allocator);
    if (value) {
      int detectedValue = std::stoi(value.get());
      if (detectedValue > 0) *targetVar = static_cast<size_t>(detectedValue);
    }
  }

  if (m_headDim == 0 && m_numQueryHeads > 0) {
    std::vector<std::string> hiddenSizeNames = {"hidden_size", "n_embd", "d_model", "embed_dim"};
    for (const std::string &name : hiddenSizeNames) {
      Ort::AllocatedStringPtr value = modelMetadata.LookupCustomMetadataMapAllocated(name.c_str(), allocator);
      if (value) {
        int hiddenSize = std::stoi(value.get());
        if (hiddenSize > 0) {
          m_headDim = static_cast<size_t>(hiddenSize) / m_numQueryHeads;
          break;
        }
      }
    }
  }
}

void TextGenerator::DetectQueryHeadsFromOutputs(const std::vector<std::string> &) {
  Ort::AllocatorWithDefaultOptions allocator;

  // Look for attention output or similar that might reveal query heads
  for (size_t i = 0; i < m_session->GetOutputCount(); ++i) {
    Ort::AllocatedStringPtr outputNamePtr = m_session->GetOutputNameAllocated(i, allocator);
    if (!outputNamePtr) continue;
    std::string outputName = outputNamePtr.get();

    if (outputName.find("attn") != std::string::npos || outputName.find("attention") != std::string::npos) {
      Ort::TypeInfo typeInfo = m_session->GetOutputTypeInfo(i);
      auto shapeInfo = typeInfo.GetTensorTypeAndShapeInfo();
      auto shape = shapeInfo.GetShape();

      // Some attention outputs might have shape [batch, num_heads, seq_len, head_dim]
      if (shape.size() >= 4 && shape[1] > 0) {
        size_t detectedHeads = static_cast<size_t>(shape[1]);
        if (detectedHeads > 0 && detectedHeads != m_numQueryHeads) {
          m_numQueryHeads = detectedHeads;
          break;
        }
      }
    }
  }
}

TextGenerator::KVShapeInfo TextGenerator::DetectKVShapeLayout(const std::vector<int64_t> &shape) {
  KVShapeInfo info;
  if (shape.size() != 4) return info;

  bool d1_fixed = shape[1] > 0;
  bool d2_fixed = shape[2] > 0;
  bool d3_fixed = shape[3] > 0;

  if (d1_fixed && !d2_fixed && d3_fixed) {
    // [-1, heads, -1, head_dim]  →  BHSD
    info.layout = TextGenerator::KVCacheLayout::BHSD;
    info.dim_heads = 1;
    info.dim_seq = 2;
    info.dim_head_dim = 3;
  } else if (!d1_fixed && d2_fixed && d3_fixed) {
    // [-1, -1, heads, head_dim]  →  BSHD
    info.layout = TextGenerator::KVCacheLayout::BSHD;
    info.dim_seq = 1;
    info.dim_heads = 2;
    info.dim_head_dim = 3;
  } else if (d1_fixed && d2_fixed && !d3_fixed) {
    // [-1, heads, head_dim, -1]  →  BHDS
    info.layout = TextGenerator::KVCacheLayout::BHDS;
    info.dim_heads = 1;
    info.dim_head_dim = 2;
    info.dim_seq = 3;
  } else {
    std::string log_msg = std::string("[WARN] KV cache shape [") + std::to_string(shape[0]) + "," +
                          std::to_string(shape[1]) + "," + std::to_string(shape[2]) + "," + std::to_string(shape[3]) +
                          "] does not match any known layout pattern\n";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return info;
  }

  info.num_heads = static_cast<size_t>(shape[info.dim_heads]);
  info.head_dim = static_cast<size_t>(shape[info.dim_head_dim]);
  return info;
}

void TextGenerator::DetectModelArchitecture(const std::vector<std::string> &inputNames,
                                            const std::vector<std::string> &outputNames) {
  m_numLayers = 0;
  m_numQueryHeads = 0;
  m_numKVHeads = 0;
  m_headDim = 0;
  m_kvCacheLayout = KVCacheLayout::UNKNOWN;
  m_attentionLayerIndices.clear();

  Ort::AllocatorWithDefaultOptions allocator;
  std::set<size_t> allKVLayers;  // any layer with key or value cache

  for (size_t i = 0; i < m_session->GetInputCount(); ++i) {
    Ort::AllocatedStringPtr inputNamePtr = m_session->GetInputNameAllocated(i, allocator);
    if (!inputNamePtr) continue;
    std::string inputName = inputNamePtr.get();

    if (inputName == "position_ids") {
      auto ti = m_session->GetInputTypeInfo(i);
      auto si = ti.GetTensorTypeAndShapeInfo();
      auto sh = si.GetShape();
      // M-RoPE: [3, batch, seq] — first dim is fixed 3
      if (sh.size() == 3 && sh[0] == 3) m_positionIdsDims = 3;
    }

    bool is_kv = inputName.find("past_key_values") != std::string::npos ||
                 inputName.find("past_key") != std::string::npos || inputName.find("cache") != std::string::npos;
    if (!is_kv) continue;
    bool is_opaque =
        inputName.find("conv_state") != std::string::npos || inputName.find("recurrent_state") != std::string::npos;
    if (is_opaque) continue;

    Ort::TypeInfo typeInfo = m_session->GetInputTypeInfo(i);
    auto shapeInfo = typeInfo.GetTensorTypeAndShapeInfo();
    auto shape = shapeInfo.GetShape();

    if (m_cacheDataType == ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED) m_cacheDataType = shapeInfo.GetElementType();

    auto [layerIdx, isKey] = ParseKVCacheInputName(inputName);

    // Track only KV (attention) layer indices — not Mamba layers
    if (isKey) m_attentionLayerIndices.insert(layerIdx);
    allKVLayers.insert(layerIdx);

    if (m_kvCacheLayout == KVCacheLayout::UNKNOWN && shape.size() == 4) {
      KVShapeInfo si = DetectKVShapeLayout(shape);
      if (si.layout != KVCacheLayout::UNKNOWN) {
        m_kvCacheLayout = si.layout;
        if (m_numKVHeads == 0) m_numKVHeads = si.num_heads;
        if (m_headDim == 0) m_headDim = si.head_dim;

        DBUG_PRINT("info", ("KV cache layout detected: %s "
                            "(dim_heads=%d, dim_seq=%d, dim_head_dim=%d) "
                            "num_kv_heads=%zu head_dim=%zu",
                            m_kvCacheLayout == KVCacheLayout::BHSD   ? "BHSD [batch,heads,seq,head_dim]"
                            : m_kvCacheLayout == KVCacheLayout::BSHD ? "BSHD [batch,seq,heads,head_dim]"
                                                                     : "BHDS [batch,heads,head_dim,seq]",
                            si.dim_heads, si.dim_seq, si.dim_head_dim, m_numKVHeads, m_headDim));
      }
    }
  }

  if (!allKVLayers.empty()) m_numLayers = *allKVLayers.rbegin() + 1;

  GetModelMetadata();

  if (m_numQueryHeads == 0) DetectQueryHeadsFromOutputs(outputNames);

  DetectInputMode(inputNames);

  InitOpaqueStates(inputNames, outputNames);

  // Validation
  if (m_numLayers == 0 || m_numKVHeads == 0 || m_headDim == 0) {
    assert(false);
  }
}

void TextGenerator::DetectInputMode(const std::vector<std::string> &inputNames) {
  m_inputMode = InputMode::INPUT_IDS;  // default

  for (const auto &name : inputNames) {
    if (name == "inputs_embeds") {
      m_inputMode = InputMode::INPUTS_EMBEDS;

      // Extract hidden size from the declared input shape [batch, seq, hidden].
      Ort::AllocatorWithDefaultOptions alloc;
      for (size_t i = 0; i < m_session->GetInputCount(); ++i) {
        auto ptr = m_session->GetInputNameAllocated(i, alloc);
        if (std::string(ptr.get()) != "inputs_embeds") continue;
        auto ti = m_session->GetInputTypeInfo(i);
        auto si = ti.GetTensorTypeAndShapeInfo();
        auto sh = si.GetShape();
        // shape: [batch, seq, hidden]  — hidden is the last fixed dim
        if (sh.size() >= 3 && sh.back() > 0) m_hiddenSize = static_cast<size_t>(sh.back());
        break;
      }

      DBUG_PRINT("info", ("InputMode = INPUTS_EMBEDS, hidden_size=%zu", m_hiddenSize));
      LoadEmbeddingModel();
      return;
    }
  }

  DBUG_PRINT("info", ("InputMode = INPUT_IDS"));
}

bool TextGenerator::LoadEmbeddingModel() {
  namespace fs = std::filesystem;
  auto extractVariantSuffix = [](const std::string &modelFilename) -> std::string {
    // notice order: _q4f16 is before _q4.
    static const std::vector<std::string> knownSuffixes = {"_q4f16", "_q4", "_fp16", "_quantized"};
    std::string stem = modelFilename;
    auto dotPos = stem.rfind(".onnx");
    if (dotPos != std::string::npos) stem = stem.substr(0, dotPos);

    for (const auto &suffix : knownSuffixes) {
      if (stem.size() >= suffix.size() && stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0)
        return suffix;
    }
    return "";
  };

  std::string mainModelFilename = fs::path(m_modelPath).filename().string();
  std::string variantSuffix = extractVariantSuffix(mainModelFilename);
  fs::path onnxDir = fs::path(m_modelPath).parent_path();

  std::vector<std::string> candidates;
  if (!variantSuffix.empty()) candidates.push_back("embed_tokens" + variantSuffix + ".onnx");
  if (variantSuffix != "_fp16") candidates.push_back("embed_tokens_fp16.onnx");
  candidates.push_back("embed_tokens.onnx");

  for (const auto &filename : candidates) {
    fs::path embedPath = onnxDir / filename;
    if (!fs::exists(embedPath)) {
      DBUG_PRINT("info", ("[EmbedModel] candidate '%s' not found, skipping.", filename.c_str()));
      continue;
    }

    try {
      m_embedSession = std::make_unique<Ort::Session>(*m_env, embedPath.string().c_str(), *m_sessionOptions);
      m_embedSessionLoaded = true;

      if (filename != "embed_tokens" + variantSuffix + ".onnx") {
        sql_print_information(
            "[EmbedModel] main model is '%s' but loaded '%s' as fallback "
            "(output dtype is float32 — fully compatible with decoder).",
            mainModelFilename.c_str(), filename.c_str());
      } else {
        DBUG_PRINT("info", ("[EmbedModel] loaded: %s (variant='%s')", embedPath.c_str(),
                            variantSuffix.empty() ? "fp32" : variantSuffix.c_str()));
      }
      return true;
    } catch (const Ort::Exception &e) {
      sql_print_warning("[EmbedModel] failed to load '%s': %s — trying next candidate.", filename.c_str(), e.what());
      m_embedSession.reset();
    } catch (const std::exception &e) {
      sql_print_warning("[EmbedModel] unexpected error loading '%s': %s — trying next candidate.", filename.c_str(),
                        e.what());
      m_embedSession.reset();
    }
  }
  return false;
}

std::vector<float> TextGenerator::TokensToEmbeddings(const std::vector<int64_t> &ids) {
  const size_t seqLen = ids.size();
  const size_t outSize = seqLen * m_hiddenSize;

  if (!m_embedSessionLoaded || !m_embedSession) {
    // Zero fallback — at least won't crash ORT
    return std::vector<float>(outSize, 0.0f);
  }

  Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  // Input: input_ids [1, seqLen] int64
  std::vector<int64_t> idsCopy(ids);
  std::vector<int64_t> inShape = {1, static_cast<int64_t>(seqLen)};
  auto idsTensor =
      Ort::Value::CreateTensor<int64_t>(mem, idsCopy.data(), idsCopy.size(), inShape.data(), inShape.size());

  Ort::AllocatorWithDefaultOptions alloc;
  auto inNamePtr = m_embedSession->GetInputNameAllocated(0, alloc);
  auto outNamePtr = m_embedSession->GetOutputNameAllocated(0, alloc);
  const char *inNames[] = {inNamePtr.get()};
  const char *outNames[] = {outNamePtr.get()};

  try {
    auto outputs = m_embedSession->Run(Ort::RunOptions{nullptr}, inNames, &idsTensor, 1, outNames, 1);
    // Output may be fp32 or fp16 — always return fp32
    auto &out = outputs[0];
    auto dtype = out.GetTensorTypeAndShapeInfo().GetElementType();

    if (dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      const float *data = out.GetTensorData<float>();
      return std::vector<float>(data, data + outSize);
    } else if (dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
      const Ort::Float16_t *data = out.GetTensorData<Ort::Float16_t>();
      std::vector<float> result(outSize);
      for (size_t k = 0; k < outSize; ++k) result[k] = static_cast<float>(data[k]);
      return result;
    }
  } catch (const Ort::Exception &e) {
    DBUG_PRINT("error", ("TokensToEmbeddings failed: %s", e.what()));
  }

  return std::vector<float>(outSize, 0.0f);
}

void TextGenerator::InitOpaqueStates(const std::vector<std::string> &, const std::vector<std::string> &outputNames) {
  m_opaqueStates.clear();

  std::set<std::string> outputSet(outputNames.begin(), outputNames.end());
  for (size_t i = 0; i < m_session->GetInputCount(); ++i) {
    Ort::AllocatorWithDefaultOptions alloc;
    std::string inName = m_session->GetInputNameAllocated(i, alloc).get();

    //   past_conv.X        → present_conv.X
    //   past_recurrent.X   → present_recurrent.X
    //   conv_state.X       → new_conv_state.X   (other models might use "conv_cache" or just "conv_state")
    //   recurrent_state.X  → new_recurrent_state.X
    auto tryRegister = [&](const std::string &inPrefix, const std::string &outPrefix) {
      if (inName.find(inPrefix) != 0) return;
      std::string suffix = inName.substr(inPrefix.size());
      std::string outName = outPrefix + suffix;
      if (outputSet.find(outName) == outputSet.end()) outName = "";

      auto typeInfo = m_session->GetInputTypeInfo(i);
      auto shapeInfo = typeInfo.GetTensorTypeAndShapeInfo();
      auto shape = shapeInfo.GetShape();
      auto dtype = shapeInfo.GetElementType();
      std::vector<int64_t> concreteShape = shape;
      if (!concreteShape.empty() && concreteShape[0] < 0) concreteShape[0] = 1;
      size_t elemCount = 1;
      for (auto d : concreteShape) elemCount *= (d > 0 ? d : 1);
      size_t elemSize = (dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) ? 4 : 2;

      OpaqueState st;
      st.in_name = inName;
      st.out_name = outName;
      st.dtype = dtype;
      st.shape = concreteShape;
      st.data.assign(elemCount * elemSize, 0);
      m_opaqueStates.push_back(std::move(st));

      DBUG_PRINT("info", ("[OpaqueState] registered: %s → %s  shape=[", inName.c_str(),
                          outName.empty() ? "?" : outName.c_str()));
      for (size_t j = 0; j < concreteShape.size(); ++j) DBUG_PRINT("info", ("%s%ld", j ? "," : "", concreteShape[j]));
      DBUG_PRINT("info", ("]\n"));
    };

    tryRegister("past_conv", "present_conv");
    tryRegister("past_recurrent", "present_recurrent");
    tryRegister("conv_state", "new_conv_state");
    tryRegister("recurrent_state", "new_recurrent_state");
  }

  DBUG_PRINT("info", ("Opaque states initialized: %zu", m_opaqueStates.size()));
}

Ort::Value TextGenerator::BuildOpaqueStateTensor(const OpaqueState &st, const Ort::MemoryInfo &mem) {
  void *ptr = const_cast<uint8_t *>(st.data.data());
  return Ort::Value::CreateTensor(mem, ptr, st.data.size(), st.shape.data(), st.shape.size(), st.dtype);
}

void TextGenerator::UpdateOpaqueStatesFromOutputs(Ort::IoBinding &binding, const std::vector<std::string> &,
                                                  const Ort::MemoryInfo &) {
  if (m_opaqueStates.empty()) return;

  auto outputValues = binding.GetOutputValues();
  auto outputValNames = binding.GetOutputNames();

  for (auto &st : m_opaqueStates) {
    if (st.out_name.empty()) continue;

    // Find the matching output tensor
    for (size_t oi = 0; oi < outputValNames.size(); ++oi) {
      if (outputValNames[oi] != st.out_name) continue;
      if (!outputValues[oi]) break;

      const void *src = nullptr;
      auto dtype = outputValues[oi].GetTensorTypeAndShapeInfo().GetElementType();
      if (dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        src = outputValues[oi].GetTensorData<float>();
      else if (dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16)
        src = outputValues[oi].GetTensorData<Ort::Float16_t>();
      else if (dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8)
        src = outputValues[oi].GetTensorData<int8_t>();

      if (src && !st.data.empty()) std::memcpy(st.data.data(), src, st.data.size());
      break;
    }
  }
}

void TextGenerator::InitializeKVCache(int max_seq) {
  if (m_numLayers == 0 || m_numKVHeads == 0 || m_headDim == 0 ||
      m_cacheDataType == ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED) {
    return;
  }

  switch (m_cacheDataType) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      m_floatCache.init(m_numLayers, m_numKVHeads, m_headDim, std::max(m_gen_option.max_tokens, max_seq));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
      m_fp16Cache.init(m_numLayers, m_numKVHeads, m_headDim, std::max(m_gen_option.max_tokens, max_seq));
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
      m_int8Cache.init(m_numLayers, m_numKVHeads, m_headDim, std::max(m_gen_option.max_tokens, max_seq));
      break;
    default:
      return;
  }

  DBUG_PRINT("info", ("KV Cache initialized: layers=%ld, heads=%ld, head_dim=%ld, max_seq=%d", m_numLayers,
                      m_numKVHeads, m_headDim, std::max(m_gen_option.max_tokens, max_seq)));
  m_kvCacheInitialized = true;
}

void TextGenerator::ClearKVCache() {
  switch (m_cacheDataType) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      m_floatCache.clear();
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
      m_fp16Cache.clear();
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
      m_int8Cache.clear();
      break;
    default:
      break;
  }
  // Also zero out Mamba recurrent states so the next conversation starts clean.
  for (auto &st : m_opaqueStates) std::fill(st.data.begin(), st.data.end(), uint8_t{0});
  m_kvCacheInitialized = false;
}

std::pair<size_t, bool> TextGenerator::ParseKVCacheInputName(const std::string &name) const {
  auto it = m_kvNameCache.find(name);
  if (it != m_kvNameCache.end()) return it->second;

  size_t layerIdx = 0;
  bool isKey = false;  // default to false

  // more patterns, such as `layer_1, past_key_values_1`.
  static const std::vector<std::regex> layerPatterns = {
      std::regex(R"(\.(\d+)\.)"),                            // match .1.
      std::regex(R"(_(\d+)_)", std::regex::optimize),        // match _1_
      std::regex(R"(layer\.(\d+))", std::regex::optimize),   // match layer.1
      std::regex(R"(layers\.(\d+))", std::regex::optimize),  // match layers.1
      std::regex(R"(layer_(\d+))", std::regex::optimize),    // match layer_1
      std::regex(R"(layers_(\d+))", std::regex::optimize),   // match layers_1
      std::regex(R"(h\.(\d+))", std::regex::optimize),       // match h.1
      std::regex(R"(block\.(\d+))", std::regex::optimize),   // match block.1
      std::regex(R"(past_key_values\.(\d+))", std::regex::optimize),
      std::regex(R"(past_key_values_(\d+))", std::regex::optimize)};

  bool foundLayer = false;
  for (const auto &pattern : layerPatterns) {
    std::smatch match;
    if (std::regex_search(name, match, pattern) && match.size() > 1) {
      layerIdx = static_cast<size_t>(std::stoi(match[1].str()));
      foundLayer = true;
      break;
    }
  }

  if (!foundLayer) {
    DBUG_PRINT("warning", ("Could not parse layer index from KV cache name: %s", name.c_str()));
  }

  size_t lastDot = name.rfind('.');
  std::string suffix = (lastDot != std::string::npos) ? name.substr(lastDot + 1) : name;
  std::string lowerSuffix = suffix;
  std::transform(lowerSuffix.begin(), lowerSuffix.end(), lowerSuffix.begin(), ::tolower);
  isKey = (lowerSuffix.find("key") != std::string::npos);

  auto res = std::make_pair(layerIdx, isKey);
  m_kvNameCache.emplace(name, res);
  return res;
}

std::pair<size_t, bool> TextGenerator::ParseKVCacheOutputName(const std::string &name) const {
  return ParseKVCacheInputName(name);
}

std::vector<int64_t> TextGenerator::BuildKVShape(size_t seqLen) const {
  std::vector<int64_t> shape(4);
  shape[0] = 1;  // batch = 1
  switch (m_kvCacheLayout) {
    case KVCacheLayout::BHSD:
      // [batch, heads, seq, head_dim]
      shape[1] = static_cast<int64_t>(m_numKVHeads);
      shape[2] = static_cast<int64_t>(seqLen);
      shape[3] = static_cast<int64_t>(m_headDim);
      break;
    case KVCacheLayout::BSHD:
      // [batch, seq, heads, head_dim]
      shape[1] = static_cast<int64_t>(seqLen);
      shape[2] = static_cast<int64_t>(m_numKVHeads);
      shape[3] = static_cast<int64_t>(m_headDim);
      break;
    case KVCacheLayout::BHDS:
      // [batch, heads, head_dim, seq]
      shape[1] = static_cast<int64_t>(m_numKVHeads);
      shape[2] = static_cast<int64_t>(m_headDim);
      shape[3] = static_cast<int64_t>(seqLen);
      break;
    default:
      // fallback：take BHSD as default
      shape[1] = static_cast<int64_t>(m_numKVHeads);
      shape[2] = static_cast<int64_t>(seqLen);
      shape[3] = static_cast<int64_t>(m_headDim);
      break;
  }
  return shape;
}

int TextGenerator::GetCurrentCacheSeq() const {
  switch (m_cacheDataType) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      return m_floatCache.seq_len();
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
      return m_fp16Cache.seq_len();
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
      return m_int8Cache.seq_len();
    default:
      return 0;
  }
}

int TextGenerator::GetCurrentCacheMaxSeq() const {
  switch (m_cacheDataType) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      return m_floatCache.max_seq_len;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
      return m_fp16Cache.max_seq_len;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
      return m_int8Cache.max_seq_len;
    default:
      return 0;
  }
}

void TextGenerator::UpdateCacheSeqCounters(size_t totalSeqLen) {
  auto update = [&](auto &cache) {
    for (int layer = 0; layer < cache.num_layers; ++layer) {
      // buffer has written by ORT, just update the seq len for next iteration.
      cache.layers[layer].seq = static_cast<int>(totalSeqLen);
    }
  };

  switch (m_cacheDataType) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      update(m_floatCache);
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
      update(m_fp16Cache);
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
      update(m_int8Cache);
      break;
    default:
      break;
  }
}

void TextGenerator::BindKVCacheDirect(Ort::IoBinding &binding, const std::vector<std::string> &outputNames,
                                      size_t totalSeqLen, const Ort::MemoryInfo &memInfo) {
  const std::vector<int64_t> shape = BuildKVShape(totalSeqLen);
  const size_t elementCount = GetElementCount(shape);

  for (const auto &name : outputNames) {
    // only dealwith present_key_values output，logits not be binded here.
    bool is_present = name.find("present") != std::string::npos || name.find("key_values") != std::string::npos;
    if (!is_present) continue;

    auto [layerIdx, isKey] = ParseKVCacheOutputName(name);
    if (layerIdx >= m_numLayers) {
      binding.BindOutput(name.c_str(), memInfo);
      continue;
    }

    switch (m_cacheDataType) {
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: {
        const size_t capacity =
            static_cast<size_t>(m_floatCache.max_seq_len) * m_floatCache.layers[layerIdx].token_stride();
        if (elementCount > capacity) {
          DBUG_PRINT("warning", ("[BindKVOutput] layer %zu %s: elementCount=%zu > capacity=%zu, "
                                 "falling back to ORT alloc",
                                 layerIdx, isKey ? "key" : "val", elementCount, capacity));
          binding.BindOutput(name.c_str(), memInfo);
          break;
        }
        float *buf = isKey ? m_floatCache.key_data(layerIdx) : m_floatCache.value_data(layerIdx);
        auto tensor = Ort::Value::CreateTensor<float>(memInfo, buf, elementCount, shape.data(), shape.size());
        binding.BindOutput(name.c_str(), tensor);
      } break;
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: {
        const size_t capacity =
            static_cast<size_t>(m_fp16Cache.max_seq_len) * m_fp16Cache.layers[layerIdx].token_stride();
        if (elementCount > capacity) {
          binding.BindOutput(name.c_str(), memInfo);
          break;
        }
        void *buf = isKey ? static_cast<void *>(m_fp16Cache.key_data(layerIdx))
                          : static_cast<void *>(m_fp16Cache.value_data(layerIdx));
        auto tensor = Ort::Value::CreateTensor(memInfo, buf,
                                               elementCount * sizeof(uint16_t),  // byteCount
                                               shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
        binding.BindOutput(name.c_str(), tensor);
      } break;
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8: {
        const size_t capacity =
            static_cast<size_t>(m_int8Cache.max_seq_len) * m_int8Cache.layers[layerIdx].token_stride();
        if (elementCount > capacity) {
          binding.BindOutput(name.c_str(), memInfo);
          break;
        }
        void *buf = isKey ? static_cast<void *>(m_int8Cache.key_data(layerIdx))
                          : static_cast<void *>(m_int8Cache.value_data(layerIdx));
        auto tensor = Ort::Value::CreateTensor(memInfo, buf, elementCount * sizeof(int8_t), shape.data(), shape.size(),
                                               ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8);
        binding.BindOutput(name.c_str(), tensor);
      } break;
      default:
        binding.BindOutput(name.c_str(), memInfo);
        break;
    }
  }
}

void TextGenerator::UpdateKVCache(const std::vector<Ort::Value> &outputs, const std::vector<std::string> &outputNames,
                                  const Ort::MemoryInfo &) {
  if (m_cacheDataType == ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED) return;
  if (m_kvCacheLayout == KVCacheLayout::UNKNOWN) return;

  struct LayerKV {
    const void *key = nullptr;
    const void *value = nullptr;
    int total_seq_len = 0;
  };
  std::map<int, LayerKV> layer_data;

  for (size_t i = 0; i < outputs.size(); ++i) {
    const std::string &name = outputNames[i];
    bool is_kv = name.find("present") != std::string::npos || name.find("key_values") != std::string::npos ||
                 name.find("past_key_values") != std::string::npos;
    if (!is_kv) continue;

    auto info = outputs[i].GetTensorTypeAndShapeInfo();
    auto shape = info.GetShape();
    if (shape.size() != 4) continue;

    int seq_dim = -1;
    switch (m_kvCacheLayout) {
      case KVCacheLayout::BHSD:
        seq_dim = 2;
        break;  // [batch, heads, seq, head_dim]
      case KVCacheLayout::BSHD:
        seq_dim = 1;
        break;  // [batch, seq, heads, head_dim]
      case KVCacheLayout::BHDS:
        seq_dim = 3;
        break;  // [batch, heads, head_dim, seq]
      default:
        continue;
    }

    int total_seq = static_cast<int>(shape[seq_dim]);
    if (total_seq <= 0) continue;

    int heads_dim = (m_kvCacheLayout == KVCacheLayout::BSHD) ? 2 : 1;
    int head_dim_dim = (m_kvCacheLayout == KVCacheLayout::BHDS) ? 2 : 3;
    if (shape[heads_dim] != static_cast<int64_t>(m_numKVHeads) ||
        shape[head_dim_dim] != static_cast<int64_t>(m_headDim)) {
      DBUG_PRINT("warning", ("[WARN] UpdateKVCache: output '%s' shape mismatch "
                             "(expected heads=%zu head_dim=%zu, got %ld %ld), skipping\n",
                             name.c_str(), m_numKVHeads, m_headDim, shape[heads_dim], shape[head_dim_dim]));
      continue;
    }

    const void *data = nullptr;
    switch (m_cacheDataType) {
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
        data = outputs[i].GetTensorData<float>();
        break;
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
        data = outputs[i].GetTensorData<Ort::Float16_t>();
        break;
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
        data = outputs[i].GetTensorData<int8_t>();
        break;
      default:
        continue;
    }
    if (!data) continue;

    auto [layerIdx, isKey] = ParseKVCacheOutputName(name);
    auto &entry = layer_data[static_cast<int>(layerIdx)];
    entry.total_seq_len = total_seq;
    if (isKey)
      entry.key = data;
    else
      entry.value = data;
  }

  if (layer_data.empty()) return;

  for (const auto &[layerIdx, kv] : layer_data) {
    if (layerIdx < 0 || static_cast<size_t>(layerIdx) >= m_numLayers) continue;
    if (!kv.key || !kv.value || kv.total_seq_len <= 0) continue;

    switch (m_cacheDataType) {
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
        m_floatCache.init_layer_from_onnx(layerIdx, static_cast<const float *>(kv.key),
                                          static_cast<const float *>(kv.value), kv.total_seq_len);
        break;
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
        m_fp16Cache.init_layer_from_onnx(layerIdx, static_cast<const Ort::Float16_t *>(kv.key),
                                         static_cast<const Ort::Float16_t *>(kv.value), kv.total_seq_len);
        break;
      case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
        m_int8Cache.init_layer_from_onnx(layerIdx, static_cast<const int8_t *>(kv.key),
                                         static_cast<const int8_t *>(kv.value), kv.total_seq_len);
        break;
      default:
        break;
    }
  }
}

size_t TextGenerator::GetElementCount(const std::vector<int64_t> &shape) const {
  size_t count = 1;
  for (int64_t dim : shape) {
    if (dim > 0) count *= static_cast<size_t>(dim);
  }
  return count;
}

bool TextGenerator::ValidateTensorBufferSize(const Ort::Value &tensor, const void *buffer, size_t bufferSize) {
  auto shapeInfo = tensor.GetTensorTypeAndShapeInfo();
  size_t elementCount = shapeInfo.GetElementCount();
  ONNXTensorElementDataType elementType = shapeInfo.GetElementType();

  size_t elementSize;
  switch (elementType) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      elementSize = sizeof(float);
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
      elementSize = sizeof(int64_t);
      break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
      elementSize = sizeof(int32_t);
      break;
    default:
      return false;
  }

  size_t expectedSize = elementCount * elementSize;
  if (expectedSize != bufferSize || buffer == nullptr || elementCount == 0) return false;

  return true;
}

int64_t TextGenerator::SampleWithTemperature(const float *logits, size_t vocabSize, float temperature) {
  if (vocabSize == 0) return -1;
  if (temperature <= 0.0f) return Argmax(logits, vocabSize);
  if (m_sampleTempBuf.size() < vocabSize) m_sampleTempBuf.resize(vocabSize);

  float maxLogit = logits[0];
  for (size_t i = 1; i < vocabSize; ++i) {
    if (logits[i] > maxLogit) maxLogit = logits[i];
  }

  float invTemp = 1.0f / temperature;
  float sum = 0.0f;
  for (size_t i = 0; i < vocabSize; ++i) {
    m_sampleTempBuf[i] = std::exp((logits[i] - maxLogit) * invTemp);
    sum += m_sampleTempBuf[i];
  }

  for (size_t i = 0; i < vocabSize; ++i) m_sampleTempBuf[i] /= sum;

  std::discrete_distribution<size_t> dist(m_sampleTempBuf.begin(), m_sampleTempBuf.begin() + vocabSize);
  return static_cast<int64_t>(dist(g_rng));
}

int64_t TextGenerator::SampleTopK(const float *logits, size_t vocabSize, int topK, float temperature) {
  if (vocabSize == 0) return -1;
  if (topK <= 0 || topK >= static_cast<int>(vocabSize)) return SampleWithTemperature(logits, vocabSize, temperature);
  if (temperature <= 0.0f) return Argmax(logits, vocabSize);

  if (m_samplePairBuf.size() < vocabSize) {
    m_samplePairBuf.resize(vocabSize);
    m_sampleProbBuf.resize(vocabSize);
  }

  for (size_t i = 0; i < vocabSize; ++i) m_samplePairBuf[i] = {logits[i], static_cast<int64_t>(i)};

  int actualK = std::min(topK, static_cast<int>(vocabSize));
  std::partial_sort(m_samplePairBuf.begin(), m_samplePairBuf.begin() + actualK, m_samplePairBuf.begin() + vocabSize,
                    std::greater<>());

  float maxLogit = m_samplePairBuf[0].first;
  float invTemp = 1.0f / temperature;
  float sum = 0.0f;
  for (int i = 0; i < actualK; ++i) {
    m_sampleProbBuf[i] = std::exp((m_samplePairBuf[i].first - maxLogit) * invTemp);
    sum += m_sampleProbBuf[i];
  }

  for (int i = 0; i < actualK; ++i) m_sampleProbBuf[i] /= sum;

  std::discrete_distribution<int> dist(m_sampleProbBuf.begin(), m_sampleProbBuf.begin() + actualK);
  int selectedIdx = dist(g_rng);
  return m_samplePairBuf[selectedIdx].second;
}

int64_t TextGenerator::SampleTopKThenTopP(const float *logits, size_t vocabSize, int topK, float topP,
                                          float temperature) {
  if (m_samplePairBuf.size() < vocabSize) m_samplePairBuf.resize(vocabSize);
  for (size_t i = 0; i < vocabSize; ++i) m_samplePairBuf[i] = {logits[i], static_cast<int64_t>(i)};
  int actualK = std::min(topK, static_cast<int>(vocabSize));
  std::partial_sort(m_samplePairBuf.begin(), m_samplePairBuf.begin() + actualK, m_samplePairBuf.begin() + vocabSize,
                    std::greater<>());

  float maxLogit = m_samplePairBuf[0].first;
  float sum = 0.0f;
  float invTemp = (temperature > 0.0f) ? (1.0f / temperature) : 1.0f;
  for (int i = 0; i < actualK; ++i) {
    m_sampleProbBuf[i] = std::exp((m_samplePairBuf[i].first - maxLogit) * invTemp);
    sum += m_sampleProbBuf[i];
  }

  float cumProb = 0.0f;
  int cutoff = actualK;
  for (int i = 0; i < actualK; ++i) {
    m_sampleProbBuf[i] /= sum;
    cumProb += m_sampleProbBuf[i];
    if (cumProb >= topP) {
      cutoff = i + 1;
      break;
    }
  }
  if (cutoff == 0) cutoff = 1;

  // In-place re-normalize + sample from [0, cutoff)
  sum = 0.0f;
  for (int i = 0; i < cutoff; ++i) sum += m_sampleProbBuf[i];
  for (int i = 0; i < cutoff; ++i) m_sampleProbBuf[i] /= sum;

  std::discrete_distribution<int> dist(m_sampleProbBuf.begin(), m_sampleProbBuf.begin() + cutoff);
  return m_samplePairBuf[dist(g_rng)].second;
}

int64_t TextGenerator::SampleTopP(const float *logits, size_t vocabSize, float topP, float temperature) {
  if (vocabSize == 0) return -1;
  if (topP <= 0.0f) return Argmax(logits, vocabSize);
  if (topP >= 1.0f) return SampleWithTemperature(logits, vocabSize, temperature);
  if (temperature <= 0.0f) return Argmax(logits, vocabSize);

  if (m_samplePairBuf.size() < vocabSize) {
    m_samplePairBuf.resize(vocabSize);
    m_sampleProbBuf.resize(vocabSize);
  }

  for (size_t i = 0; i < vocabSize; ++i) m_samplePairBuf[i] = {logits[i], static_cast<int64_t>(i)};

  std::sort(m_samplePairBuf.begin(), m_samplePairBuf.begin() + vocabSize, std::greater<>());

  float maxLogit = m_samplePairBuf[0].first;
  float invTemp = 1.0f / temperature;
  float sum = 0.0f;
  for (size_t i = 0; i < vocabSize; ++i) {
    m_sampleProbBuf[i] = std::exp((m_samplePairBuf[i].first - maxLogit) * invTemp);
    sum += m_sampleProbBuf[i];
  }

  for (size_t i = 0; i < vocabSize; ++i) m_sampleProbBuf[i] /= sum;

  float cumProb = 0.0f;
  size_t cutoff = vocabSize;
  for (size_t i = 0; i < vocabSize; ++i) {
    cumProb += m_sampleProbBuf[i];
    if (cumProb >= topP) {
      cutoff = i + 1;
      break;
    }
  }
  if (cutoff == 0) cutoff = 1;

  if (cutoff < vocabSize) {
    float subsetSum = 0.0f;
    for (size_t i = 0; i < cutoff; ++i) subsetSum += m_sampleProbBuf[i];
    for (size_t i = 0; i < cutoff; ++i) m_sampleProbBuf[i] /= subsetSum;
  }

  std::discrete_distribution<size_t> dist(m_sampleProbBuf.begin(), m_sampleProbBuf.begin() + cutoff);
  size_t selectedIdx = dist(g_rng);
  return m_samplePairBuf[selectedIdx].second;
}

void TextGenerator::ApplyRepeatPenalty(float *logits, size_t vocabSize, const std::vector<int64_t> &generatedTokens,
                                       float penalty) {
  if (penalty == 1.0f) return;

  for (int64_t token : generatedTokens) {
    if (token >= 0 && token < static_cast<int64_t>(vocabSize)) {
      if (logits[token] > 0) {
        logits[token] /= penalty;
      } else {
        logits[token] *= penalty;
      }
    }
  }
}

void TextGenerator::ApplyFrequencyPenalty(float *logits, size_t vocabSize, float penalty) {
  if (penalty == 0.0f) return;

  for (size_t i = 0; i < vocabSize && i < m_tokenFrequency.size(); ++i) {
    logits[i] -= penalty * static_cast<float>(m_tokenFrequency[i]);
  }
}

void TextGenerator::ApplyPresencePenalty(float *logits, size_t vocabSize, float penalty) {
  if (penalty == 0.0f) return;

  for (size_t i = 0; i < vocabSize && i < m_tokenPresence.size(); ++i) {
    if (m_tokenPresence[i] > 0) {
      logits[i] -= penalty;
    }
  }
}

bool TextGenerator::ShouldStop(const std::vector<int64_t> &tokens, const std::vector<std::string> &stopSequences) {
  if (tokens.empty()) return false;

  if (tokens.back() == m_eosTokenId) {
    return true;
  }

  // merge all stop words.
  std::vector<std::string> allStopSequences = stopSequences;
  auto modelSpecificStops = GetModelSpecificStopTokens(m_modelType);
  allStopSequences.insert(allStopSequences.end(), modelSpecificStops.begin(), modelSpecificStops.end());

  if (allStopSequences.empty()) return false;

  size_t checkLength = std::min(tokens.size(), size_t(20));
  std::vector<uint32_t> recentTokens;
  recentTokens.reserve(checkLength);

  for (size_t i = tokens.size() - checkLength; i < tokens.size(); ++i) {
    recentTokens.push_back(static_cast<uint32_t>(tokens[i]));
  }

  // skip_special_tokens=false so that model-specific stop tokens like <|im_end|>
  // are preserved in the decoded text and can be matched against allStopSequences.
  std::string recentText = m_tokenizer->decode(recentTokens, false);

  for (const auto &stopSeq : allStopSequences) {
    if (!stopSeq.empty() && recentText.find(stopSeq) != std::string::npos) {
      return true;
    }
  }

  return false;
}

void TextGenerator::UpdateTokenTracking(int64_t token) {
  if (token >= 0 && token < static_cast<int64_t>(m_tokenFrequency.size())) {
    m_tokenFrequency[token]++;
    m_tokenPresence[token] = 1;
  }
}

void TextGenerator::PrintTopKLogits(const std::vector<float> &logits, int top_k) const {
  float max_logit = *std::max_element(logits.begin(), logits.end());

  std::vector<std::pair<int, float>> idx_logits;
  idx_logits.reserve(logits.size());
  for (int i = 0; i < (int)logits.size(); ++i) {
    idx_logits.emplace_back(i, logits[i]);
  }

  std::partial_sort(idx_logits.begin(), idx_logits.begin() + top_k, idx_logits.end(),
                    [](auto &a, auto &b) { return a.second > b.second; });

  // softmax
  float denom = 0.0f;
  for (int i = 0; i < top_k; ++i) {
    denom += std::exp(idx_logits[i].second - max_logit);
  }

  std::cout << "[Debug] Top-" << top_k << " token probabilities:" << std::endl;
  for (int i = 0; i < top_k; ++i) {
    auto [idx, logit] = idx_logits[i];
    float prob = std::exp(logit - max_logit) / denom;

    std::string token_str;
    token_str = m_tokenizer->decode({static_cast<uint32_t>(idx)});

    std::cout << "  [" << i << "] token_id=" << idx << ", token='" << token_str << "'"
              << ", logit=" << logit << ", prob=" << prob * 100 << "%" << std::endl;
  }
}

Ort::Value TextGenerator::CreateZeroCacheTensor(ONNXTensorElementDataType type, const std::vector<int64_t> &shape,
                                                const Ort::MemoryInfo &memInfo) {
  size_t elementCount = 1;
  for (int64_t dim : shape) {
    if (dim < 0) return Ort::Value(nullptr);
    elementCount *= static_cast<size_t>(dim);
  }

  switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: {
      // Allocate at least 1 element so buf.data() is always non-null.
      // ORT still receives elementCount=0 for the actual 0-dim tensor.
      m_stepFloatBuffers.emplace_back(std::max(elementCount, size_t(1)), 0.0f);
      auto &buf = m_stepFloatBuffers.back();
      return Ort::Value::CreateTensor<float>(memInfo, buf.data(), elementCount, shape.data(), shape.size());
    }
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: {
      size_t words = (std::max(elementCount, size_t(1)) * sizeof(uint16_t) + sizeof(int64_t) - 1) / sizeof(int64_t);
      m_stepInt64Buffers.emplace_back(words, 0LL);
      void *ptr = m_stepInt64Buffers.back().data();
      return Ort::Value::CreateTensor(memInfo, ptr, elementCount * sizeof(uint16_t), shape.data(), shape.size(),
                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
    }
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8: {
      size_t words = (std::max(elementCount, size_t(1)) * sizeof(int8_t) + sizeof(int64_t) - 1) / sizeof(int64_t);
      m_stepInt64Buffers.emplace_back(words, 0LL);
      void *ptr = m_stepInt64Buffers.back().data();
      return Ort::Value::CreateTensor(memInfo, ptr, elementCount * sizeof(int8_t), shape.data(), shape.size(),
                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8);
    }
    default:
      return Ort::Value(nullptr);
  }
}

Ort::Value TextGenerator::CreateInputCacheTensor(ONNXTensorElementDataType type, size_t layerIdx, bool isKey,
                                                 const std::vector<int64_t> &shape, const Ort::MemoryInfo &memInfo) {
  switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: {
      if (static_cast<int>(layerIdx) < m_floatCache.num_layers && m_floatCache.layer_seq(layerIdx) > 0) {
        // # of expected = 1 * num_heads * past_seq * head_dim
        size_t expected = GetElementCount(shape);
        size_t cached = m_floatCache.layer_elements(layerIdx);
        if (cached == expected) {
          float *data = isKey ? m_floatCache.key_data(layerIdx) : m_floatCache.value_data(layerIdx);
          return Ort::Value::CreateTensor<float>(memInfo, data, expected, shape.data(), shape.size());
        }
      }
      return CreateZeroCacheTensor(type, shape, memInfo);
    } break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: {
      if (static_cast<int>(layerIdx) < m_fp16Cache.num_layers && m_fp16Cache.layer_seq(layerIdx) > 0) {
        size_t expected = GetElementCount(shape);
        size_t cached = m_fp16Cache.layer_elements(layerIdx);
        if (cached == expected) {
          void *data = isKey ? (void *)m_fp16Cache.key_data(layerIdx) : (void *)m_fp16Cache.value_data(layerIdx);
          return Ort::Value::CreateTensor(memInfo, data, expected * sizeof(uint16_t), shape.data(), shape.size(),
                                          ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
        }
      }
      return CreateZeroCacheTensor(type, shape, memInfo);
    } break;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8: {
      if (static_cast<int>(layerIdx) < m_int8Cache.num_layers && m_int8Cache.layer_seq(layerIdx) > 0) {
        size_t expected = GetElementCount(shape);
        size_t cached = m_int8Cache.layer_elements(layerIdx);
        if (cached == expected) {
          void *data = isKey ? (void *)m_int8Cache.key_data(layerIdx) : (void *)m_int8Cache.value_data(layerIdx);
          return Ort::Value::CreateTensor(memInfo, data, expected * sizeof(int8_t), shape.data(), shape.size(),
                                          ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8);
        }
      }
      return CreateZeroCacheTensor(type, shape, memInfo);
    } break;
    default:
      return Ort::Value(nullptr);
  }
}

void TextGenerator::NamesInitialized() {
  if (m_namesInitialized) return;
  Ort::AllocatorWithDefaultOptions alloc;
  m_inputNames.clear();
  m_outputNames.clear();
  for (size_t i = 0; i < m_session->GetInputCount(); ++i)
    m_inputNames.emplace_back(m_session->GetInputNameAllocated(i, alloc).get());
  for (size_t i = 0; i < m_session->GetOutputCount(); ++i)
    m_outputNames.emplace_back(m_session->GetOutputNameAllocated(i, alloc).get());
  m_namesInitialized = true;
}

TextGenerator::Result TextGenerator::Generate(const std::string &userPrompt, int maxNewTokens) {
  Result result;
  if (!Initialized() || !m_gen_option.validate()) return result;
  NamesInitialized();
  auto &inputNames = m_inputNames;
  auto &outputNames = m_outputNames;

#ifndef NDEBUG
  TestTokenizerCompatibility();
  AnalyzeModelInputShapes();
#endif

  m_stepFloatBuffers.clear();
  m_stepInt64Buffers.clear();

  if (m_lastPrompt != userPrompt) {
    KVCache_Reset();
    m_lastPrompt = userPrompt;
  }

  // 1. encoding prompt
  std::string promptInput =
      (m_gen_option.task == "summarization") ? "Please summarize the following text: " + userPrompt : userPrompt;
  auto encoding = BuildPromptEncoding(promptInput, "");
  std::vector<uint32_t> inputIds = encoding.ids();
  if (inputIds.empty()) return result;

  m_stopTokenIds.clear();
  if (m_eosTokenId >= 0) m_stopTokenIds.push_back(m_eosTokenId);
  // Resolve user-specified stop sequences
  for (const auto &seq : m_gen_option.stop_sequences) {
    auto enc = m_tokenizer->encode(seq, false);
    for (auto id : enc.ids()) m_stopTokenIds.push_back(static_cast<int64_t>(id));
  }

#ifndef NDEBUG
  ValidatePromptFormat(inputIds);
#endif

  std::vector<int64_t> inputIds64;
  inputIds64.reserve(inputIds.size());
  for (auto id : inputIds) inputIds64.push_back(static_cast<int64_t>(id));

  InitializeTokenTracking(m_vocabularySize > 0 ? m_vocabularySize : TextGenerator::default_vocab_size);

  Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::AllocatorWithDefaultOptions allocator;

  // 2. get input and output names.
  for (size_t i = 0; i < m_session->GetInputCount(); ++i)
    inputNames.emplace_back(m_session->GetInputNameAllocated(i, allocator).get());
  for (size_t i = 0; i < m_session->GetOutputCount(); ++i)
    outputNames.emplace_back(m_session->GetOutputNameAllocated(i, allocator).get());

  // 3. detect model architecture + initialize KV Cache
  if (m_numLayers == 0) DetectModelArchitecture(inputNames, outputNames);

  const int actualMaxSeq = static_cast<int>(inputIds64.size()) + maxNewTokens;
  if (!m_kvCacheInitialized || GetCurrentCacheMaxSeq() < actualMaxSeq) {
    InitializeKVCache(actualMaxSeq);
  }

  // 4. build up IoBinding
  if (!m_ioBinding) {
    m_ioBinding = std::make_unique<Ort::IoBinding>(*m_session);
  }
  auto &binding = *m_ioBinding;

  std::vector<int64_t> generatedTokens(inputIds64);
  std::vector<int64_t> newTokens;
  newTokens.reserve(maxNewTokens);

  // 5. generating.
  for (int step = 0; step < maxNewTokens; ++step) {
    m_stepFloatBuffers.clear();
    m_stepInt64Buffers.clear();
    binding.ClearBoundInputs();
    binding.ClearBoundOutputs();

    const size_t reserveSize = inputNames.size() * 2 + 8;
    m_stepFloatBuffers.reserve(reserveSize);
    m_stepInt64Buffers.reserve(reserveSize);

    const size_t pastSeqLen = (step == 0) ? 0 : static_cast<size_t>(GetCurrentCacheSeq());
    const size_t currentInputLen = (step == 0) ? generatedTokens.size() : 1;
    const size_t totalSeqLen = pastSeqLen + currentInputLen;

    // 5.1 bind（input_ids|inputs_embeds / attention_mask / position_ids / KV cache / opaque states）
    for (size_t inputIdx = 0; inputIdx < inputNames.size(); ++inputIdx) {
      const std::string &inputName = inputNames[inputIdx];
      Ort::Value tensor{nullptr};

      // ── opaque recurrent states (conv_state / recurrent_state) ──────────
      // These must be checked first because their names contain "past_key_values"
      // which would otherwise fall into the KV cache branch.
      bool handledAsOpaque = false;
      for (auto &st : m_opaqueStates) {
        if (st.in_name == inputName) {
          tensor = BuildOpaqueStateTensor(st, memInfo);
          handledAsOpaque = true;
          break;
        }
      }
      if (handledAsOpaque) {
        if (!tensor) break;
        binding.BindInput(inputName.c_str(), tensor);
        continue;
      }

      // ── standard inputs ──────────────────────────────────────────────────
      if (inputName == "input_ids" || inputName == "inputs" || inputName == "input") {
        // ── INPUT_IDS mode ─────────────────────────────────────────────────
        std::vector<int64_t> currentInput =
            (step == 0) ? generatedTokens : std::vector<int64_t>{generatedTokens.back()};
        m_stepInt64Buffers.push_back(std::move(currentInput));
        auto &buf = m_stepInt64Buffers.back();
        std::vector<int64_t> shape = {1, static_cast<int64_t>(buf.size())};
        tensor = Ort::Value::CreateTensor<int64_t>(memInfo, buf.data(), buf.size(), shape.data(), shape.size());

      } else if (inputName == "inputs_embeds") {
        // ── INPUTS_EMBEDS mode ─────────────────────────────────────────────
        // Convert the current token(s) to embedding vectors via the companion
        // embed_tokens session (or zero-fill as fallback).
        std::vector<int64_t> currentIds = (step == 0) ? generatedTokens : std::vector<int64_t>{generatedTokens.back()};
        auto embeddings = TokensToEmbeddings(currentIds);

        m_stepFloatBuffers.push_back(std::move(embeddings));
        auto &buf = m_stepFloatBuffers.back();
        // Shape: [1, seqLen, hiddenSize]
        std::vector<int64_t> shape = {1, static_cast<int64_t>(currentIds.size()), static_cast<int64_t>(m_hiddenSize)};
        tensor = Ort::Value::CreateTensor<float>(memInfo, buf.data(), buf.size(), shape.data(), shape.size());

      } else if (inputName == "attention_mask" || inputName.find("attention") != std::string::npos) {
        std::vector<int64_t> mask(totalSeqLen, 1);
        m_stepInt64Buffers.push_back(std::move(mask));
        auto &buf = m_stepInt64Buffers.back();
        std::vector<int64_t> shape = {1, static_cast<int64_t>(buf.size())};
        tensor = Ort::Value::CreateTensor<int64_t>(memInfo, buf.data(), buf.size(), shape.data(), shape.size());

      } else if (inputName == "position_ids" || inputName.find("position") != std::string::npos) {
        // ── Build position ids for the current step ────────────────────────
        // Standard models: [1, seqLen]  (m_positionIdsDims == 2)
        // M-RoPE models:   [3, 1, seqLen] — dim-0 is always 3 (fixed)
        //   row 0 = absolute positions, rows 1 & 2 = same (pure text)
        std::vector<int64_t> posIds;
        if (step == 0) {
          posIds.resize(currentInputLen);
          for (size_t pi = 0; pi < currentInputLen; ++pi) posIds[pi] = static_cast<int64_t>(pi);
        } else {
          posIds = {static_cast<int64_t>(pastSeqLen + currentInputLen - 1)};
        }

        if (m_positionIdsDims == 3) {
          // Replicate the 1D position vector 3 times → [3, 1, seqLen]
          size_t seqLenPos = posIds.size();
          std::vector<int64_t> pos3(3 * seqLenPos);
          for (int row = 0; row < 3; ++row)
            for (size_t c = 0; c < seqLenPos; ++c) pos3[row * seqLenPos + c] = posIds[c];
          m_stepInt64Buffers.push_back(std::move(pos3));
          auto &buf = m_stepInt64Buffers.back();
          std::vector<int64_t> shape = {3, 1, static_cast<int64_t>(seqLenPos)};
          tensor = Ort::Value::CreateTensor<int64_t>(memInfo, buf.data(), buf.size(), shape.data(), shape.size());
        } else {
          m_stepInt64Buffers.push_back(std::move(posIds));
          auto &buf = m_stepInt64Buffers.back();
          std::vector<int64_t> shape = {1, static_cast<int64_t>(buf.size())};
          tensor = Ort::Value::CreateTensor<int64_t>(memInfo, buf.data(), buf.size(), shape.data(), shape.size());
        }

      } else if (inputName.find("past_key_values") != std::string::npos) {
        // ── KV cache input： into cache buffer directly ───────────────────
        auto [layerIdx, isKey] = ParseKVCacheInputName(inputName);
        auto cacheShape = BuildKVShape(pastSeqLen);
        tensor = (pastSeqLen == 0) ? CreateZeroCacheTensor(m_cacheDataType, cacheShape, memInfo)
                                   : CreateInputCacheTensor(m_cacheDataType, layerIdx, isKey, cacheShape, memInfo);
      } else {
        // Unknown input — bind a correctly-typed zero scalar to satisfy ORT's
        // graph input validation.  This path should not be hit in practice.
        std::vector<int64_t> defaultVal = {0};
        m_stepInt64Buffers.push_back(std::move(defaultVal));
        auto &buf = m_stepInt64Buffers.back();
        std::vector<int64_t> shape = {1, 1};
        tensor = Ort::Value::CreateTensor<int64_t>(memInfo, buf.data(), buf.size(), shape.data(), shape.size());
      }

      if (!tensor) break;
      binding.BindInput(inputName.c_str(), tensor);
    }

    // 5.2 bind KV cache outputs directly.
    //   present_key_values 输出直接写入 KVCacheManager buffer，
    BindKVCacheDirect(binding, outputNames, totalSeqLen, memInfo);

    // 5.2b bind opaque recurrent state outputs (conv_state / recurrent_state).
    //   ORT will allocate and fill them; we read them back in 5.5.
    for (const auto &st : m_opaqueStates) {
      if (!st.out_name.empty()) {
        binding.BindOutput(st.out_name.c_str(), memInfo);
      }
    }

    // 5.3 bind logits and other non-KV, non-opaque outputs (let ORT handle allocation)
    for (const auto &name : outputNames) {
      bool is_kv = name.find("present") != std::string::npos || name.find("key_values") != std::string::npos;
      bool is_opaque = false;
      for (const auto &st : m_opaqueStates) {
        if (st.out_name == name) {
          is_opaque = true;
          break;
        }
      }
      if (!is_kv && !is_opaque) {
        binding.BindOutput(name.c_str(), memInfo);
      }
    }

    // 5.4 execute inference
    try {
      m_session->Run(Ort::RunOptions{nullptr}, binding);
    } catch (const Ort::Exception &e) {
      DBUG_PRINT("error", ("ORT Run failed at step %d: %s", step, e.what()));
      break;
    }

    // 5.5 update KV cache seq counters (ORT already wrote present KV to cache buffer)
    UpdateCacheSeqCounters(totalSeqLen);

    // 5.5b propagate opaque recurrent states for the next step
    UpdateOpaqueStatesFromOutputs(binding, outputNames, memInfo);

    // 5.6 dealing with logits
    auto outputValues = binding.GetOutputValues();
    auto outputValNames = binding.GetOutputNames();

    // check logits tensor
    size_t logitsTensorIdx = outputValues.size();  // sentinel value for "not found"
    for (size_t oi = 0; oi < outputValNames.size(); ++oi) {
      if (outputValNames[oi] == "logits") {
        logitsTensorIdx = oi;
        break;
      }
    }
    if (logitsTensorIdx == outputValues.size()) {
      for (size_t oi = 0; oi < outputValues.size(); ++oi) {
        if (!outputValues[oi]) continue;
        auto info = outputValues[oi].GetTensorTypeAndShapeInfo();
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) continue;
        auto sh = info.GetShape();
        if (!sh.empty() && sh.back() > 10000) {
          logitsTensorIdx = oi;
          break;
        }
      }
    }
    if (logitsTensorIdx == outputValues.size()) break;

    const Ort::Value &logitsTensor = outputValues[logitsTensorIdx];
    auto logitsInfo = logitsTensor.GetTensorTypeAndShapeInfo();
    auto logitsShape = logitsInfo.GetShape();
    const float *logitsData = logitsTensor.GetTensorData<float>();
    int64_t vocabSize = logitsShape.back();

    // the last logits slice corresponds to the next token prediction, with shape [vocab_size]. If the logits tensor has
    // more than 2 dims, we need to calculate the offset to get to that slice.
    size_t logitsOffset = 0;
    if (logitsShape.size() >= 2) {
      size_t seqLen = static_cast<size_t>(logitsShape[logitsShape.size() - 2]);
      logitsOffset = (seqLen - 1) * static_cast<size_t>(vocabSize);
    }

    std::vector<float> currentLogits(logitsData + logitsOffset, logitsData + logitsOffset + vocabSize);

#ifndef NDEBUG
    PrintTopKLogits(currentLogits, 5);
#endif

    // 5.7 apply punishment
    ApplyRepeatPenalty(currentLogits.data(), vocabSize, newTokens, m_gen_option.repeat_penalty);
    ApplyFrequencyPenalty(currentLogits.data(), vocabSize, m_gen_option.frequency_penalty);
    ApplyPresencePenalty(currentLogits.data(), vocabSize, m_gen_option.presence_penalty);

    // 5.8 sampling
    int64_t nextToken;
    if (m_gen_option.top_k > 0 && m_gen_option.top_p < 1.0f) {
      nextToken = SampleTopKThenTopP(currentLogits.data(), vocabSize, m_gen_option.top_k, m_gen_option.top_p,
                                     m_gen_option.temperature);
    } else if (m_gen_option.top_k > 0) {
      nextToken = SampleTopK(currentLogits.data(), vocabSize, m_gen_option.top_k, m_gen_option.temperature);
    } else if (m_gen_option.top_p < 1.0f) {
      nextToken = SampleTopP(currentLogits.data(), vocabSize, m_gen_option.top_p, m_gen_option.temperature);
    } else {
      nextToken = SampleWithTemperature(currentLogits.data(), vocabSize, m_gen_option.temperature);
    }

    // 5.9 stop condition check.
    generatedTokens.push_back(nextToken);
    newTokens.push_back(nextToken);
    UpdateTokenTracking(nextToken);

    bool eosHit = std::find(m_stopTokenIds.begin(), m_stopTokenIds.end(), nextToken) != m_stopTokenIds.end();
    bool minReached = (static_cast<int>(newTokens.size()) >= m_gen_option.min_new_tokens);
    if (minReached && eosHit) break;
    if (!minReached && eosHit) continue;
    // Only fall through to the expensive string decode for custom stop sequences
    if (minReached && ShouldStop(newTokens, m_gen_option.stop_sequences)) break;
  }

  // 6. decoding and return result
  if (!newTokens.empty()) {
    std::vector<uint32_t> decodeTokens;
    decodeTokens.reserve(newTokens.size());
    for (auto t : newTokens) decodeTokens.push_back(static_cast<uint32_t>(t));
    result.output = m_tokenizer->decode(decodeTokens, true);
  } else {
    result.output = "[No tokens generated: possibly EOS or empty output]";
  }

  result.tokens = std::move(generatedTokens);
  return result;
}
}  // namespace LLM_Generate
}  // namespace ML
}  // namespace ShannonBase