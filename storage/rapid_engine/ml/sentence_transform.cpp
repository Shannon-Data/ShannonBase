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

#include "storage/rapid_engine/ml/sentence_transform.h"

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
  } else if (exists("model.onnx")) {
    result.filename = "model.onnx";
    result.precision = Precision::FP32;
    result.opt_level = "base";
  } else {
    // throw std::runtime_error("ER_MODEL_VARIANT_NOT_FOUND: no valid ONNX file");
  }

  return result;
}

MiniLMEmbedding::MiniLMEmbedding(const std::string &modelPath, const std::string &tokenizerPath)
    : m_modelPath(modelPath), m_tokenizerPath(tokenizerPath) {
  InitializeONNX();
}

void MiniLMEmbedding::InitializeONNX() {
  // Initialialization ONNX Runtime
  m_ortEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "MiniLM");
  m_sessionOptions = std::make_unique<Ort::SessionOptions>();

  // enable optimization
  m_sessionOptions->SetIntraOpNumThreads(4);
  m_sessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

  // load model
  m_ortSession = std::make_unique<Ort::Session>(*m_ortEnv, m_modelPath.c_str(), *m_sessionOptions);

  Ort::AllocatorWithDefaultOptions allocator;

  size_t numInputNodes = m_ortSession->GetInputCount();
  for (size_t i = 0; i < numInputNodes; ++i) {
    auto inputName = m_ortSession->GetInputNameAllocated(i, allocator);
    m_inputNames.emplace_back(inputName.get());
  }

  size_t numOutputNodes = m_ortSession->GetOutputCount();
  for (size_t i = 0; i < numOutputNodes; ++i) {
    auto outputName = m_ortSession->GetOutputNameAllocated(i, allocator);
    m_outputNames.emplace_back(outputName.get());
  }
}

// single file embedding
MiniLMEmbedding::EmbeddingResult MiniLMEmbedding::EmbedText(const std::string &text) {
  auto tokens = Tokenize(text);
  auto embedding = RunInference(tokens);

  return EmbeddingResult{
      text, std::move(embedding),
      1.0  // truth ratio
  };
}

std::vector<MiniLMEmbedding::EmbeddingResult> MiniLMEmbedding::EmbedFile(const std::string &filePath,
                                                                         size_t maxChunkSize) {
  std::vector<EmbeddingResult> results;

  auto chunks = ReadAndChunkFile(filePath, maxChunkSize);
  results.reserve(chunks.size());

  for (size_t i = 0; i < chunks.size(); ++i) {
    if (!chunks[i].empty()) {
      auto result = EmbedText(chunks[i]);
      if (result.confidence > 0) {
        results.push_back(std::move(result));
      }
    }
  }

  return results;
}

// simple tokenize（in fact using HuggingFace tokenizer）
std::vector<int64_t> MiniLMEmbedding::Tokenize(const std::string &text) {
  std::vector<int64_t> tokens;

  std::string cleanText = PreprocessText(text);
  std::istringstream iss(cleanText);
  std::string word;

  // CLS token (101 for BERT-like models)
  tokens.push_back(101);

  while (iss >> word && tokens.size() < 512) {
    int64_t tokenId = SimpleHash(word) % 30522;  // BERT vocab size
    tokens.push_back(tokenId);
  }

  // SEP token (102)
  tokens.push_back(102);

  // Padding to fixed length
  while (tokens.size() < 128) {
    tokens.push_back(0);  // PAD token
  }

  if (tokens.size() > 128) {
    tokens.resize(128);
    tokens[127] = 102;  // Ensure SEP at end
  }

  return tokens;
}

std::string MiniLMEmbedding::PreprocessText(const std::string &text) {
  std::string result = text;

  std::transform(result.begin(), result.end(), result.begin(), ::tolower);

  std::regex whitespace(R"(\s+)");
  result = std::regex_replace(result, whitespace, " ");

  std::regex punctuation(R"([^\w\s])");
  result = std::regex_replace(result, punctuation, " ");

  return result;
}

