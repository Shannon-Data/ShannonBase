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

namespace ShannonBase {
namespace ML {
namespace LLM_Generate {

// CPUDetector Implementation
std::string CPUDetector::getCPUInfo() const {
  std::string info = "CPU Features: ";
  if (m_features.arm64) info += "ARM64 ";
  if (m_features.avx2) info += "AVX2 ";
  if (m_features.avx512f) info += "AVX512F ";
  if (m_features.avx512vnni) info += "AVX512-VNNI ";
  if (m_features.avx512bw) info += "AVX512BW ";
  if (m_features.avx512dq) info += "AVX512DQ ";
  if (m_features.fma) info += "FMA ";
  if (m_features.fp16) info += "FP16 ";
  return info.empty() ? "CPU Features: None detected" : info;
}

void CPUDetector::cpuid(std::array<int, 4> &regs, int funcId, int subFuncId) const noexcept {
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

void CPUDetector::detectFeatures() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
  m_features.arm64 = true;
#if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC)
  m_features.fp16 = true;
#endif
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

// Model Selection Implementation
ModelSelection select_model_variant(const std::string &model_dir, const std::string &user_precision,
                                    const std::string &) {
  ModelSelection result;
  result.device = Device::CPU;
  result.precision = Precision::FP32;
  result.variant = "base";

  auto exists = [&](const std::string &fname) {
    return fs::exists(fs::path(model_dir) / fname) && (fs::exists(fs::path(model_dir) / (fname + "_data")) ||
                                                       fs::file_size(fs::path(model_dir) / fname) < 2147483648);
  };

  CPUDetector cpu;

  // User-specified precision takes priority
  if (!user_precision.empty()) {
    if (user_precision == "fp32" && exists("model.onnx")) {
      result.filename = "model.onnx";
      result.precision = Precision::FP32;
      result.variant = "fp32";
      result.estimated_memory_gb = 12.9;
      return result;
    } else if (user_precision == "fp16" && exists("model_fp16.onnx")) {
      result.filename = "model_fp16.onnx";
      result.precision = Precision::FP16;
      result.variant = "fp16";
      result.estimated_memory_gb = 6.43;
      return result;
    } else if (user_precision == "int8" && exists("model_int8.onnx")) {
      result.filename = "model_int8.onnx";
      result.precision = Precision::INT8;
      result.variant = "int8";
      result.estimated_memory_gb = 3.21;
      return result;
    } else if (user_precision == "q4" && exists("model_q4.onnx")) {
      result.filename = "model_q4.onnx";
      result.precision = Precision::QINT4;
      result.variant = "q4";
      result.estimated_memory_gb = 3.34;
      return result;
    }
  }

  // Auto-selection based on CPU capabilities
  if (cpu.isARM64()) {
    if (cpu.hasFP16() && exists("model_fp16.onnx")) {
      result.filename = "model_fp16.onnx";
      result.precision = Precision::FP16;
      result.variant = "fp16_arm";
      result.estimated_memory_gb = 6.43;
    } else if (exists("model_int8.onnx")) {
      result.filename = "model_int8.onnx";
      result.precision = Precision::INT8;
      result.variant = "int8_arm";
      result.estimated_memory_gb = 3.21;
    } else if (exists("model_q4.onnx")) {
      result.filename = "model_q4.onnx";
      result.precision = Precision::QINT4;
      result.variant = "q4_arm";
      result.estimated_memory_gb = 3.34;
    }
  } else if (cpu.hasAVX512VNNI()) {
    if (exists("model_int8.onnx")) {
      result.filename = "model_int8.onnx";
      result.precision = Precision::INT8;
      result.variant = "int8_avx512_vnni";
      result.estimated_memory_gb = 3.21;
    }
  } else if (cpu.hasAVX512F()) {
    if (exists("model_fp16.onnx")) {
      result.filename = "model_fp16.onnx";
      result.precision = Precision::FP16;
      result.variant = "fp16_avx512";
      result.estimated_memory_gb = 6.43;
    }
  } else if (cpu.hasAVX2()) {
    if (exists("model_int8.onnx")) {
      result.filename = "model_int8.onnx";
      result.precision = Precision::INT8;
      result.variant = "int8_avx2";
      result.estimated_memory_gb = 3.21;
    }
  }

  // Fallback selection
  if (result.filename.empty()) {
    if (exists("model_fp16.onnx")) {
      result.filename = "model_fp16.onnx";
      result.precision = Precision::FP16;
      result.variant = "fp16_fallback";
      result.estimated_memory_gb = 6.43;
    } else if (exists("model_int8.onnx")) {
      result.filename = "model_int8.onnx";
      result.precision = Precision::INT8;
      result.variant = "int8_fallback";
      result.estimated_memory_gb = 3.21;
    } else if (exists("model.onnx")) {
      result.filename = "model.onnx";
      result.precision = Precision::FP32;
      result.variant = "fp32_fallback";
      result.estimated_memory_gb = 12.9;
    }
  }

  return result;
}

std::mt19937 g_rng(std::random_device{}());

// TextGenerator Implementation
TextGenerator::TextGenerator(const std::string &modelPath, const std::string &tokenizerPath,
                             const GenerationOptions &option)
    : m_gen_option(option), m_modelPath(modelPath), m_tokenizerPath(tokenizerPath), m_modelType(option.model_id) {
  auto ms = select_model_variant(m_modelPath);
  m_modelPath = (fs::path(m_modelPath) / ms.filename).string();
  m_initialized = !(InitializeONNX() || InitializeTokenizer() || LoadTokenizerConfig());
}

TextGenerator::~TextGenerator() {
  m_session.reset();
  m_sessionOptions.reset();
  m_env.reset();
}

bool TextGenerator::InitializeONNX() {
  m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "TextGenerator");
  m_sessionOptions = std::make_unique<Ort::SessionOptions>();

  int numThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  m_sessionOptions->SetIntraOpNumThreads(numThreads);
  m_sessionOptions->SetInterOpNumThreads(1);
  m_sessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  m_sessionOptions->EnableMemPattern();
  m_sessionOptions->EnableCpuMemArena();

#ifdef _WIN32
  std::wstring wModelPath(m_modelPath.begin(), m_modelPath.end());
  m_session = std::make_unique<Ort::Session>(*m_env, wModelPath.c_str(), *m_sessionOptions);
#else
  m_session = std::make_unique<Ort::Session>(*m_env, m_modelPath.c_str(), *m_sessionOptions);
#endif
  return false;
}

bool TextGenerator::InitializeTokenizer() {
  auto token_path = fs::path(m_tokenizerPath) / "tokenizer.json";
  m_tokenizer = std::make_shared<tokenizers::Tokenizer>(token_path.string());
  if (!m_tokenizer->is_valid()) return true;
  return false;
}

bool TextGenerator::LoadTokenizerConfig() {
  fs::path configPath = fs::path(m_tokenizerPath) / "tokenizer_config.json";
  std::ifstream configFile(configPath);
  if (!configFile.is_open()) return true;

  std::string jsonStr((std::istreambuf_iterator<char>(configFile)), std::istreambuf_iterator<char>());
  configFile.close();

  rapidjson::Document config;
  if (config.Parse(jsonStr.c_str()).HasParseError()) return true;

  auto extractTokenId = [&](const char *tokenName) -> std::optional<int64_t> {
    if (config.HasMember(tokenName)) {
      const rapidjson::Value &token = config[tokenName];
      if (token.IsObject() && token.HasMember("id") && token["id"].IsInt64()) {
        return token["id"].GetInt64();
      } else if (token.IsInt64()) {
        return token.GetInt64();
      }
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

  return false;
}

std::string TextGenerator::ApplyChatTemplate(const std::string &userInput) {
  std::string modelTypeLower = m_modelType;
  std::transform(modelTypeLower.begin(), modelTypeLower.end(), modelTypeLower.begin(), ::tolower);

  if (modelTypeLower.find("llama") != std::string::npos) {
    if (modelTypeLower.find("llama-3") != std::string::npos || modelTypeLower.find("llama3") != std::string::npos ||
        modelTypeLower.find("llama-3.2") != std::string::npos || modelTypeLower.find("llama3.2") != std::string::npos) {
      return "<|begin_of_text|><|start_header_id|>user<|end_header_id|>\n\n" + userInput +
             "<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n";
    }
  } else if (modelTypeLower.find("mistral") != std::string::npos) {
    return "<s>[INST] " + userInput + " [/INST]";
  } else if (modelTypeLower.find("codellama") != std::string::npos) {
    return "<s>[INST] " + userInput + " [/INST]";
  } else if (modelTypeLower.find("phi") != std::string::npos) {
    return "Instruct: " + userInput + "\nOutput:";
  } else if (modelTypeLower.find("gemma") != std::string::npos) {
    return "<bos><start_of_turn>user\n" + userInput + "<end_of_turn>\n<start_of_turn>model\n";
  }
  return userInput;
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
    auto elementType = shapeInfo.GetElementType();

    std::cout << "input" << i << " : " << inputName << std::endl;
    std::cout << "  share: [";
    for (size_t j = 0; j < shape.size(); ++j) {
      if (j > 0) std::cout << ", ";
      if (shape[j] == -1) {
        std::cout << "state";
      } else {
        std::cout << shape[j];
      }
    }
    std::cout << "]" << std::endl;

    std::cout << "  data type: " << elementType << std::endl;

    if (inputName.find("past_key_values") != std::string::npos) {
      auto [layerIdx, isKey] = ParseKVCacheInputName(inputName);
      std::cout << "  KV Cache -> Layer: " << layerIdx << ", Type: " << (isKey ? "Key" : "Value") << std::endl;

      if (shape.size() >= 4) {
        std::cout << "  Dimension:" << std::endl;
        std::cout << "    Batch (dim 0): " << shape[0] << std::endl;
        std::cout << "    KV Heads (dim 1): " << shape[1] << std::endl;
        std::cout << "    Seq Length (dim 2): " << shape[2] << std::endl;
        std::cout << "    Head Dim (dim 3): " << shape[3] << std::endl;

        if (shape[3] > 0) {
          std::cout << "  [OK] Fixed head_dim: " << shape[3] << std::endl;
        } else {
          std::cout << "  [failed] head_dim dynamic, need to do ananlysis more" << std::endl;
        }
      }
    }
    std::cout << std::endl;
  }

  std::cout << "=== Done ===" << std::endl;
}

std::string TextGenerator::NormalizeModelType(const std::string &modelType) const {
  std::string normalized = modelType;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);

  std::vector<std::string> suffixes = {"-3b", "-7b", "-13b", "-70b", "-instruct", "-chat", "-base"};
  for (const auto &suffix : suffixes) {
    size_t pos = normalized.find(suffix);
    if (pos != std::string::npos) {
      normalized = normalized.substr(0, pos);
      break;
    }
  }

  std::regex versionRegex(R"((\d+)\.(\d+))");
  std::smatch match;
  if (std::regex_search(normalized, match, versionRegex)) {
    std::string version = match.str();
    if (version == "3.2") normalized = std::regex_replace(normalized, std::regex(R"(3\.2)"), "3.2");
  }

  return normalized;
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

void TextGenerator::DetectQueryHeadsFromOutputs(const std::vector<std::string> &outputNames) {
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

  // For Llama-3.2-3B specifically, use the correct configuration
  if (m_numQueryHeads == 0 && m_numKVHeads == 8) {
    // Llama-3.2-3B has 24 query heads and 8 KV heads (3:1 ratio)
    if (m_modelType.find("llama-3.2") != std::string::npos || m_modelType.find("llama3.2") != std::string::npos) {
      m_numQueryHeads = 24;  // Correct value for Llama-3.2-3B
    } else {                 // General fallback
      m_numQueryHeads = m_numKVHeads * 3;
    }
  }
}

void TextGenerator::DetectModelArchitecture(const std::vector<std::string> &inputNames,
                                            const std::vector<std::string> &outputNames) {
  m_numLayers = 0;
  m_numQueryHeads = 0;
  m_numKVHeads = 0;
  m_headDim = 0;

  Ort::AllocatorWithDefaultOptions allocator;

  // step 1:detect KV cache params.
  std::set<size_t> detectedLayers;

  for (size_t i = 0; i < m_session->GetInputCount(); ++i) {
    Ort::AllocatedStringPtr inputNamePtr = m_session->GetInputNameAllocated(i, allocator);
    if (!inputNamePtr) continue;
    std::string inputName = inputNamePtr.get();

    if (inputName.find("past_key_values") != std::string::npos || inputName.find("past_key") != std::string::npos ||
        inputName.find("cache") != std::string::npos) {
      Ort::TypeInfo typeInfo = m_session->GetInputTypeInfo(i);
      auto shapeInfo = typeInfo.GetTensorTypeAndShapeInfo();
      auto shape = shapeInfo.GetShape();

      // parse index layer.
      auto [layerIdx, isKey] = ParseKVCacheInputName(inputName);
      detectedLayers.insert(layerIdx);

      // param detect (shape : [batch, kv_heads, seq_len, head_dim])
      if (shape.size() >= 4) {  // KV heads (no 2 dim, index 1)
        if (shape[1] > 0 && m_numKVHeads == 0) m_numKVHeads = static_cast<size_t>(shape[1]);
        // Head dimension (no.4 dim, index:3) - key checkpoint.
        if (shape[3] > 0 && m_headDim == 0) m_headDim = static_cast<size_t>(shape[3]);
      }
    }
  }

  // # of layers.
  if (!detectedLayers.empty()) {
    m_numLayers = *detectedLayers.rbegin() + 1;  // max # index layer + 1
  }

  // step 2:detect the missing param.
  GetModelMetadata();

  // step3: output sharpe query heads
  if (m_numQueryHeads == 0) DetectQueryHeadsFromOutputs(outputNames);

  // step 4: no param detected, using assumption.
  if (m_numQueryHeads == 0 && m_numKVHeads > 0) {
    if (m_modelType.find("llama-3") != std::string::npos || m_modelType.find("llama3") != std::string::npos)
      m_numQueryHeads = m_numKVHeads * 3;  // Llama-3, 3:1
    else
      m_numQueryHeads = m_numKVHeads;  // default 1:1
  }

  // step 5: check the results.
  if (m_numLayers == 0 || m_numKVHeads == 0 || m_headDim == 0) {
    if (m_headDim == 0) m_headDim = (m_modelType.find("llama") != std::string::npos) ? 128 : 64;
  }
}

void TextGenerator::InitializeKVCache() {
  if (m_numLayers == 0 || m_numKVHeads == 0 || m_headDim == 0) {
    return;
  }

  m_keyCache.clear();
  m_valueCache.clear();
  m_keyCache.resize(m_numLayers);
  m_valueCache.resize(m_numLayers);

  for (auto &cache : m_keyCache) cache.clear();
  for (auto &cache : m_valueCache) cache.clear();

  m_kvCacheInitialized = true;
}

void TextGenerator::ClearKVCache() {
  for (auto &cache : m_keyCache) cache.clear();
  for (auto &cache : m_valueCache) cache.clear();
  m_kvCacheInitialized = false;
}

std::pair<size_t, bool> TextGenerator::ParseKVCacheInputName(const std::string &name) const {
  size_t layerIdx = 0;
  bool isKey = true;

  std::vector<std::regex> layerPatterns = {std::regex(R"(\.(\d+)\.)"),    std::regex(R"(_(\d+)_)"),
                                           std::regex(R"(layer\.(\d+))"), std::regex(R"(layers\.(\d+))"),
                                           std::regex(R"(h\.(\d+))"),     std::regex(R"(block\.(\d+))")};

  for (const auto &pattern : layerPatterns) {
    std::smatch match;
    if (std::regex_search(name, match, pattern) && match.size() > 1) {
      layerIdx = static_cast<size_t>(std::stoi(match[1].str()));
      break;
    }
  }

  if (name.find("value") != std::string::npos || name.find("val") != std::string::npos) {
    isKey = false;
  } else if (name.find("key") != std::string::npos) {
    isKey = true;
  }

  return {layerIdx, isKey};
}

std::pair<size_t, bool> TextGenerator::ParseKVCacheOutputName(const std::string &name) const {
  return ParseKVCacheInputName(name);
}

void TextGenerator::UpdateKVCache(const std::vector<Ort::Value> &outputTensors,
                                  const std::vector<std::string> &outputNames, const Ort::MemoryInfo &memInfo) {
  for (size_t i = 1; i < outputTensors.size(); ++i) {
    const std::string &outputName = outputNames[i];
    if (outputName.find("present") == std::string::npos && outputName.find("key_values") == std::string::npos) {
      continue;
    }

    auto [layerIdx, isKey] = ParseKVCacheOutputName(outputName);
    if (layerIdx < m_keyCache.size()) {
      const float *data = outputTensors[i].GetTensorData<float>();
      auto shapeInfo = outputTensors[i].GetTensorTypeAndShapeInfo();
      size_t elementCount = shapeInfo.GetElementCount();

      auto &cache = isKey ? m_keyCache[layerIdx] : m_valueCache[layerIdx];
      if (cache.size() != elementCount) {
        cache.resize(elementCount);
      }
      std::memcpy(cache.data(), data, elementCount * sizeof(float));
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
  if (temperature <= 0.0f) {
    return Argmax(logits, vocabSize);
  }

  std::vector<float> scaledLogits(vocabSize);
  for (size_t i = 0; i < vocabSize; ++i) {
    scaledLogits[i] = logits[i] / temperature;
  }

  // Softmax
  float maxLogit = *std::max_element(scaledLogits.begin(), scaledLogits.end());
  float sum = 0.0f;
  for (size_t i = 0; i < vocabSize; ++i) {
    scaledLogits[i] = std::exp(scaledLogits[i] - maxLogit);
    sum += scaledLogits[i];
  }

  for (size_t i = 0; i < vocabSize; ++i) {
    scaledLogits[i] /= sum;
  }

  // random sampling
  std::discrete_distribution<int64_t> dist(scaledLogits.begin(), scaledLogits.end());
  return dist(g_rng);
}

int64_t TextGenerator::SampleTopK(const float *logits, size_t vocabSize, int topK, float temperature) {
  std::vector<std::pair<float, int64_t>> logitPairs;
  logitPairs.reserve(vocabSize);

  for (size_t i = 0; i < vocabSize; ++i) {
    logitPairs.emplace_back(logits[i], static_cast<int64_t>(i));
  }

  // sort by logit value desc
  std::partial_sort(logitPairs.begin(), logitPairs.begin() + std::min(topK, static_cast<int>(vocabSize)),
                    logitPairs.end(), std::greater<>());

  // only keep top-k
  int actualK = std::min(topK, static_cast<int>(vocabSize));
  std::vector<float> topKLogits(actualK);
  std::vector<int64_t> topKIndices(actualK);

  for (int i = 0; i < actualK; ++i) {
    topKLogits[i] = logitPairs[i].first;
    topKIndices[i] = logitPairs[i].second;
  }

  // in top-k with temp sampling.
  int64_t selectedIdx = SampleWithTemperature(topKLogits.data(), actualK, temperature);
  return topKIndices[selectedIdx];
}

int64_t TextGenerator::SampleTopP(const float *logits, size_t vocabSize, float topP, float temperature) {
  std::vector<std::pair<float, int64_t>> logitPairs;
  logitPairs.reserve(vocabSize);

  for (size_t i = 0; i < vocabSize; ++i) {
    logitPairs.emplace_back(logits[i], static_cast<int64_t>(i));
  }

  std::sort(logitPairs.begin(), logitPairs.end(), std::greater<>());

  // calc softmax prob
  float maxLogit = logitPairs[0].first;
  float sum = 0.0f;
  std::vector<float> probs(vocabSize);

  for (size_t i = 0; i < vocabSize; ++i) {
    probs[i] = std::exp(logitPairs[i].first - maxLogit);
    sum += probs[i];
  }

  for (size_t i = 0; i < vocabSize; ++i) {
    probs[i] /= sum;
  }

  // find cumulateive top_p pos.
  float cumulativeProb = 0.0f;
  size_t cutoff = vocabSize;

  for (size_t i = 0; i < vocabSize; ++i) {
    cumulativeProb += probs[i];
    if (cumulativeProb >= topP) {
      cutoff = i + 1;
      break;
    }
  }

  // normalize the tokens
  std::vector<float> selectedLogits(cutoff);
  std::vector<int64_t> selectedIndices(cutoff);

  for (size_t i = 0; i < cutoff; ++i) {
    selectedLogits[i] = logitPairs[i].first;
    selectedIndices[i] = logitPairs[i].second;
  }

  int64_t selectedIdx = SampleWithTemperature(selectedLogits.data(), cutoff, temperature);
  return selectedIndices[selectedIdx];
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
  if (stopSequences.empty()) return false;

  // decode recent tokens
  size_t checkLength = std::min(tokens.size(), size_t(50));  // check the lastest 50 tokens
  std::vector<uint32_t> recentTokens;
  for (size_t i = tokens.size() - checkLength; i < tokens.size(); ++i) {
    recentTokens.push_back(static_cast<uint32_t>(tokens[i]));
  }

  std::string recentText = m_tokenizer->decode(recentTokens, true);

  for (const auto &stopSeq : stopSequences) {
    if (recentText.find(stopSeq) != std::string::npos) {
      return true;
    }
  }

  return false;
}

void TextGenerator::InitializeTokenTracking(size_t vocabSize) {
  m_tokenFrequency.assign(vocabSize, 0);
  m_tokenPresence.assign(vocabSize, 0);
}

void TextGenerator::UpdateTokenTracking(int64_t token) {
  if (token >= 0 && token < static_cast<int64_t>(m_tokenFrequency.size())) {
    m_tokenFrequency[token]++;
    m_tokenPresence[token] = 1;
  }
}

TextGenerator::Result TextGenerator::Generate(const std::string &userPrompt, int maxNewTokens) {
  Result result;
  if (!Initialized() || !m_gen_option.validate()) return result;

  m_stepFloatBuffers.clear();
  m_stepInt64Buffers.clear();

  if (m_shouldClearKVCache) {
    ClearKVCache();
    m_shouldClearKVCache = false;
  }

  std::string inputText;
  if (m_gen_option.task == "summarization") {
    inputText = ApplyChatTemplate("Please summarize the following text: " + userPrompt);
  } else {
    inputText = ApplyChatTemplate(userPrompt);
  }

  // encoding the input text into token ids.
  auto encoding = m_tokenizer->encode(inputText, true);
  std::vector<uint32_t> inputIds = encoding.ids();
  if (inputIds.empty()) return result;

  std::vector<int64_t> inputIds64;
  inputIds64.reserve(inputIds.size());
  for (auto id : inputIds) {
    inputIds64.push_back(static_cast<int64_t>(id));
  }

  InitializeTokenTracking(m_vocabularySize > 0 ? m_vocabularySize : 50000);  // default 50k vocabulary.

  Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::AllocatorWithDefaultOptions allocator;

  std::vector<std::string> inputNames, outputNames;
  for (size_t i = 0; i < m_session->GetInputCount(); ++i) {
    inputNames.emplace_back(m_session->GetInputNameAllocated(i, allocator).get());
  }
  for (size_t i = 0; i < m_session->GetOutputCount(); ++i) {
    outputNames.emplace_back(m_session->GetOutputNameAllocated(i, allocator).get());
  }

  if (m_numLayers == 0 || !m_kvCacheInitialized) {
    DetectModelArchitecture(inputNames, outputNames);
    InitializeKVCache();
  }

  std::vector<const char *> inputNamesPtrs, outputNamesPtrs;
  for (auto &n : inputNames) inputNamesPtrs.push_back(n.c_str());
  for (auto &n : outputNames) outputNamesPtrs.push_back(n.c_str());

  std::vector<int64_t> generatedTokens(inputIds64);
  std::vector<int64_t> newTokens;
  newTokens.reserve(maxNewTokens);

  for (int step = 0; step < maxNewTokens; ++step) {
    m_stepFloatBuffers.clear();
    m_stepInt64Buffers.clear();

    std::vector<Ort::Value> stepInputs;
    stepInputs.reserve(inputNames.size());

    size_t pastSeqLen = (step == 0) ? 0 : generatedTokens.size() - 1;
    size_t currentInputLen = (step == 0) ? generatedTokens.size() : 1;

    // Process inputs in the exact order expected by the model
    for (size_t inputIdx = 0; inputIdx < inputNames.size(); ++inputIdx) {
      const std::string &inputName = inputNames[inputIdx];

      if (inputName == "input_ids" || inputName.find("input") != std::string::npos) {
        std::vector<int64_t> currentInput;
        if (step == 0) {
          currentInput = generatedTokens;
        } else {
          currentInput = {generatedTokens.back()};
        }

        m_stepInt64Buffers.push_back(std::move(currentInput));
        auto &buf = m_stepInt64Buffers.back();
        std::vector<int64_t> shape = {1, static_cast<int64_t>(buf.size())};
        stepInputs.emplace_back(
            Ort::Value::CreateTensor<int64_t>(memInfo, buf.data(), buf.size(), shape.data(), shape.size()));

      } else if (inputName == "attention_mask" || inputName.find("attention") != std::string::npos) {
        size_t fullSeqLen = pastSeqLen + currentInputLen;
        std::vector<int64_t> attentionMask(fullSeqLen, 1);

        m_stepInt64Buffers.push_back(std::move(attentionMask));
        auto &buf = m_stepInt64Buffers.back();
        std::vector<int64_t> maskShape = {1, static_cast<int64_t>(buf.size())};
        stepInputs.emplace_back(
            Ort::Value::CreateTensor<int64_t>(memInfo, buf.data(), buf.size(), maskShape.data(), maskShape.size()));

      } else if (inputName == "position_ids" || inputName.find("position") != std::string::npos) {
        std::vector<int64_t> positionIds;

        if (step == 0) {
          positionIds.resize(currentInputLen);
          for (size_t i = 0; i < currentInputLen; ++i) {
            positionIds[i] = i;
          }
        } else {
          positionIds = {static_cast<int64_t>(pastSeqLen + currentInputLen - 1)};
        }

        m_stepInt64Buffers.push_back(std::move(positionIds));
        auto &buf = m_stepInt64Buffers.back();
        std::vector<int64_t> shape = {1, static_cast<int64_t>(buf.size())};
        stepInputs.emplace_back(
            Ort::Value::CreateTensor<int64_t>(memInfo, buf.data(), buf.size(), shape.data(), shape.size()));

      } else if (inputName.find("past_key_values") != std::string::npos) {
        // Create KV cache input for this specific layer and type
        auto [layerIdx, isKey] = ParseKVCacheInputName(inputName);

        // Determine the sequence length for this cache tensor
        size_t cacheSeqLen = pastSeqLen;  // Previous tokens for past cache

        std::vector<int64_t> cacheShape = {
            1,                                   // batch size
            static_cast<int64_t>(m_numKVHeads),  // KV heads
            static_cast<int64_t>(cacheSeqLen),   // sequence length
            static_cast<int64_t>(m_headDim)      // head dimension
        };

        size_t elementCount = GetElementCount(cacheShape);

        // Create empty cache or use existing
        std::vector<float> cacheData;
        if (elementCount > 0 && step > 0 && layerIdx < m_keyCache.size()) {
          // Use existing cache from previous step
          const auto &existingCache = isKey ? m_keyCache[layerIdx] : m_valueCache[layerIdx];
          if (!existingCache.empty() && existingCache.size() == elementCount) {
            cacheData = existingCache;
          } else
            cacheData.resize(elementCount, 0.0f);
        } else  // First step or empty cache
          cacheData.resize(elementCount, 0.0f);

        m_stepFloatBuffers.push_back(std::move(cacheData));
        auto &buf = m_stepFloatBuffers.back();
        stepInputs.emplace_back(
            Ort::Value::CreateTensor<float>(memInfo, buf.data(), buf.size(), cacheShape.data(), cacheShape.size()));
      } else {
        // Unknown input - create default
        std::vector<int64_t> defaultValue = {0};
        m_stepInt64Buffers.push_back(std::move(defaultValue));
        auto &buf = m_stepInt64Buffers.back();
        std::vector<int64_t> shape = {1, 1};
        stepInputs.emplace_back(
            Ort::Value::CreateTensor<int64_t>(memInfo, buf.data(), buf.size(), shape.data(), shape.size()));
      }
    }

    // Verify we have the correct number of inputs
    if (stepInputs.size() != inputNames.size()) break;

    auto outputTensors = m_session->Run(Ort::RunOptions{nullptr}, inputNamesPtrs.data(), stepInputs.data(),
                                        stepInputs.size(), outputNamesPtrs.data(), outputNamesPtrs.size());

    if (outputTensors.empty()) break;

    const Ort::Value &logitsTensor = outputTensors[0];
    auto logitsInfo = logitsTensor.GetTensorTypeAndShapeInfo();
    auto logitsShape = logitsInfo.GetShape();
    const float *logitsData = logitsTensor.GetTensorData<float>();

    int64_t vocabSize = logitsShape.back();
    size_t logitsOffset = 0;
    if (logitsShape.size() >= 2) {
      size_t seqLen = static_cast<size_t>(logitsShape[logitsShape.size() - 2]);
      logitsOffset = (seqLen - 1) * static_cast<size_t>(vocabSize);
    }

    std::vector<float> currentLogits(logitsData + logitsOffset, logitsData + logitsOffset + vocabSize);

    // Apply penalty strategies.
    ApplyRepeatPenalty(currentLogits.data(), vocabSize, generatedTokens, m_gen_option.repeat_penalty);
    ApplyFrequencyPenalty(currentLogits.data(), vocabSize, m_gen_option.frequency_penalty);
    ApplyPresencePenalty(currentLogits.data(), vocabSize, m_gen_option.presence_penalty);

    // According to the options values to choose sample strategy.
    int64_t nextToken;
    if (m_gen_option.top_k > 0 && m_gen_option.top_p < 1.0f) {
      // top-k and top-p
      nextToken = SampleTopK(currentLogits.data(), vocabSize, m_gen_option.top_k, m_gen_option.temperature);
    } else if (m_gen_option.top_k > 0) {
      nextToken = SampleTopK(currentLogits.data(), vocabSize, m_gen_option.top_k, m_gen_option.temperature);
    } else if (m_gen_option.top_p < 1.0f) {
      nextToken = SampleTopP(currentLogits.data(), vocabSize, m_gen_option.top_p, m_gen_option.temperature);
    } else {
      nextToken = SampleWithTemperature(currentLogits.data(), vocabSize, m_gen_option.temperature);
    }

    // Check EOS or stop serial.
    if (nextToken == m_eosTokenId) break;

    generatedTokens.push_back(nextToken);
    newTokens.push_back(nextToken);
    UpdateTokenTracking(nextToken);
    if (ShouldStop(generatedTokens, m_gen_option.stop_sequences)) {
      break;
    }
    UpdateKVCache(outputTensors, outputNames, memInfo);
  }

  if (!newTokens.empty()) {
    std::vector<uint32_t> decodeTokens;
    decodeTokens.reserve(newTokens.size());
    for (auto token : newTokens) {
      decodeTokens.push_back(static_cast<uint32_t>(token));
    }
    result.output = m_tokenizer->decode(decodeTokens, true);
  } else  // No new tokens generated
    result.output = "";

  result.tokens = std::move(generatedTokens);
  return result;
}
}  // namespace LLM_Generate
}  // namespace ML
}  // namespace ShannonBase