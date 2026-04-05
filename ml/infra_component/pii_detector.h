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
   Copyright (c) 2023 -, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_RAPID_PII_DETECTOR_H__
#define __SHANNONBASE_RAPID_PII_DETECTOR_H__
#include <filesystem>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

#include <onnxruntime_cxx_api.h>
#include "ml/infra_component/llm_model_detector.h"
#include "ml/infra_component/tokenizer.h"
namespace ShannonBase {
namespace ML {
class PIIDetector {
 public:
  /**
   * Entity types supported by the PII detector. NER-sourced: PERSON, ORGANIZATION, LOCATION, MISC
   * Regex-sourced: EMAIL, PHONE_NUMBER, CREDIT_CARD, SSN, IP_ADDRESS, DATE_OF_BIRTH, STREET_ADDRESS
   */
  enum class EntityType {
    PERSON,
    ORGANIZATION,
    LOCATION,
    MISC,
    EMAIL,
    PHONE_NUMBER,
    CREDIT_CARD,
    SSN,
    IP_ADDRESS,
    DATE_OF_BIRTH,  // bare date used as a birthday (MM/DD/YYYY or YYYY-MM-DD)
    DATETIME,       // full timestamp: YYYY-MM-DD HH:MM:SS  (log lines, receipts)
    STREET_ADDRESS,
    UNKNOWN
  };

  struct PIIEntity {
    EntityType type;
    std::string text;  // The matched substring from the original input.
    size_t start;      // Character start offset (inclusive).
    size_t end;        // Character end offset (exclusive).
    float confidence;  // Confidence score [0.0, 1.0].
  };

  struct DetectionResult {
    std::vector<PIIEntity> entities;
  };

  enum class MaskingPolicy {
    REPLACE_WITH_TYPE,  // Replace with bracketed type label, e.g. [PERSON].
    REPLACE_WITH_MASK,  // Replace with fixed mask string "***".
    HASH,               // Replace with truncated hex hash of the original text.
    PRESERVE_FORMAT,    // Mask while preserving structural format (e.g. email
                        // domain, phone digit count, card last-4 digits).
    CUSTOM              // Per-entity-type replacement rules supplied by caller.
  };

  struct MaskingOptions {
    MaskingPolicy policy{MaskingPolicy::REPLACE_WITH_TYPE};
    std::map<std::string, std::string> custom_rules;
  };

  struct Options {
    std::string model_id;
    std::string output_format = "json";

    std::vector<std::string> label_names = {"O",     "B-PER", "I-PER",  "B-ORG", "I-ORG",
                                            "B-LOC", "I-LOC", "B-MISC", "I-MISC"};
    float min_confidence = 0.50f;
    std::unordered_set<std::string> ner_stopwords = {
        // ── English: function words ──
        "a", "an", "the", "and", "or", "but", "at", "by", "for", "in", "of", "on", "to", "up", "as", "is", "it", "its",
        "be", "was", "are", "were", "do", "did", "has", "have", "had", "not", "no", "so", "if", "my", "we", "he", "she",
        "they", "you", "i", "me", "him", "her", "us", "them", "from", "with", "into", "than", "that", "this", "these",
        "those", "then", "when", "where", "who", "what", "how", "why", "which", "can", "may", "will", "shall", "just",
        "via", "re", "cc", "vs", "etc", "eg", "ie",
        // ── English: verbs/nouns misclassified in structured/log text ──
        "send", "reach", "get", "set", "go", "about", "info", "log", "note", "notes", "user", "agent", "trans",
        "action", "token", "data", "null", "none", "true", "false", "error", "warn", "debug", "trace", "event", "query",
        "request", "response", "status", "type", "value", "name", "size", "card", "ending", "ending in", "shipping",
        "merchant", "payment", "transaction", "checkout", "please", "leave", "door", "arrived", "when",
        // ── Chinese: pronouns & possessives ──
        "我", "你", "他", "她", "它", "我们", "你们", "他们", "她们", "我的", "你的", "他的", "她的", "我们的",
        "你们的", "的", "地", "得", "了", "着", "过", "吗", "呢", "吧", "啊", "和", "与", "或", "或者", "但", "但是",
        "而", "然后", "如果", "所以", "也", "都", "很", "就", "还", "只", "已经", "正在", "是", "不", "没", "在", "有",
        "这", "那", "这个", "那个", "这些", "那些", "什么", "哪", "谁", "怎么", "多少", "可以", "可能", "应该", "需要",
        "能", "会", "要", "想"};
  };

