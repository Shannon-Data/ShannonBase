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

   The fundmental code for GenAI.

   The ML_RAG routine performs retrieval-augmented generation (RAG) by:
   Taking a natural-language query.
   Retrieving context from relevant documents using semantic search.
   Generating a response that integrates information from the retrieved documents.

   Copyright (c) 2023-, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_ML_RAG_H__
#define __SHANNONBASE_ML_RAG_H__

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "sql-common/json_dom.h"  //Json_wrapper.

#include "ml_algorithm.h"

class Json_wrapper;
namespace ShannonBase {
namespace ML {

class ML_RAG : public ML_algorithm {
 public:
  struct Citation {
    std::string segment;        // Retrieved text segment
    double distance;            // Distance from query
    std::string document_name;  // Source document name
    std::string vector_store;   // Source vector store table
    int segment_number;         // Segment number in document
    Json_object metadata;       // Additional metadata
  };

  struct RAGResult {
    std::string generated_text;
    std::vector<Citation> citations;
    bool success = false;
    std::string error_message;
  };

  // ctor
  ML_RAG() = default;
  virtual ~ML_RAG() override = default;

  // process RAG.
  /**
   * options: JSON_OBJECT(keyvalue[, keyvalue]...)
   * keyvalue:
   * {
   *   'vector_store', JSON_ARRAY('VectorStoreTableName'[, 'VectorStoreTableName']...)
   *   |'schema', JSON_ARRAY('SchemaName'[, 'SchemaName']...)
   *   |'n_citations', NumberOfCitations
   *   |'distance_metric', {'COSINE'|'DOT'|'EUCLIDEAN'}
   *   |'document_name', JSON_ARRAY('DocumentName'[, 'DocumentName']...)
   *   |'skip_generate', {true|false}
   *   |'model_options', modeloptions
   *   |'exclude_vector_store', JSON_ARRAY('ExcludeVectorStoreTableName'[, 'ExcludeVectorStoreTableName']...)
   *   |'exclude_document_name', JSON_ARRAY('ExcludeDocumentName'[, 'ExcludeDocumentName']...)
   *   |'retrieval_options', retrievaloptions
   *   |'vector_store_columns', vscoptions
   *   |'embed_model_id', 'EmbeddingModelID'
   *   |'query_embedding', 'QueryEmbedding'
   * }
   */
  virtual std::string ProcessRAG(const std::string &query_text, const Json_wrapper &options) = 0;

  /*
   * Convert to RAG result to JSON format string.
   */
  virtual std::string ToJSON(const RAGResult &result) {
    std::ostringstream json;

    json << "{";
    json << "\"text\": \"" << EscapeJSON(result.generated_text) << "\",";
    json << "\"citations\": [";

    for (size_t i = 0; i < result.citations.size(); ++i) {
      if (i > 0) json << ",";
      const Citation &citation = result.citations[i];

      json << "{";
      json << "\"segment\": \"" << EscapeJSON(citation.segment) << "\",";
      json << "\"distance\": " << citation.distance << ",";
      json << "\"document_name\": \"" << EscapeJSON(citation.document_name) << "\",";
      json << "\"vector_store\": \"" << EscapeJSON(citation.vector_store) << "\",";
      json << "\"segment_number\": " << citation.segment_number;
      json << "}";
    }

    json << "]";
    json << "}";

    return json.str();
  }

 private:
  // Helper function to escape JSON strings
  std::string EscapeJSON(const std::string &input) {
    std::string output;
    output.reserve(input.length());

    for (char c : input) {
      switch (c) {
        case '"':
          output += "\\\"";
          break;
        case '\\':
          output += "\\\\";
          break;
        case '\b':
          output += "\\b";
          break;
        case '\f':
          output += "\\f";
          break;
        case '\n':
          output += "\\n";
          break;
        case '\r':
          output += "\\r";
          break;
        case '\t':
          output += "\\t";
          break;
        default:
          if (c < 0x20) {
            output += "\\u";
            output += "0123456789abcdef"[(c >> 12) & 0xf];
            output += "0123456789abcdef"[(c >> 8) & 0xf];
            output += "0123456789abcdef"[(c >> 4) & 0xf];
            output += "0123456789abcdef"[c & 0xf];
          } else {
            output += c;
          }
          break;
      }
    }
    return output;
  }
};

class ML_RAG_row : public ML_RAG {
 public:
  ML_RAG_row() = default;
  virtual ~ML_RAG_row() override = default;

  virtual ML_TASK_TYPE_T type() override { return ML_TASK_TYPE_T::RAG; }

  virtual std::string ProcessRAG(const std::string &query_text, const Json_wrapper &options) override;

 private:
  virtual int train() override { return false; }
  virtual int load(std::string &) override { return false; }
  virtual int load_from_file(std::string &, std::string &) override { return false; }
  virtual int unload(std::string &) override { return false; }
  virtual int import(Json_wrapper &, Json_wrapper &, std::string &) override { return false; }
  virtual double score(std::string &, std::string &, std::string &, std::string &, Json_wrapper &) override {
    return 0.0f;
  }

  virtual int explain(std::string &, std::string &, std::string &, Json_wrapper &) override { return false; }
  virtual int explain_row() override { return false; }
  virtual int explain_table() override { return false; }
  virtual int predict_row(Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) override { return false; }
  virtual int predict_table(std::string &, std::string &, std::string &, Json_wrapper &) override { return false; }
};

class ML_RAG_table : public ML_RAG {
 public:
  ML_RAG_table() = default;
  virtual ~ML_RAG_table() override = default;

  virtual ML_TASK_TYPE_T type() override { return ML_TASK_TYPE_T::RAG; }

  virtual std::string ProcessRAG(const std::string &query_text, const Json_wrapper &options) override;

 private:
  virtual int train() override { return false; }
  virtual int load(std::string &) override { return false; }
  virtual int load_from_file(std::string &, std::string &) override { return false; }
  virtual int unload(std::string &) override { return false; }
  virtual int import(Json_wrapper &, Json_wrapper &, std::string &) override { return false; }
  virtual double score(std::string &, std::string &, std::string &, std::string &, Json_wrapper &) override {
    return 0.0f;
  }

  virtual int explain(std::string &, std::string &, std::string &, Json_wrapper &) override { return false; }
  virtual int explain_row() override { return false; }
  virtual int explain_table() override { return false; }
  virtual int predict_row(Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) override { return false; }
  virtual int predict_table(std::string &, std::string &, std::string &, Json_wrapper &) override { return false; }
};

}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ML_RAG_H__