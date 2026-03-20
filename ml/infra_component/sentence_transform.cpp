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

#include "storage/rapid_engine/include/rapid_const.h"
namespace ShannonBase {
namespace ML {
namespace SentenceTransform {
static ModelSelection select_model_variant(const std::string &model_dir, const std::string &user_precision,
                                           const std::string &user_opt) {
  ModelSelection result;
  result.device = Device::CPU;
  result.precision = Precision::FP32;
  result.opt_level = "auto";

  auto exists = [&](const std::string &fname) { return fs::exists(model_dir + "/" + fname); };

  if (!user_opt.empty()) {
    std::string fname = "model_" + user_opt + ".onnx";
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
    // Baseline FP32 model — always works on any platform.
    result.filename = "model.onnx";
    result.precision = Precision::FP32;
    result.opt_level = "base";
  } else {
#ifdef SHANNONBASE_ONNX_CUDA_EP
    if (exists("model_O4.onnx")) {
      result.filename = "model_O4.onnx";
      result.precision = Precision::FP32;
      result.opt_level = "O4";
    } else {
      my_error(ER_ML_FAIL, MYF(0), "no valid ONNX model found");
    }
#else
    // model_O4.onnx is intentionally excluded from automatic selection:
    // O4 pre-optimization can bake MemcpyFromHost/MemcpyToHost nodes into
    // the graph (CPU↔GPU transfer nodes inserted by the CUDA-EP graph
    // transformer).  These nodes have no kernel registered in
    // CPUExecutionProvider, causing "Kernel not found" at session creation.
    // Use model_O3.onnx or model.onnx for CPU-only deployments.
    my_error(ER_ML_FAIL, MYF(0),
             "no valid ONNX model found (model_O4.onnx is excluded — it may "
             "contain GPU-specific Memcpy nodes incompatible with CPU-only ORT)");
    return result;
#endif
  }

  return result;
}

MiniLMEmbedding::MiniLMEmbedding(const std::string &modelPath, const std::string &tokenizerPath)
    : m_modelPath(modelPath), m_tokenizerPath(tokenizerPath) {
  if (!m_tokenizerPath.empty()) {
    m_tokenizer = tokenizers::TokenizerUtils::load_from_file(m_tokenizerPath);
    if (!m_tokenizer || !m_tokenizer->is_valid()) {
      my_error(ER_ML_FAIL, MYF(0), ("Failed to load Tokenizer from: " + m_tokenizerPath).c_str());
      return;
    }
  } else {
    my_error(ER_ML_FAIL, MYF(0), "Tokenizer path must be provided for MiniLMEmbedding");
    return;
  }

  auto ms = select_model_variant(m_modelPath, "", "");
  const std::string selected = ms.filename.empty() ? "model.onnx" : ms.filename;
  m_modelPath += selected;
  DBUG_PRINT("MiniLM", ("MiniLMEmbedding: selected model '%s' (precision=%d opt=%s)", selected.c_str(),
                        static_cast<int>(ms.precision), ms.opt_level.c_str()));

  InitializeONNX();

  if (!m_ortSession && selected != "model.onnx") {  // use basic model.
    const std::string base_dir = m_modelPath.substr(0, m_modelPath.size() - selected.size());
    const std::string fallback = base_dir + "model.onnx";
    if (fs::exists(fallback)) {
      DBUG_PRINT("MiniLM", ("MiniLMEmbedding: '%s' failed, falling back to model.onnx", selected.c_str()));
      m_modelPath = fallback;
      InitializeONNX();
    }
  }
}

void MiniLMEmbedding::InitializeONNX() {
  // Initialization ONNX Runtime
  m_run_opts = std::make_unique<Ort::RunOptions>();
  m_ortEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "MiniLM");
  m_sessionOptions = std::make_unique<Ort::SessionOptions>();

  int intra_threads = std::min(8, std::max(4, static_cast<int>(std::thread::hardware_concurrency() / 2)));
  m_sessionOptions->SetIntraOpNumThreads(intra_threads);
  m_sessionOptions->SetInterOpNumThreads(1);
  m_sessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

#ifdef SHANNONBASE_ONNX_CUDA_EP
  OrtStatusPtr cuda_status = OrtSessionOptionsAppendExecutionProvider_CUDA(*m_sessionOptions, 0);
  if (cuda_status) {  // FAILED.
    const char *msg = Ort::GetApi().GetErrorMessage(cuda_status);
    DBUG_PRINT("MiniLM", ("MiniLM: CUDA unavailable (%s), falling back to CPU", msg));
    Ort::GetApi().ReleaseStatus(cuda_status);
  }
#endif