  PIIDetector(const std::string &model_path, const std::string &tokenizer_path, const Options &options);
  virtual ~PIIDetector() = default;

  PIIDetector(const PIIDetector &) = delete;
  PIIDetector &operator=(const PIIDetector &) = delete;

  bool Initialized() const noexcept { return m_initialized; }

  /**
   * Detect PII entities in @p text.
   * @return JSON string with the detection report, or empty string on error.
   */
  std::string Detect(const std::string &text);

  /**
   * Detect and mask PII entities in @p text according to @p mask_opts.
   * @return Masked text string, or empty string on error.
   */
  std::string Mask(const std::string &text, const MaskingOptions &mask_opts);

 private:
  bool InitializeONNX();
  bool InitializeTokenizer();

  DetectionResult RunNER(const std::string &text);

  void RunRegex(const std::string &text, DetectionResult &result);

  // Remove fully-overlapping duplicates (NER vs regex conflicts).
  // When two spans overlap, the one with higher confidence wins.
  void DeduplicateOverlapping(DetectionResult &result);

  void SortByPosition(DetectionResult &result);

  using CharSpan = std::pair<size_t, size_t>;
  std::vector<CharSpan> ComputeTokenCharOffsets(const std::string &text, const std::vector<std::string> &tokens) const;

  void AggregateBIOSpans(const std::string &text, const std::vector<std::string> &tokens,
                         const std::vector<CharSpan> &charOffsets, const std::vector<int> &labelIds,
                         const std::vector<float> &confidences, DetectionResult &result);

  std::string BuildDetectionJSON(const std::string &text, const DetectionResult &result) const;
  std::string ApplyMasking(const std::string &text, const DetectionResult &result, const MaskingOptions &opts) const;

  std::string ReplaceWithType(EntityType type) const;
  std::string HashText(const std::string &text) const;
  std::string PreserveFormatMask(const std::string &text, EntityType type) const;
  std::string ApplyCustomRule(const std::string &text, EntityType type, const MaskingOptions &opts) const;

  static std::string EntityTypeToString(EntityType type);
  static EntityType LabelToEntityType(const std::string &bio_label);
  static std::string EscapeJSON(const std::string &s);

  EntityType LabelIdToEntityType(int label_id) const;

  bool m_initialized = false;

  std::string m_modelPath;      // Path to ONNX model file.
  std::string m_tokenizerPath;  // Path to tokenizer.json.
  Options m_options;

  // ONNX Runtime objects.
  std::unique_ptr<Ort::Env> m_env;
  std::unique_ptr<Ort::SessionOptions> m_sessionOptions;
  std::unique_ptr<Ort::Session> m_session;

  // BERT NER models require input_ids, attention_mask, optionally
  // token_type_ids.  We detect which inputs are required at init time.
  bool m_needsTokenTypeIds = false;

  size_t m_numLabels = 0;

  int64_t m_maxSeqLen = 512;

  std::shared_ptr<tokenizers::Tokenizer> m_tokenizer;

  struct RegexPattern {
    std::regex re;
    EntityType type;
    float confidence;
  };
  std::vector<RegexPattern> m_regexPatterns;

  void BuildRegexPatterns();
};
}  // namespace ML
}  // namespace ShannonBase
#endif  // __SHANNONBASE_RAPID_PII_DETECTOR_H__