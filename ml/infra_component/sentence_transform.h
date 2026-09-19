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
  ERROR_PLATFORM_UNSUPPORTED = 7,
  ERROR_INPUT_TOO_LONG = 8
};

class MiniLMEmbedding {
 public:
  using EmbeddingVector = std::vector<float>;

  struct EmbeddingResult {
    std::string text;
    EmbeddingVector embedding;
    double confidence{0.0};
  };

  MiniLMEmbedding(const std::string &modelPath, const std::string &tokenizerPath = "");
  ~MiniLMEmbedding() = default;

  EmbeddingResult EmbedText(const std::string &text, bool truncate = true);
  std::vector<EmbeddingResult> EmbedFile(const std::string &filePath, size_t maxChunkSize = 512);
  std::vector<EmbeddingResult> EmbedBatch(const std::vector<std::string> &texts);
  int TerminateTask();

  static double CosineSimilarity(const EmbeddingVector &a, const EmbeddingVector &b);

  std::vector<std::pair<size_t, double>> SemanticSearch(const EmbeddingVector &queryEmbedding,
                                                        const std::vector<EmbeddingResult> &corpus, size_t topK = 5);

  bool is_initialized() const noexcept { return m_initialized; }
  const std::string &last_error() const noexcept { return m_error_string; }

  /* The part of a loaded model that never changes once it is built, and that
   * is therefore safe to share between sessions: the ORT environment and
   * session (Session::Run is thread-safe) and the tokenizer (encode() is
   * const and reentrant).  Building one is the entire cost of constructing a
   * MiniLMEmbedding -- a full ONNX model load plus 4-8 intra-op threads --
   * and every sys.ML_EMBED_ROW() call used to pay it, because the Item is
   * rebuilt for each non-prepared statement.  Opaque here, defined in the
   * .cpp, so the ORT types stay out of this header. */
  struct Model;

 private:
  /* Process-wide, keyed by model directory + tokenizer path.  Returns the
   * already-loaded model when there is one; loads it under a lock otherwise. */
  static std::shared_ptr<Model> AcquireModel(const std::string &modelDir, const std::string &tokenizerPath);

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
  bool m_initialized{false};
  std::string m_error_string;
  std::string m_last_ort_error;

  /* Shared; see struct Model. */
  std::shared_ptr<Model> m_model;

  /* Per instance, deliberately not shared: TerminateTask() latches the
   * terminate flag on these options, and a shared RunOptions would let one
   * session's cancellation abort every other session's inference. */
  std::unique_ptr<Ort::RunOptions> m_run_opts;
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