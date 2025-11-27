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

#include "ml/infra_component/sentence_transform.h"

#include "sql/sql_class.h"
#include "sql/sql_optimizer.h"

namespace ShannonBase {
namespace ML {
namespace SentenceTransform {

ModelSelection select_model_variant(const std::string &model_dir, const std::string &user_precision,
                                    const std::string &user_opt) {
  ModelSelection result;
  result.device = Device::CPU;
  result.precision = Precision::FP32;
  result.opt_level = "auto";

  auto exists = [&](const std::string &fname) { return fs::exists(model_dir + "/" + fname); };

  if (!user_opt.empty()) {
    std::string fname = "model_" + user_opt + ".onnx";  // e.g. model_O3.onnx
    if (exists(fname)) {
      result.filename = fname;
      result.opt_level = user_opt;
      result.precision = Precision::FP32;
      return result;
    }
  }

  CPUDetector cpu;

  if (user_precision == "int8") {
    if (cpu.isARM64() && exists("model_qint8_arm64.onnx")) {
      result.filename = "model_qint8_arm64.onnx";
      result.precision = Precision::INT8;
      return result;
    }
    if (cpu.hasAVX512VNNI() && exists("model_qint8_avx512_vnni.onnx")) {
      result.filename = "model_qint8_avx512_vnni.onnx";
      result.precision = Precision::INT8;
      return result;
    }
    if (cpu.hasAVX512F() && exists("model_qint8_avx512.onnx")) {
      result.filename = "model_qint8_avx512.onnx";
      result.precision = Precision::INT8;
      return result;
    }
    if (cpu.hasAVX2() && exists("model_quint8_avx2.onnx")) {
      result.filename = "model_quint8_avx2.onnx";
      result.precision = Precision::INT8;
      return result;
    }
  }

  if (cpu.isARM64() && exists("model_qint8_arm64.onnx")) {
    result.filename = "model_qint8_arm64.onnx";
    result.precision = Precision::INT8;
  } else if (cpu.hasAVX512VNNI() && exists("model_qint8_avx512_vnni.onnx")) {
    result.filename = "model_qint8_avx512_vnni.onnx";
    result.precision = Precision::INT8;
  } else if (cpu.hasAVX512F() && exists("model_qint8_avx512.onnx")) {
    result.filename = "model_qint8_avx512.onnx";
    result.precision = Precision::INT8;
  } else if (cpu.hasAVX2() && exists("model_quint8_avx2.onnx")) {
    result.filename = "model_quint8_avx2.onnx";
    result.precision = Precision::INT8;
  } else if (exists("model_O3.onnx")) {
    result.filename = "model_O3.onnx";
    result.precision = Precision::FP32;
    result.opt_level = "O3";
  } else if (exists("model_O4.onnx")) {
    result.filename = "model_O4.onnx";
    result.precision = Precision::FP32;
    result.opt_level = "O4";
  } else if (exists("model.onnx")) {
    result.filename = "model.onnx";
    result.precision = Precision::FP32;
    result.opt_level = "base";
  } else {
    std::string err("no valid ONNX file found");
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return result;
  }

  return result;
}

MiniLMEmbedding::MiniLMEmbedding(const std::string &modelPath, const std::string &tokenizerPath)
    : m_modelPath(modelPath), m_tokenizerPath(tokenizerPath) {
  // 1. load Tokenizer(Huggingface Tokenizer FFI version)
  if (!m_tokenizerPath.empty()) {
    m_tokenizer = tokenizers::TokenizerUtils::load_from_file(m_tokenizerPath);
    if (!m_tokenizer || !m_tokenizer->is_valid()) {
      std::string error("Failed to load Tokenizer from: " + m_tokenizerPath);
      my_error(ER_ML_FAIL, MYF(0), error.c_str());
      return;
    }
  } else {
    my_error(ER_ML_FAIL, MYF(0), "Tokenizer path must be provided for MiniLMEmbedding");
    return;
  }

  // 2. select the matched model variant.
  auto ms = select_model_variant(m_modelPath);
  if (ms.filename.length())
    m_modelPath.append(ms.filename);
  else
    m_modelPath.append("model_O4.onnx");

  // 3. initialize ONNX Runtime.
  InitializeONNX();
}

void MiniLMEmbedding::InitializeONNX() {
  // Initialization ONNX Runtime
  m_ortEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "MiniLM");
  m_sessionOptions = std::make_unique<Ort::SessionOptions>();

  m_sessionOptions->SetIntraOpNumThreads(4);
  m_sessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

  // Load model
  m_ortSession = std::make_unique<Ort::Session>(*m_ortEnv, m_modelPath.c_str(), *m_sessionOptions);