  m_ortSession = std::make_unique<Ort::Session>(*m_ortEnv, m_modelPath.c_str(), *m_sessionOptions);

  Ort::AllocatorWithDefaultOptions allocator;

  size_t numInputNodes = m_ortSession->GetInputCount();
  m_inputNames.clear();
  for (size_t i = 0; i < numInputNodes; ++i)
    m_inputNames.emplace_back(m_ortSession->GetInputNameAllocated(i, allocator).get());

  size_t numOutputNodes = m_ortSession->GetOutputCount();
  m_outputNames.clear();
  for (size_t i = 0; i < numOutputNodes; ++i)
    m_outputNames.emplace_back(m_ortSession->GetOutputNameAllocated(i, allocator).get());
}

int MiniLMEmbedding::TerminateTask() {
  m_run_opts->SetTerminate();
  return ShannonBase::SHANNON_SUCCESS;
}

MiniLMEmbedding::EmbeddingResult MiniLMEmbedding::EmbedText(const std::string &text) {
  if (text.empty()) return {};

  EmbeddingResult result;
  result.text = text;
  result.confidence = 0.0;

  tokenizers::Tokenizer::Encoding encoding(nullptr);
  EmbeddingVector embedding;

  // 1. Tokenization
  auto token_status = Tokenize(text, encoding);
  if (token_status != STATUS_T::OK) {
    my_error(ER_ML_FAIL, MYF(0),
             ("Tokenization failed with error code: " + std::to_string(static_cast<int>(token_status))).c_str());
    return result;
  }

  // 2. Convert encoding results to int64_t vectors.
  const auto &src_ids = encoding.input_ids();
  const auto &src_mask = encoding.attention_mask();
  const auto &src_types = encoding.token_type_ids();

  std::vector<int64_t> input_ids(src_ids.begin(), src_ids.end());
  std::vector<int64_t> attention_mask(src_mask.begin(), src_mask.end());
  std::vector<int64_t> token_type_ids(src_types.begin(), src_types.end());

  // 3. Run Inference
  auto inference_status = RunInference(input_ids, attention_mask, token_type_ids, embedding);
  if (inference_status != STATUS_T::OK) {
    my_error(ER_ML_FAIL, MYF(0),
             ("Inference failed with error code: " + std::to_string(static_cast<int>(inference_status))).c_str());
    return result;
  }

  // 4. L2 Normalization
  if (!embedding.empty()) {
    NormalizeL2(embedding);
    result.embedding = std::move(embedding);
    result.confidence = 1.0;
  } else {
    my_error(ER_ML_FAIL, MYF(0), "Inference succeeded but returned empty embedding vector");
  }

  return result;
}

std::vector<MiniLMEmbedding::EmbeddingResult> MiniLMEmbedding::EmbedFile(const std::string &filePath,
                                                                         size_t maxChunkSize) {
  std::vector<EmbeddingResult> results;
  auto chunks = ReadAndChunkFile(filePath, maxChunkSize);
  for (const auto &chunk : chunks) {
    if (chunk.empty()) continue;
    auto r = EmbedText(chunk);
    if (r.confidence > 0) results.push_back(std::move(r));
  }
  return results;
}

std::vector<MiniLMEmbedding::EmbeddingResult> MiniLMEmbedding::EmbedBatch(const std::vector<std::string> &texts) {
  std::vector<EmbeddingResult> results;
  results.reserve(texts.size());
  for (const auto &t : texts) results.push_back(EmbedText(t));
  return results;
}

double MiniLMEmbedding::CosineSimilarity(const EmbeddingVector &a, const EmbeddingVector &b) {
  if (a.size() != b.size() || a.empty()) return 0.0;
  double dot = 0.0, na = 0.0, nb = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += double(a[i]) * double(b[i]);
    na += double(a[i]) * double(a[i]);
    nb += double(b[i]) * double(b[i]);
  }
  const double denom = std::sqrt(na) * std::sqrt(nb);
  return denom > 1e-9 ? dot / denom : 0.0;
}

std::vector<std::pair<size_t, double>> MiniLMEmbedding::SemanticSearch(const EmbeddingVector &queryEmbedding,
                                                                       const std::vector<EmbeddingResult> &corpus,
                                                                       size_t topK) {
  std::vector<std::pair<size_t, double>> similarities;
  similarities.reserve(corpus.size());
  for (size_t i = 0; i < corpus.size(); ++i)
    similarities.emplace_back(i, CosineSimilarity(queryEmbedding, corpus[i].embedding));

  std::partial_sort(similarities.begin(), similarities.begin() + std::min(topK, similarities.size()),
                    similarities.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
  similarities.resize(std::min(topK, similarities.size()));
  return similarities;
}

STATUS_T MiniLMEmbedding::Tokenize(const std::string &text, tokenizers::Tokenizer::Encoding &enc) const {
  if (!m_tokenizer) return STATUS_T::ERROR_MODEL_NOT_INIT;
  if (text.empty()) return STATUS_T::ERROR_INVALID_INPUT;
  enc = m_tokenizer->encode(text, true);
  if (!enc.is_valid()) return STATUS_T::ERROR_TOKENIZER_FAIL;
  return STATUS_T::OK;
}

STATUS_T MiniLMEmbedding::RunInference(const std::vector<int64_t> &input_ids,
                                       const std::vector<int64_t> &attention_mask,
                                       const std::vector<int64_t> &token_type_ids, EmbeddingVector &embeded_res) {
  embeded_res.clear();
  if (!m_ortSession || m_inputNames.empty()) return STATUS_T::ERROR_MODEL_NOT_INIT;

  const size_t sequenceLength = input_ids.size();
  if (sequenceLength == 0) return STATUS_T::ERROR_INVALID_INPUT;

  std::array<int64_t, 2> inputShape = {1, static_cast<int64_t>(sequenceLength)};

  Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::vector<Ort::Value> inputTensors;

  inputTensors.emplace_back(Ort::Value::CreateTensor<int64_t>(memoryInfo, const_cast<int64_t *>(input_ids.data()),
                                                              input_ids.size(), inputShape.data(), inputShape.size()));

  if (m_inputNames.size() >= 2) {
    if (attention_mask.size() != sequenceLength) return STATUS_T::ERROR_INVALID_INPUT;
    inputTensors.emplace_back(
        Ort::Value::CreateTensor<int64_t>(memoryInfo, const_cast<int64_t *>(attention_mask.data()),
                                          attention_mask.size(), inputShape.data(), inputShape.size()));
  }

  if (m_inputNames.size() >= 3) {
    if (token_type_ids.size() != sequenceLength) return STATUS_T::ERROR_INVALID_INPUT;
    inputTensors.emplace_back(
        Ort::Value::CreateTensor<int64_t>(memoryInfo, const_cast<int64_t *>(token_type_ids.data()),
                                          token_type_ids.size(), inputShape.data(), inputShape.size()));
  }

  std::vector<const char *> c_input_names;
  c_input_names.reserve(m_inputNames.size());
  for (const auto &n : m_inputNames) c_input_names.push_back(n.c_str());

  std::vector<const char *> c_output_names;
  c_output_names.reserve(m_outputNames.size());
  for (const auto &n : m_outputNames) c_output_names.push_back(n.c_str());

  std::vector<Ort::Value> outputTensors;
  try {
    outputTensors = m_ortSession->Run(*m_run_opts.get(), c_input_names.data(), inputTensors.data(), inputTensors.size(),
                                      c_output_names.data(), c_output_names.size());
  } catch (const Ort::Exception &e) {
    DBUG_PRINT("MiniLM", ("RunInference: ORT exception (code=%d): %s", e.GetOrtErrorCode(), e.what()));
    return STATUS_T::ERROR_MODEL_NOT_INIT;
  } catch (const std::exception &e) {
    DBUG_PRINT("MiniLM", ("RunInference: std::exception: %s", e.what()));
    return STATUS_T::ERROR_MODEL_NOT_INIT;
  }
  if (outputTensors.empty()) return STATUS_T::ERROR_OUTPUT_TENSOR_EMPTY;

  Ort::Value &lastHiddenState = outputTensors[0];
  const float *outputData = lastHiddenState.GetTensorMutableData<float>();
  auto outputShape = lastHiddenState.GetTensorTypeAndShapeInfo().GetShape();

  if (outputShape.size() != 3 || outputShape[0] != 1) return STATUS_T::ERROR_OUTPUT_SHAPE_INVALID;

  const size_t embeddingDimension = outputShape.back();
  embeded_res.resize(embeddingDimension, 0.0f);
  size_t actualTokens = 0;
  for (size_t i = 0; i < sequenceLength; ++i) {
    if (attention_mask[i] == 1) {
      const float *tokenVector = outputData + i * embeddingDimension;
      for (size_t j = 0; j < embeddingDimension; ++j) embeded_res[j] += tokenVector[j];
      ++actualTokens;
    }
  }
  if (actualTokens > 0)
    for (float &v : embeded_res) v /= static_cast<float>(actualTokens);

  return STATUS_T::OK;
}

std::vector<std::string> MiniLMEmbedding::ReadAndChunkFile(const std::string &filePath, size_t maxChunkSize) {
  std::vector<std::string> chunks;
  std::ifstream file(filePath);
  if (!file.is_open()) {
    my_error(ER_ML_FAIL, MYF(0), ("Cannot open file: " + filePath).c_str());
    return chunks;
  }
  std::string line, currentChunk;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    if (!currentChunk.empty() && currentChunk.length() + line.length() > maxChunkSize) {
      chunks.push_back(currentChunk);
      currentChunk.clear();
    }
    if (!currentChunk.empty()) currentChunk += ' ';
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
  while (start < text.length()) {
    size_t end = std::min(start + maxChunkSize, text.length());
    if (end < text.length()) {
      size_t sent_end = text.find_last_of(".!?。！？", end);
      if (sent_end != std::string::npos && sent_end > start) end = sent_end + 1;
    }

    std::string chunk = text.substr(start, end - start);
    size_t cs = chunk.find_first_not_of(" \t\n\r");
    size_t ce = chunk.find_last_not_of(" \t\n\r");
    if (cs != std::string::npos && ce != std::string::npos) chunk = chunk.substr(cs, ce - cs + 1);
    if (!chunk.empty()) chunks.push_back(chunk);

    start = end;
    while (start < text.length() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
  }
  return chunks;
}

void DocumentEmbeddingManager::ProcessDocument(const std::string &filePath) {
  auto embeddings = m_embedder->EmbedFile(filePath);
  m_documentEmbeddings.insert(m_documentEmbeddings.end(), embeddings.begin(), embeddings.end());
}

bool DocumentEmbeddingManager::ProcessText(const std::string &text, size_t maxChunkSize) {
  if (text.empty()) return false;
  if (text.length() <= maxChunkSize) {
    auto result = m_embedder->EmbedText(text);
    if (result.confidence > 0) {
      m_documentEmbeddings.push_back(std::move(result));
      return false;
    }
    return true;
  }
  for (const auto &chunk : SplitTextIntoChunks(text, maxChunkSize)) {
    if (chunk.empty()) continue;
    auto result = m_embedder->EmbedText(chunk);
    if (result.confidence > 0) m_documentEmbeddings.push_back(std::move(result));
  }

  return false;
}

std::vector<std::pair<std::string, double>> DocumentEmbeddingManager::SemanticSearch(const std::string &query,
                                                                                     size_t topK) {
  auto queryResult = m_embedder->EmbedText(query);

  auto results = m_embedder->SemanticSearch(queryResult.embedding, m_documentEmbeddings, topK);
  std::vector<std::pair<std::string, double>> ret;
  for (const auto &[idx, similarity] : results)
    ret.emplace_back(m_documentEmbeddings[idx].text.substr(0, 100), similarity);
  return ret;
}

void DocumentEmbeddingManager::SaveEmbeddings(const std::string &outputPath) {
  std::ofstream file(outputPath);
  if (!file.is_open()) return;
  for (const auto &result : m_documentEmbeddings) {
    file << result.text << '\t';
    for (size_t i = 0; i < result.embedding.size(); ++i) {
      if (i) file << ',';
      file << result.embedding[i];
    }
    file << '\n';
  }
}
}  // namespace SentenceTransform
}  // namespace ML
}  // namespace ShannonBase