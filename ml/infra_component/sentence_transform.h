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

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>
#include "ml/infra_component/tokenizer.h"

namespace ShannonBase {
namespace ML {
namespace SentenceTransform {
namespace fs = std::filesystem;
enum class STATUS_T {
  OK = 0,
  ERROR_INVALID_INPUT = 1,
  ERROR_MODEL_NOT_INIT = 2,
  ERROR_ONNX_INFERENCE_FAIL = 3,
  ERROR_OUTPUT_TENSOR_EMPTY = 4,
  ERROR_OUTPUT_SHAPE_INVALID = 5,
  ERROR_TOKENIZER_FAIL = 6,
  ERROR_PLATFORM_UNSUPPORTED = 7
};

class MiniLMEmbedding {
 public:
  using EmbeddingVector = std::vector<float>;

  struct EmbeddingResult {
    std::string text;
    EmbeddingVector embedding;
    double confidence = 0.0;
  };

  MiniLMEmbedding(const std::string &modelPath, const std::string &tokenizerPath = "");
  ~MiniLMEmbedding() = default;

  EmbeddingResult EmbedText(const std::string &text);
  std::vector<EmbeddingResult> EmbedFile(const std::string &filePath, size_t maxChunkSize = 512);
  std::vector<EmbeddingResult> EmbedBatch(const std::vector<std::string> &texts);
  int TerminateTask();

  static double CosineSimilarity(const EmbeddingVector &a, const EmbeddingVector &b);

  std::vector<std::pair<size_t, double>> SemanticSearch(const EmbeddingVector &queryEmbedding,
                                                        const std::vector<EmbeddingResult> &corpus, size_t topK = 5);

  bool is_initialized() const noexcept { return m_initialized; }
  const std::string &last_error() const noexcept { return m_error_string; }

 private:
  void InitializeONNX();
  void ConfigureExecutionProviders(Ort::SessionOptions &opts);

  STATUS_T Tokenize(const std::string &text, tokenizers::Tokenizer::Encoding &enc) const;

  STATUS_T RunInference(const std::vector<int64_t> &input_ids, const std::vector<int64_t> &attention_mask,
                        const std::vector<int64_t> &token_type_ids, EmbeddingVector &result);

  void NormalizeL2(EmbeddingVector &vec) {
    double norm = std::sqrt(std::inner_product(vec.begin(), vec.end(), vec.begin(), 0.0));
    if (norm > 1e-9)
      for (float &v : vec) v /= static_cast<float>(norm);
  }

  std::vector<std::string> ReadAndChunkFile(const std::string &filePath, size_t maxChunkSize);

 private:
  std::string m_modelPath;
  std::string m_tokenizerPath;
  bool m_initialized{false};
  std::string m_error_string;

  std::unique_ptr<tokenizers::Tokenizer> m_tokenizer;
  std::unique_ptr<Ort::RunOptions> m_run_opts;
  std::unique_ptr<Ort::Env> m_ortEnv;
  std::unique_ptr<Ort::Session> m_ortSession;
  std::unique_ptr<Ort::SessionOptions> m_sessionOptions;
  std::vector<std::string> m_inputNames;
  std::vector<std::string> m_outputNames;
};

class DocumentEmbeddingManager {
 public:
  DocumentEmbeddingManager(const std::string &modelPath, const std::string &tokenizer) {
    m_embedder = std::make_unique<MiniLMEmbedding>(modelPath, tokenizer);
  }

  void ProcessDocument(const std::string &filePath);
  bool ProcessText(const std::string &text, size_t maxChunkSize = 512);
  std::vector<std::pair<std::string, double>> SemanticSearch(const std::string &query, size_t topK = 3);
  void SaveEmbeddings(const std::string &outputPath);

  inline std::vector<MiniLMEmbedding::EmbeddingResult> &Results() { return m_documentEmbeddings; }

 private:
  std::vector<std::string> SplitTextIntoChunks(const std::string &text, size_t maxChunkSize);
  std::unique_ptr<MiniLMEmbedding> m_embedder{nullptr};
  std::vector<MiniLMEmbedding::EmbeddingResult> m_documentEmbeddings;
};
}  // namespace SentenceTransform
}  // namespace ML
}  // namespace ShannonBase
#endif  // __SHANNONBASE_RAPID_SENTENCE_TRANSFORM_H__