  Ort::AllocatorWithDefaultOptions allocator;

  // Get input names
  size_t numInputNodes = m_ortSession->GetInputCount();
  m_inputNames.clear();
  for (size_t i = 0; i < numInputNodes; ++i) {
    auto inputName = m_ortSession->GetInputNameAllocated(i, allocator);
    m_inputNames.emplace_back(inputName.get());
  }

  // Get output names
  size_t numOutputNodes = m_ortSession->GetOutputCount();
  m_outputNames.clear();
  for (size_t i = 0; i < numOutputNodes; ++i) {
    auto outputName = m_ortSession->GetOutputNameAllocated(i, allocator);
    m_outputNames.emplace_back(outputName.get());
  }
}

MiniLMEmbedding::EmbeddingResult MiniLMEmbedding::EmbedText(const std::string &text) {
  EmbeddingResult result;
  result.text = text;
  result.confidence = 0.0;  // default set 0, set 1 on succeeded.

  tokenizers::Tokenizer::Encoding encoding(nullptr);
  EmbeddingVector embedding;

  // 1. Tokenization
  auto token_status = Tokenize(text, encoding);
  if (token_status != STATUS_T::OK) {
    std::string err = "Tokenization failed with error code: " + std::to_string(static_cast<int>(token_status));
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return result;
  }

  // 2. Convert encoding results to int64_t vectors
  const auto &source_input_ids = encoding.input_ids();
  const auto &source_attention_mask = encoding.attention_mask();
  const auto &source_token_type_ids = encoding.token_type_ids();

  std::vector<int64_t> input_ids(source_input_ids.size());
  std::vector<int64_t> attention_mask(source_attention_mask.size());
  std::vector<int64_t> token_type_ids(source_token_type_ids.size());

  for (uint32_t id : source_input_ids) input_ids.push_back(static_cast<int64_t>(id));
  for (uint32_t mask : source_attention_mask) attention_mask.push_back(static_cast<int64_t>(mask));
  for (uint32_t type_id : source_token_type_ids) token_type_ids.push_back(static_cast<int64_t>(type_id));

  // 3. Run Inference
  auto inference_status = RunInference(input_ids, attention_mask, token_type_ids, embedding);
  if (inference_status != STATUS_T::OK) {
    std::string err = "Inference failed with error code: " + std::to_string(static_cast<int>(inference_status));
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return result;
  }

  // 4. L2 Normalization
  if (!embedding.empty()) {
    NormalizeL2(embedding);
    result.embedding = std::move(embedding);
    result.confidence = 1.0;
  } else {
    my_error(ER_ML_FAIL, MYF(0), "Inference succeeded but returned empty vector");
    return result;
  }

  return result;
}

std::vector<MiniLMEmbedding::EmbeddingResult> MiniLMEmbedding::EmbedFile(const std::string &filePath,
                                                                         size_t maxChunkSize) {
  std::vector<EmbeddingResult> results;

  auto chunks = ReadAndChunkFile(filePath, maxChunkSize);
  results.reserve(chunks.size());

  for (size_t i = 0; i < chunks.size(); ++i) {
    if (!chunks[i].empty()) {
      auto result = EmbedText(chunks[i]);
      if (result.confidence > 0) results.push_back(std::move(result));
    }
  }

  return results;
}

// simple tokenize（in fact using HuggingFace tokenizer）
STATUS_T MiniLMEmbedding::Tokenize(const std::string &text, tokenizers::Tokenizer::Encoding &encoding_res) const {
  if (!m_tokenizer || !m_tokenizer->is_valid()) return STATUS_T::ERROR_MODEL_NOT_INIT;

  if (text.empty()) return STATUS_T::OK;

  // add_special_tokens=true make sure [CLS] and [SEP]
  auto encoding = m_tokenizer->encode(text, true);
  if (!encoding.is_valid()) {
    my_error(ER_ML_FAIL, MYF(0), "Tokenizer failed to encode text");
    return STATUS_T::ERROR_TOKENIZER_FAIL;
  }

  encoding_res = std::move(encoding);
  return STATUS_T::OK;
}

std::vector<MiniLMEmbedding::EmbeddingResult> MiniLMEmbedding::EmbedBatch(const std::vector<std::string> &texts) {
  std::vector<MiniLMEmbedding::EmbeddingResult> results;
  results.reserve(texts.size());
  for (const auto &text : texts) results.push_back(EmbedText(text));

  return results;
}

double MiniLMEmbedding::CosineSimilarity(const EmbeddingVector &a, const EmbeddingVector &b) {
  if (a.size() != b.size()) return 0.0;

  double dotProduct = 0.0, normA = 0.0, normB = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    dotProduct += a[i] * b[i];
    normA += a[i] * a[i];
    normB += b[i] * b[i];
  }