std::vector<MiniLMEmbedding::EmbeddingResult> MiniLMEmbedding::EmbedBatch(const std::vector<std::string> &texts) {
  std::vector<MiniLMEmbedding::EmbeddingResult> results;
  results.reserve(texts.size());

  for (const auto &text : texts) {
    results.push_back(EmbedText(text));
  }

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

MiniLMEmbedding::EmbeddingVector MiniLMEmbedding::RunInference(const std::vector<int64_t> &tokens) {
  std::vector<int64_t> inputShape = {1, static_cast<int64_t>(tokens.size())};

  Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  std::vector<Ort::Value> inputTensors;
  inputTensors.emplace_back(Ort::Value::CreateTensor<int64_t>(memoryInfo, const_cast<int64_t *>(tokens.data()),
                                                              tokens.size(), inputShape.data(), inputShape.size()));

  // create attention mask
  std::vector<int64_t> attentionMask(tokens.size(), 1);
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i] == 0) attentionMask[i] = 0;  // PAD tokens
  }

  if (m_inputNames.size() > 1) {
    inputTensors.emplace_back(Ort::Value::CreateTensor<int64_t>(memoryInfo, attentionMask.data(), attentionMask.size(),
                                                                inputShape.data(), inputShape.size()));
  }

  std::vector<const char *> inputNamesPtr;
  for (const auto &name : m_inputNames) {
    inputNamesPtr.push_back(name.c_str());
  }

  std::vector<const char *> outputNamesPtr;
  for (const auto &name : m_outputNames) {
    outputNamesPtr.push_back(name.c_str());
  }

  // run inference
  auto outputTensors = m_ortSession->Run(Ort::RunOptions{nullptr}, inputNamesPtr.data(), inputTensors.data(),
                                         inputTensors.size(), outputNamesPtr.data(), outputNamesPtr.size());

  const float *outputData = outputTensors[0].GetTensorData<float>();
  auto outputShape = outputTensors[0].GetTensorTypeAndShapeInfo().GetShape();

  // the output [batch_size, sequence_length, hidden_size]
  // gets [CLS] token (the first token)
  size_t embeddingSize = outputShape.back();  // hidden_size (384 for MiniLM)

  EmbeddingVector embedding(outputData, outputData + embeddingSize);

  // L2 normalization.
  NormalizeL2(embedding);

  return embedding;
}

std::vector<std::string> MiniLMEmbedding::ReadAndChunkFile(const std::string &filePath, size_t maxChunkSize) {
  std::ifstream file(filePath);
  if (!file.is_open()) {
    throw std::runtime_error("Cannot open file: " + filePath);
  }

  std::vector<std::string> chunks;
  std::string line;
  std::string currentChunk;

  while (std::getline(file, line)) {
    if (line.empty()) continue;

    if (!currentChunk.empty() && currentChunk.length() + line.length() > maxChunkSize) {
      chunks.push_back(currentChunk);
      currentChunk.clear();
    }

    if (!currentChunk.empty()) {
      currentChunk += " ";
    }
    currentChunk += line;
  }

  if (!currentChunk.empty()) {
    chunks.push_back(currentChunk);
  }

  return chunks;
}

void DocumentEmbeddingManager::ProcessDocument(const std::string &filePath) {
  auto embeddings = m_embedder.EmbedFile(filePath);

  m_documentEmbeddings.insert(m_documentEmbeddings.end(), embeddings.begin(), embeddings.end());
}
void DocumentEmbeddingManager::SemanticSearch(const std::string &query, size_t topK) {
  auto queryResult = m_embedder.EmbedText(query);

  auto results = m_embedder.SemanticSearch(queryResult.embedding, m_documentEmbeddings, topK);

  for (size_t i = 0; i < results.size(); ++i) {
    const auto &[idx, similarity] = results[i];
    // std::cout << (i + 1) << ". Similarity: " << std::fixed << std::setprecision(4) << similarity << std::endl;
    // std::cout << "   Text: " << m_documentEmbeddings[idx].text.substr(0, 100) << "..." << std::endl << std::endl;
  }
}

void DocumentEmbeddingManager::SaveEmbeddings(const std::string &outputPath) {
  std::ofstream file(outputPath);
  if (!file.is_open()) {
    return;
  }

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