  if (normA == 0.0 || normB == 0.0) return 0.0;

  return dotProduct / (std::sqrt(normA) * std::sqrt(normB));
}

std::vector<std::pair<size_t, double>> MiniLMEmbedding::SemanticSearch(const EmbeddingVector &queryEmbedding,
                                                                       const std::vector<EmbeddingResult> &corpus,
                                                                       size_t topK) {
  std::vector<std::pair<size_t, double>> similarities;
  similarities.reserve(corpus.size());

  for (size_t i = 0; i < corpus.size(); ++i) {
    double similarity = CosineSimilarity(queryEmbedding, corpus[i].embedding);
    similarities.emplace_back(i, similarity);
  }

  std::partial_sort(similarities.begin(), similarities.begin() + std::min(topK, similarities.size()),
                    similarities.end(), [](const auto &a, const auto &b) { return a.second > b.second; });

  similarities.resize(std::min(topK, similarities.size()));
  return similarities;
}

STATUS_T MiniLMEmbedding::RunInference(const std::vector<int64_t> &input_ids,
                                       const std::vector<int64_t> &attention_mask,
                                       const std::vector<int64_t> &token_type_ids, EmbeddingVector &embeded_res) {
  embeded_res.clear();
  if (!m_ortSession || m_inputNames.empty()) return STATUS_T::ERROR_MODEL_NOT_INIT;

  const size_t sequenceLength = input_ids.size();
  if (sequenceLength == 0) return STATUS_T::ERROR_INVALID_INPUT;

  // ONNX tensor dim：[Batch Size, Sequence Length]
  std::array<int64_t, 2> inputShape = {1, static_cast<int64_t>(sequenceLength)};
  Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::vector<Ort::Value> inputTensors;

  // 1. create input_ids Tensor
  inputTensors.emplace_back(Ort::Value::CreateTensor<int64_t>(memoryInfo, const_cast<int64_t *>(input_ids.data()),
                                                              input_ids.size(), inputShape.data(), inputShape.size()));

  // 2. create attention mask Tensor
  if (m_inputNames.size() >= 2) {
    if (attention_mask.size() != sequenceLength) return STATUS_T::ERROR_INVALID_INPUT;
    inputTensors.emplace_back(
        Ort::Value::CreateTensor<int64_t>(memoryInfo, const_cast<int64_t *>(attention_mask.data()),
                                          attention_mask.size(), inputShape.data(), inputShape.size()));
  }

  // 3. create token_type_ids Tensor
  if (m_inputNames.size() >= 3) {
    if (token_type_ids.size() != sequenceLength) return STATUS_T::ERROR_INVALID_INPUT;
    inputTensors.emplace_back(
        Ort::Value::CreateTensor<int64_t>(memoryInfo, const_cast<int64_t *>(token_type_ids.data()),
                                          token_type_ids.size(), inputShape.data(), inputShape.size()));
  }

  // 4. ONNX Session inference
  std::vector<const char *> c_input_names;
  c_input_names.reserve(m_inputNames.size());
  for (const auto &name : m_inputNames) c_input_names.push_back(name.c_str());

  std::vector<const char *> c_output_names;
  c_output_names.reserve(m_outputNames.size());
  for (const auto &name : m_outputNames) c_output_names.push_back(name.c_str());

  std::vector<Ort::Value> outputTensors;
  outputTensors = m_ortSession->Run(Ort::RunOptions{nullptr}, c_input_names.data(), inputTensors.data(),
                                    inputTensors.size(), c_output_names.data(), c_output_names.size());
  if (outputTensors.empty()) return STATUS_T::ERROR_OUTPUT_TENSOR_EMPTY;

  Ort::Value &lastHiddenState = outputTensors[0];
  const float *outputData = lastHiddenState.GetTensorMutableData<float>();
  std::vector<int64_t> outputShape = lastHiddenState.GetTensorTypeAndShapeInfo().GetShape();

  // check shape [1, SeqLen, Dim]
  if (outputShape.size() != 3 || outputShape[0] != 1) return STATUS_T::ERROR_OUTPUT_SHAPE_INVALID;

  // 5. Mean Pooling
  const size_t embeddingDimension = outputShape.back();
  embeded_res.resize(embeddingDimension, 0.0f);
  size_t actualTokens = 0;
  for (size_t i = 0; i < sequenceLength; ++i) {
    if (attention_mask[i] == 1) {
      const float *tokenVector = outputData + i * embeddingDimension;
      for (size_t j = 0; j < embeddingDimension; ++j) embeded_res[j] += tokenVector[j];
      actualTokens++;
    }
  }

  if (actualTokens > 0) {
    for (float &val : embeded_res) val /= static_cast<float>(actualTokens);
  }
  return STATUS_T::OK;
}

std::vector<std::string> MiniLMEmbedding::ReadAndChunkFile(const std::string &filePath, size_t maxChunkSize) {
  std::vector<std::string> chunks;

  std::ifstream file(filePath);
  if (!file.is_open()) {
    std::string err("Cannot open file: ");
    err.append(filePath);
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return chunks;
  }

  std::string line;
  std::string currentChunk;

  while (std::getline(file, line)) {
    if (line.empty()) continue;
    if (!currentChunk.empty() && currentChunk.length() + line.length() > maxChunkSize) {
      chunks.push_back(currentChunk);
      currentChunk.clear();
    }

    if (!currentChunk.empty()) currentChunk += " ";
    currentChunk += line;
  }

  if (!currentChunk.empty()) chunks.push_back(currentChunk);
  return chunks;
}

std::vector<std::string> DocumentEmbeddingManager::SplitTextIntoChunks(const std::string &text, size_t maxChunkSize) {
  std::vector<std::string> chunks;

  if (text.empty()) return chunks;

  if (text.length() <= maxChunkSize) {
    chunks.push_back(text);
    return chunks;
  }

  size_t start = 0;
  size_t end = 0;
  while (start < text.length()) {
    end = std::min(start + maxChunkSize, text.length());
    if (end < text.length()) {
      size_t sentence_end = text.find_last_of(".!?。！？", end);
      if (sentence_end != std::string::npos && sentence_end > start) end = sentence_end + 1;
    }

    std::string chunk = text.substr(start, end - start);

    // get rid of the blank at head and tail
    size_t chunk_start = chunk.find_first_not_of(" \t\n\r");
    size_t chunk_end = chunk.find_last_not_of(" \t\n\r");
    if (chunk_start != std::string::npos && chunk_end != std::string::npos)
      chunk = chunk.substr(chunk_start, chunk_end - chunk_start + 1);

    if (!chunk.empty()) chunks.push_back(chunk);

    start = end;
    while (start < text.length() && std::isspace(text[start])) start++;
  }
  return chunks;
}

void DocumentEmbeddingManager::ProcessDocument(const std::string &filePath) {
  auto embeddings = m_embedder.EmbedFile(filePath);
  m_documentEmbeddings.insert(m_documentEmbeddings.end(), embeddings.begin(), embeddings.end());
}

bool DocumentEmbeddingManager::ProcessText(const std::string &text, size_t maxChunkSize) {
  if (text.empty()) return false;
  if (text.length() <= maxChunkSize) {
    auto result = m_embedder.EmbedText(text);
    if (result.confidence > 0) {
      m_documentEmbeddings.push_back(std::move(result));
      return false;
    } else
      return true;  // Low confidence embedding for text chunk
  }

  std::vector<std::string> chunks = SplitTextIntoChunks(text, maxChunkSize);

  for (size_t i = 0; i < chunks.size(); ++i) {
    if (chunks[i].empty()) continue;
    auto result = m_embedder.EmbedText(chunks[i]);
    if (result.confidence > 0) m_documentEmbeddings.push_back(std::move(result));
  }

  return false;
}

std::vector<std::pair<std::string, double>> DocumentEmbeddingManager::SemanticSearch(const std::string &query,
                                                                                     size_t topK) {
  auto queryResult = m_embedder.EmbedText(query);

  auto results = m_embedder.SemanticSearch(queryResult.embedding, m_documentEmbeddings, topK);
  std::vector<std::pair<std::string, double>> ret;

  for (size_t i = 0; i < results.size(); ++i) {
    const auto &[idx, similarity] = results[i];
    ret.push_back(std::make_pair(m_documentEmbeddings[idx].text.substr(0, 100), similarity));
    // std::cout << (i + 1) << ". Similarity: " << std::fixed << std::setprecision(4) << similarity << std::endl;
    // std::cout << "   Text: " << m_documentEmbeddings[idx].text.substr(0, 100) << "..." << std::endl << std::endl;
  }
  return ret;
}

void DocumentEmbeddingManager::SaveEmbeddings(const std::string &outputPath) {
  std::ofstream file(outputPath);
  if (!file.is_open()) return;
  for (const auto &result : m_documentEmbeddings) {
    file << result.text << "\t";
    for (size_t i = 0; i < result.embedding.size(); ++i) {
      file << result.embedding[i];
      if (i < result.embedding.size() - 1) file << ",";
    }
    file << std::endl;
  }
}
}  // namespace SentenceTransform
}  // namespace ML
}  // namespace ShannonBase