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
#include "pii_detector.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace ShannonBase {
namespace ML {
PIIDetector::PIIDetector(const std::string &model_path, const std::string &tokenizer_path, const Options &options)
    : m_modelPath(model_path),
      m_tokenizerPath(tokenizer_path),
      m_options(options),
      m_numLabels(options.label_names.size()) {
  auto ms = select_model_variant(m_modelPath);
  m_modelPath = (std::filesystem::path(m_modelPath) / ms.filename).string();
  BuildRegexPatterns();
  BuildGazetteer();
  m_initialized = InitializeTokenizer() && InitializeONNX();
}

bool PIIDetector::InitializeTokenizer() {
  std::string tokenizer_file = m_tokenizerPath + "tokenizer.json";
  if (!std::filesystem::exists(tokenizer_file)) return false;

  try {
    m_tokenizer = std::make_shared<tokenizers::Tokenizer>(tokenizer_file);
    return m_tokenizer && m_tokenizer->is_valid();
  } catch (...) {
    return false;
  }
}

bool PIIDetector::InitializeONNX() {
  try {
    m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "PIIDetector");

    m_sessionOptions = std::make_unique<Ort::SessionOptions>();
    m_sessionOptions->SetIntraOpNumThreads(static_cast<int>(std::max(1u, std::thread::hardware_concurrency() / 2)));
    m_sessionOptions->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

    m_session = std::make_unique<Ort::Session>(*m_env, m_modelPath.c_str(), *m_sessionOptions);

    // Inspect input names to determine whether token_type_ids are required.
    Ort::AllocatorWithDefaultOptions alloc;
    size_t input_count = m_session->GetInputCount();
    for (size_t i = 0; i < input_count; ++i) {
      auto name = m_session->GetInputNameAllocated(i, alloc);
      if (std::string(name.get()) == "token_type_ids") {
        m_needsTokenTypeIds = true;
      }
    }

    // Detect max sequence length from model input shape (dim 1).
    auto input_info = m_session->GetInputTypeInfo(0);
    auto shape = input_info.GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() >= 2 && shape[1] > 0) {
      m_maxSeqLen = shape[1];
    }

    return true;
  } catch (const Ort::Exception &ex) {
    (void)ex;
    return false;
  }
}

void PIIDetector::BuildRegexPatterns() {
  // PHONE: Chinese mobile (11 digits, 1[3-9]xx xxxx xxxx)
  m_regexPatterns.push_back(
      {std::regex(R"(\b1[3-9]\d[\s\-]?\d{4}[\s\-]?\d{4}\b)", std::regex::ECMAScript), EntityType::PHONE_NUMBER, 0.95f});

  // PHONE: Chinese landline (area code 3-4 digits + local 7-8 digits)
  m_regexPatterns.push_back(
      {std::regex(R"(\b0\d{2,3}[\s\-]?\d{7,8}\b)", std::regex::ECMAScript), EntityType::PHONE_NUMBER, 0.88f});

  // PHONE: North American 10-digit (NXX-NXX-XXXX)
  m_regexPatterns.push_back(
      {std::regex(R"(\b(\+?1[\s\-.]?)?\(?[2-9]\d{2}\)?[\s\-.]?[2-9]\d{2}[\s\-.]?\d{4}\b)", std::regex::ECMAScript),
       EntityType::PHONE_NUMBER, 0.92f});

  // CREDIT CARD — 13–19 digit groups (Visa, MC, Amex, Discover)
  m_regexPatterns.push_back(
      {std::regex(R"(\b[2-9]\d{2}[\-\s]\d{4}\b)", std::regex::ECMAScript), EntityType::PHONE_NUMBER, 0.82f});

  //  EMAIL
  m_regexPatterns.push_back({std::regex(R"(\b[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}\b)",
                                        std::regex::ECMAScript | std::regex::icase),
                             EntityType::EMAIL, 0.99f});

  // CREDIT CARD: Visa / MC / Amex / Discover
  m_regexPatterns.push_back(
      {std::regex(R"(\b(?:4\d{3}|5[1-5]\d{2}|3[47]\d{2}|6(?:011|5\d{2}))[\s\-]?\d{4}[\s\-]?\d{4}[\s\-]?\d{4}\b)",
                  std::regex::ECMAScript),
       EntityType::CREDIT_CARD, 0.95f});

  // SSN - US Social Security Number
  m_regexPatterns.push_back({std::regex(R"(\b\d{3}-\d{2}-\d{4}\b)", std::regex::ECMAScript), EntityType::SSN, 0.96f});

  // IPv4
  m_regexPatterns.push_back(
      {std::regex(R"(\b(?:(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\b)",
                  std::regex::ECMAScript),
       EntityType::IP_ADDRESS, 0.97f});

  // US STREET ADDRESS
  m_regexPatterns.push_back({std::regex(R"(\b\d{1,5}\s+[A-Za-z0-9]+(?:\s+[A-Za-z0-9]+){0,3}\s+)"
                                        R"((?:St|Ave|Blvd|Dr|Rd|Ln|Ct|Pl|Way|Pkwy|Hwy|Ste|Suite|Apt)\.?)"
                                        R"((?:[,\s]+[A-Za-z\s]{2,20})?(?:[,\s]+[A-Z]{2})?(?:\s+\d{5}(?:-\d{4})?)?\b)",
                                        std::regex::ECMAScript | std::regex::icase),
                             EntityType::STREET_ADDRESS, 0.85f});
  m_regexPatterns.push_back(
      {std::regex(R"(\b(?:19|20)\d{2}-(?:0[1-9]|1[0-2])-(?:0[1-9]|[12]\d|3[01])[T ]\d{2}:\d{2}:\d{2}\b)",
                  std::regex::ECMAScript),
       EntityType::DATETIME, 0.97f});

  // DATE OF BIRTH / general date: MM/DD/YYYY or MM-DD-YYYY
  m_regexPatterns.push_back(
      {std::regex(R"(\b(?:0?[1-9]|1[0-2])[\/\-\.](?:0?[1-9]|[12]\d|3[01])[\/\-\.](?:19|20)\d{2}\b)",
                  std::regex::ECMAScript),
       EntityType::DATE_OF_BIRTH, 0.80f});

  // ISO date YYYY-MM-DD (also matches log timestamps; label as DOB
  // for masking purposes — both are temporal PII in many jurisdictions).
  m_regexPatterns.push_back(
      {std::regex(R"(\b(?:19|20)\d{2}-(?:0[1-9]|1[0-2])-(?:0[1-9]|[12]\d|3[01])\b)", std::regex::ECMAScript),
       EntityType::DATE_OF_BIRTH, 0.82f});
}

std::string PIIDetector::Detect(const std::string &text) {
  if (!m_initialized || text.empty()) return "{}";

  DetectionResult result = RunNER(text);
  RunRegex(text, result);
  RunGazetteer(text, result);
  DeduplicateOverlapping(result);
  SortByPosition(result);
  MergeAdjacentNERSpans(text, result);
  NormalizeNERTypes(result);

  return BuildDetectionJSON(text, result);
}

std::string PIIDetector::Mask(const std::string &text, const MaskingOptions &mask_opts) {
  if (!m_initialized || text.empty()) return text;

  DetectionResult result = RunNER(text);
  RunRegex(text, result);
  RunGazetteer(text, result);
  DeduplicateOverlapping(result);
  SortByPosition(result);
  MergeAdjacentNERSpans(text, result);
  NormalizeNERTypes(result);

  return ApplyMasking(text, result, mask_opts);
}

PIIDetector::DetectionResult PIIDetector::RunNER(const std::string &text) {
  PIIDetector::DetectionResult result;
  if (!m_session || !m_tokenizer) return result;

  auto encoding = m_tokenizer->encode(text, /*add_special_tokens=*/true);
  if (!encoding.is_valid()) return result;

  auto input_ids_u32 = encoding.input_ids();
  auto attn_mask_u32 = encoding.attention_mask();
  auto token_strs = encoding.tokens();

  size_t seq_len = input_ids_u32.size();
  if (seq_len == 0) return result;

  // Truncate to model's maximum sequence length.
  if (static_cast<int64_t>(seq_len) > m_maxSeqLen) {
    seq_len = static_cast<size_t>(m_maxSeqLen);
    input_ids_u32.resize(seq_len);
    attn_mask_u32.resize(seq_len);
    token_strs.resize(seq_len);
  }

  // Convert uint32 → int64 (ONNX expects int64 for token ids).
  std::vector<int64_t> input_ids(seq_len);
  std::vector<int64_t> attn_mask(seq_len);
  std::vector<int64_t> token_type_ids(seq_len, 0);
  for (size_t i = 0; i < seq_len; ++i) {
    input_ids[i] = static_cast<int64_t>(input_ids_u32[i]);
    attn_mask[i] = static_cast<int64_t>(attn_mask_u32[i]);
  }

  std::array<int64_t, 2> shape = {1, static_cast<int64_t>(seq_len)};

  auto mem_info = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

  std::vector<Ort::Value> input_tensors;
  std::vector<const char *> input_names;

  auto ids_tensor =
      Ort::Value::CreateTensor<int64_t>(mem_info, input_ids.data(), input_ids.size(), shape.data(), shape.size());
  input_tensors.push_back(std::move(ids_tensor));
  input_names.push_back("input_ids");

  auto mask_tensor =
      Ort::Value::CreateTensor<int64_t>(mem_info, attn_mask.data(), attn_mask.size(), shape.data(), shape.size());
  input_tensors.push_back(std::move(mask_tensor));
  input_names.push_back("attention_mask");

  if (m_needsTokenTypeIds) {
    auto tti_tensor = Ort::Value::CreateTensor<int64_t>(mem_info, token_type_ids.data(), token_type_ids.size(),
                                                        shape.data(), shape.size());
    input_tensors.push_back(std::move(tti_tensor));
    input_names.push_back("token_type_ids");
  }

  const char *output_names[] = {"logits"};

  std::vector<Ort::Value> output_tensors;
  try {
    output_tensors = m_session->Run(Ort::RunOptions{nullptr}, input_names.data(), input_tensors.data(),
                                    input_tensors.size(), output_names, 1);
  } catch (const Ort::Exception &) {
    return result;
  }

  if (output_tensors.empty()) return result;

  const float *logits = output_tensors[0].GetTensorData<float>();
  size_t num_labels = m_numLabels > 0 ? m_numLabels : 9;  // BIO PERSON/ORG/LOC/MISC

  // Softmax per token, then argmax.
  std::vector<int> label_ids(seq_len);
  std::vector<float> confidences(seq_len, 0.0f);

  for (size_t t = 0; t < seq_len; ++t) {
    const float *row = logits + t * num_labels;

    // Softmax
    float max_logit = *std::max_element(row, row + num_labels);
    float sum_exp = 0.0f;
    std::vector<float> probs(num_labels);
    for (size_t l = 0; l < num_labels; ++l) {
      probs[l] = std::exp(row[l] - max_logit);
      sum_exp += probs[l];
    }
    for (auto &p : probs) p /= sum_exp;

    // Argmax
    int best = static_cast<int>(std::max_element(probs.begin(), probs.end()) - probs.begin());
    label_ids[t] = best;
    confidences[t] = probs[best];
  }

  auto charOffsets = ComputeTokenCharOffsets(text, token_strs);
  AggregateBIOSpans(text, token_strs, charOffsets, label_ids, confidences, result);
  return result;
}

std::vector<PIIDetector::CharSpan> PIIDetector::ComputeTokenCharOffsets(const std::string &text,
                                                                        const std::vector<std::string> &tokens) const {
  static const size_t NPOS = std::string::npos;
  std::vector<CharSpan> offsets(tokens.size(), {NPOS, NPOS});

  size_t char_pos = 0;  // Current search start in the original text.

  for (size_t i = 0; i < tokens.size(); ++i) {
    const std::string &tok = tokens[i];

    // Skip BERT special tokens.
    if (tok == "[CLS]" || tok == "[SEP]" || tok == "[PAD]" || tok == "<s>" || tok == "</s>" || tok == "<pad>") {
      continue;
    }

    // BERT WordPiece continuation pieces start with "##".
    // GPT-2/RoBERTa BPE continuation pieces start with "Ġ" (U+0120).
    // SentencePiece non-first pieces start with "▁" (U+2581) on FIRST piece.
    std::string raw_tok = tok;
    bool is_continuation = false;

    if (raw_tok.size() >= 2 && raw_tok[0] == '#' && raw_tok[1] == '#') {
      raw_tok = raw_tok.substr(2);
      is_continuation = true;
    } else if (!raw_tok.empty() && static_cast<unsigned char>(raw_tok[0]) == 0xC4 &&
               static_cast<unsigned char>(raw_tok[1]) == 0xA0) {
      // Ġ is UTF-8 0xC4 0xA0 — indicates word boundary (space before word).
      raw_tok = raw_tok.substr(2);
    } else if (raw_tok.size() >= 3 && static_cast<unsigned char>(raw_tok[0]) == 0xE2 &&
               static_cast<unsigned char>(raw_tok[1]) == 0x96 && static_cast<unsigned char>(raw_tok[2]) == 0x81) {
      // ▁ is UTF-8 E2 96 81 (SentencePiece word-start prefix).
      raw_tok = raw_tok.substr(3);
    }

    if (raw_tok.empty()) continue;

    // For continuation pieces, search from the current position without
    // skipping whitespace (the piece immediately follows the previous token).
    // For first-of-word pieces, skip over whitespace first.
    if (!is_continuation) {
      while (char_pos < text.size() &&
             (text[char_pos] == ' ' || text[char_pos] == '\t' || text[char_pos] == '\n' || text[char_pos] == '\r')) {
        ++char_pos;
      }
    }

    // Case-insensitive substring search (NER models often lowercase).
    size_t found = NPOS;
    size_t search_from = char_pos;
    while (search_from + raw_tok.size() <= text.size()) {
      bool match = true;
      for (size_t k = 0; k < raw_tok.size() && match; ++k) {
        if (std::tolower(static_cast<unsigned char>(text[search_from + k])) !=
            std::tolower(static_cast<unsigned char>(raw_tok[k]))) {
          match = false;
        }
      }
      if (match) {
        found = search_from;
        break;
      }
      ++search_from;
    }

    if (found != NPOS) {
      offsets[i] = {found, found + raw_tok.size()};
      char_pos = found + raw_tok.size();
    }
  }

  return offsets;
}

void PIIDetector::AggregateBIOSpans(const std::string &text, const std::vector<std::string> &tokens,
                                    const std::vector<CharSpan> &charOffsets, const std::vector<int> &labelIds,
                                    const std::vector<float> &confidences, DetectionResult &result) {
  static const size_t NPOS = std::string::npos;
  EntityType cur_type = EntityType::UNKNOWN;
  size_t span_start = NPOS;
  size_t span_end = NPOS;
  float span_conf = 0.0f;
  size_t span_tokens = 0;

  auto flush = [&]() {
    if (cur_type != EntityType::UNKNOWN && span_start != NPOS && span_end != NPOS && span_end > span_start &&
        span_tokens > 0) {
      float avg_conf = span_conf / static_cast<float>(span_tokens);
      // Per-type threshold takes precedence over the global min_confidence.
      std::string type_str = EntityTypeToString(cur_type);
      auto thr_it = m_options.type_min_confidence.find(type_str);
      float threshold = (thr_it != m_options.type_min_confidence.end()) ? thr_it->second : m_options.min_confidence;
      if (avg_conf >= threshold) {
        bool ends_mid_word = (span_end < text.size() && std::isalnum(static_cast<unsigned char>(text[span_end])));
        bool starts_mid_word = (span_start > 0 && std::isalnum(static_cast<unsigned char>(text[span_start - 1])));
        if (ends_mid_word || starts_mid_word) {
          // Reset state and skip this entity.
          cur_type = EntityType::UNKNOWN;
          span_start = NPOS;
          span_end = NPOS;
          span_conf = 0.0f;
          span_tokens = 0;
          return;
        }

        std::string span_text = text.substr(span_start, span_end - span_start);

        // Stopword guard
        std::string lower_span = span_text;
        std::transform(lower_span.begin(), lower_span.end(), lower_span.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        auto trim_start = lower_span.find_first_not_of(" \t");
        auto trim_end = lower_span.find_last_not_of(" \t");
        std::string trimmed = (trim_start == NPOS) ? "" : lower_span.substr(trim_start, trim_end - trim_start + 1);

        bool is_stopword = !trimmed.empty() && m_options.ner_stopwords.count(trimmed) > 0;
        if (!is_stopword) {
          PIIEntity ent;
          ent.type = cur_type;
          ent.start = span_start;
          ent.end = span_end;
          ent.confidence = avg_conf;
          ent.text = std::move(span_text);
          result.entities.push_back(std::move(ent));
        }
      }
    }
    cur_type = EntityType::UNKNOWN;
    span_start = NPOS;
    span_end = NPOS;
    span_conf = 0.0f;
    span_tokens = 0;
  };

  for (size_t i = 0; i < tokens.size(); ++i) {
    int label_id = labelIds[i];
    const std::string &label = (label_id >= 0 && static_cast<size_t>(label_id) < m_options.label_names.size())
                                   ? m_options.label_names[label_id]
                                   : "O";

    char bio_prefix = 'O';
    std::string entity_tag;
    if (label.size() >= 3 && (label[0] == 'B' || label[0] == 'I') && label[1] == '-') {
      bio_prefix = label[0];
      entity_tag = label.substr(2);
    }

    EntityType etype = LabelToEntityType(entity_tag);
    const CharSpan &cs = charOffsets[i];
    const std::string &tok = tokens[i];

    bool is_wordpiece_continuation = (tok.size() >= 2 && tok[0] == '#' && tok[1] == '#');
    if (bio_prefix == 'B' && is_wordpiece_continuation && etype == cur_type && cur_type != EntityType::UNKNOWN) {
      bio_prefix = 'I';  // promote: merge into the ongoing span
    }

    if (bio_prefix == 'B') {
      flush();
      if (cs.first != NPOS) {
        cur_type = etype;
        span_start = cs.first;
        span_end = cs.second != NPOS ? cs.second : cs.first;
        span_conf = confidences[i];
        span_tokens = 1;
      }
    } else if (bio_prefix == 'I' && etype == cur_type && cur_type != EntityType::UNKNOWN) {
      if (cs.first != NPOS) {
        if (cs.second != NPOS && cs.second > span_end) {
          span_end = cs.second;
        }
        span_conf += confidences[i];
        ++span_tokens;
      }
    } else {
      flush();
    }
  }
  flush();
}

void PIIDetector::RunRegex(const std::string &text, DetectionResult &result) {
  for (const auto &pat : m_regexPatterns) {
    auto begin = std::sregex_iterator(text.begin(), text.end(), pat.re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
      const std::smatch &m = *it;
      PIIEntity ent;
      ent.type = pat.type;
      ent.text = m.str();
      ent.start = static_cast<size_t>(m.position());
      ent.end = ent.start + ent.text.size();
      ent.confidence = pat.confidence;
      result.entities.push_back(std::move(ent));
    }
  }
}

void PIIDetector::DeduplicateOverlapping(DetectionResult &result) {
  // Structured types come from high-precision regex patterns and should always
  // win over NER-sourced spans when the two overlap, regardless of confidence.
  // Within the same source category, prefer longer spans then higher confidence.
  auto is_structured = [](EntityType t) {
    switch (t) {
      case EntityType::EMAIL:
      case EntityType::PHONE_NUMBER:
      case EntityType::CREDIT_CARD:
      case EntityType::SSN:
      case EntityType::IP_ADDRESS:
      case EntityType::DATE_OF_BIRTH:
      case EntityType::DATETIME:
      case EntityType::STREET_ADDRESS:
      case EntityType::LOCATION:
        return true;
      default:
        return false;
    }
  };

  // Sort so the "preferred" entity of any overlapping pair comes first:
  //   1. Earlier start position
  //   2. Structured type before NER type (0 < 1)
  //   3. Longer span (higher end) before shorter
  //   4. Higher confidence before lower
  std::sort(result.entities.begin(), result.entities.end(), [&](const PIIEntity &a, const PIIEntity &b) {
    if (a.start != b.start) return a.start < b.start;
    int a_prio = is_structured(a.type) ? 0 : 1;
    int b_prio = is_structured(b.type) ? 0 : 1;
    if (a_prio != b_prio) return a_prio < b_prio;
    if (a.end != b.end) return a.end > b.end;  // longer span first
    return a.confidence > b.confidence;
  });

  std::vector<PIIEntity> deduped;
  deduped.reserve(result.entities.size());

  for (auto &ent : result.entities) {
    if (!deduped.empty() && ent.start < deduped.back().end) {
      // Overlaps with the already-accepted entity — discard.
      continue;
    }
    deduped.push_back(std::move(ent));
  }

  result.entities = std::move(deduped);
}

void PIIDetector::SortByPosition(DetectionResult &result) {
  std::sort(result.entities.begin(), result.entities.end(),
            [](const PIIEntity &a, const PIIEntity &b) { return a.start < b.start; });
}

// BuildGazetteer — world cities, countries and major Chinese regions.
void PIIDetector::BuildGazetteer() {
  auto add = [&](const std::string &name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    m_gazetteer[lower] = name;
  };
  for (auto &s : {"New York",     "Los Angeles",
                  "Chicago",      "Houston",
                  "Phoenix",      "Philadelphia",
                  "San Antonio",  "San Diego",
                  "Dallas",       "San Jose",
                  "London",       "Paris",
                  "Berlin",       "Madrid",
                  "Rome",         "Amsterdam",
                  "Brussels",     "Vienna",
                  "Zurich",       "Geneva",
                  "Stockholm",    "Oslo",
                  "Copenhagen",   "Helsinki",
                  "Warsaw",       "Prague",
                  "Budapest",     "Bucharest",
                  "Athens",       "Lisbon",
                  "Dublin",       "Edinburgh",
                  "Toronto",      "Montreal",
                  "Vancouver",    "Ottawa",
                  "Calgary",      "Sydney",
                  "Melbourne",    "Brisbane",
                  "Perth",        "Auckland",
                  "Tokyo",        "Osaka",
                  "Kyoto",        "Seoul",
                  "Busan",        "Taipei",
                  "Singapore",    "Bangkok",
                  "Jakarta",      "Manila",
                  "Kuala Lumpur", "Beijing",
                  "Shanghai",     "Guangzhou",
                  "Shenzhen",     "Chengdu",
                  "Hangzhou",     "Wuhan",
                  "Nanjing",      "Chongqing",
                  "Tianjin",      "Suzhou",
                  "Zhengzhou",    "Qingdao",
                  "Shenyang",     "Changsha",
                  "Xiamen",       "Ningbo",
                  "Hefei",        "Jinan",
                  "Harbin",       "Dalian",
                  "Mumbai",       "Delhi",
                  "Bangalore",    "Hyderabad",
                  "Chennai",      "Kolkata",
                  "Pune",         "Ahmedabad",
                  "Dubai",        "Abu Dhabi",
                  "Riyadh",       "Doha",
                  "Kuwait City",  "Cairo",
                  "Lagos",        "Nairobi",
                  "Johannesburg", "Cape Town",
                  "Moscow",       "Saint Petersburg",
                  "Istanbul",     "Ankara",
                  "São Paulo",    "Rio de Janeiro",
                  "Buenos Aires", "Lima",
                  "Bogotá",       "Santiago",
                  "Mexico City",  "Guadalajara",
                  "Hong Kong",    "Macau"}) {
    add(s);
  }
  for (auto &s :
       {"China",       "United States", "United Kingdom", "Germany",      "France",    "Japan",          "South Korea",
        "India",       "Australia",     "Canada",         "Brazil",       "Russia",    "Italy",          "Spain",
        "Mexico",      "Indonesia",     "Netherlands",    "Saudi Arabia", "Turkey",    "Switzerland",    "Argentina",
        "Sweden",      "Poland",        "Belgium",        "Thailand",     "Singapore", "Malaysia",       "Vietnam",
        "Philippines", "Pakistan",      "Bangladesh",     "Nigeria",      "Egypt",     "South Africa",   "Israel",
        "UAE",         "Iran",          "Iraq",           "Ukraine",      "Romania",   "Czech Republic", "Portugal",
        "Greece",      "Hungary",       "New Zealand",    "Norway",       "Denmark",   "Finland",        "Austria"}) {
    add(s);
  }
  for (auto &s :
       {"北京", "上海", "广州", "深圳",   "成都",     "杭州",   "武汉",     "南京",  "西安",     "重庆",   "天津",
        "苏州", "郑州", "青岛", "沈阳",   "长沙",     "厦门",   "宁波",     "合肥",  "济南",     "哈尔滨", "大连",
        "福州", "昆明", "太原", "石家庄", "贵阳",     "南宁",   "乌鲁木齐", "拉萨",  "呼和浩特", "银川",   "西宁",
        "海口", "广东", "浙江", "江苏",   "四川",     "湖北",   "湖南",     "河南",  "河北",     "山东",   "山西",
        "陕西", "辽宁", "吉林", "黑龙江", "安徽",     "福建",   "江西",     "云南",  "贵州",     "广西",   "内蒙古",
        "新疆", "西藏", "宁夏", "青海",   "香港",     "澳门",   "台湾",     "中国",  "美国",     "英国",   "德国",
        "法国", "日本", "韩国", "印度",   "澳大利亚", "加拿大", "俄罗斯",   "新加坡"}) {
    add(s);
  }
}

void PIIDetector::RunGazetteer(const std::string &text, DetectionResult &result) {
  if (m_gazetteer.empty() || text.empty()) return;
  std::string lower_text = text;
  std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  for (const auto &[lower_name, display_name] : m_gazetteer) {
    size_t search_from = 0;
    while (true) {
      size_t pos = lower_text.find(lower_name, search_from);
      if (pos == std::string::npos) break;
      size_t end_pos = pos + lower_name.size();

      bool is_ascii_entry = (static_cast<unsigned char>(lower_name[0]) < 0x80);
      if (is_ascii_entry) {
        bool left_ok = (pos == 0) || !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
        bool right_ok = (end_pos >= text.size()) || !std::isalnum(static_cast<unsigned char>(text[end_pos]));
        if (!left_ok || !right_ok) {
          search_from = pos + 1;
          continue;
        }
      }

      PIIEntity ent;
      ent.type = EntityType::LOCATION;
      ent.start = pos;
      ent.end = end_pos;
      ent.text = text.substr(pos, end_pos - pos);
      ent.confidence = 0.93f;
      result.entities.push_back(std::move(ent));
      search_from = end_pos;
    }
  }
}

void PIIDetector::MergeAdjacentNERSpans(const std::string &text, DetectionResult &result) {
  auto is_ner = [](EntityType t) {
    return t == EntityType::PERSON || t == EntityType::ORGANIZATION || t == EntityType::LOCATION ||
           t == EntityType::MISC;
  };

  bool merged = true;
  while (merged) {
    merged = false;
    std::vector<PIIEntity> out;
    out.reserve(result.entities.size());
    size_t i = 0;
    while (i < result.entities.size()) {
      if (i + 1 < result.entities.size()) {
        const PIIEntity &cur = result.entities[i];
        const PIIEntity &next = result.entities[i + 1];
        size_t gap = (next.start >= cur.end) ? next.start - cur.end : 0;

        if (is_ner(cur.type) && is_ner(next.type) && gap <= 2) {
          bool gap_ok = true;
          for (size_t g = cur.end; g < next.start && g < text.size(); ++g) {
            unsigned char c = static_cast<unsigned char>(text[g]);
            if (c != ' ' && c != '-' && c != '\t') {
              gap_ok = false;
              break;
            }
          }
          if (gap_ok) {
            PIIEntity m;
            m.type =
                (cur.type == EntityType::PERSON || next.type == EntityType::PERSON) ? EntityType::PERSON : cur.type;
            m.start = cur.start;
            m.end = next.end;
            m.confidence = (cur.confidence + next.confidence) / 2.0f;
            m.text = text.substr(m.start, m.end - m.start);
            out.push_back(std::move(m));
            i += 2;
            merged = true;
            continue;
          }
        }
      }
      out.push_back(std::move(result.entities[i]));
      ++i;
    }
    result.entities = std::move(out);
  }
}

void PIIDetector::NormalizeNERTypes(DetectionResult &result) {
  static const std::unordered_set<std::string> org_loc_hints = {
      "inc",      "ltd",        "llc",     "corp",     "co",     "company",   "group", "holdings", "bank",
      "fund",     "university", "college", "hospital", "school", "city",      "town",  "county",   "district",
      "province", "state",      "street",  "avenue",   "road",   "boulevard", "lane",  "drive",    "有限公司",
      "集团",     "银行",       "大学",    "医院",     "市",     "县",        "省"};
  for (auto &ent : result.entities) {
    if (ent.type != EntityType::MISC) continue;
    const std::string &span = ent.text;
    if (span.empty()) continue;

    // (a) No digits.
    bool has_digit = false;
    for (unsigned char c : span)
      if (std::isdigit(c)) {
        has_digit = true;
        break;
      }
    if (has_digit) continue;

    // (b) Every ASCII word must start with uppercase.
    bool all_title = true;
    bool word_start = true;
    for (unsigned char c : span) {
      if (c == ' ' || c == '-') {
        word_start = true;
        continue;
      }
      if (word_start && c < 0x80 && !(c >= 'A' && c <= 'Z')) {
        all_title = false;
        break;
      }
      word_start = false;
    }
    if (!all_title) continue;

    // (c) Stopword check.
    std::string lower = span;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (m_options.ner_stopwords.count(lower)) continue;

    // (d) ORG/LOC suffix check.
    bool has_hint = false;
    std::istringstream ss(lower);
    std::string word;
    while (ss >> word) {
      while (!word.empty() && std::ispunct(static_cast<unsigned char>(word.back()))) word.pop_back();
      if (org_loc_hints.count(word)) {
        has_hint = true;
        break;
      }
    }
    if (has_hint) continue;

    ent.type = EntityType::PERSON;
  }
}

std::string PIIDetector::BuildDetectionJSON(const std::string & /*text*/, const DetectionResult &result) const {
  std::ostringstream out;
  out << "{\n  \"entities\": [";

  for (size_t i = 0; i < result.entities.size(); ++i) {
    const auto &ent = result.entities[i];
    if (i > 0) out << ",";
    out << "\n    {"
        << "\n      \"type\": \"" << EntityTypeToString(ent.type) << "\","
        << "\n      \"text\": \"" << EscapeJSON(ent.text) << "\","
        << "\n      \"start\": " << ent.start << ","
        << "\n      \"end\": " << ent.end << ","
        << "\n      \"confidence\": " << std::fixed << std::setprecision(4) << ent.confidence << "\n    }";
  }

  out << "\n  ],\n  \"summary\": {\n";
  out << "    \"total_entities\": " << result.entities.size() << ",\n";
  out << "    \"types\": [";

  std::vector<std::string> seen_types;
  for (const auto &ent : result.entities) {
    std::string ts = EntityTypeToString(ent.type);
    if (std::find(seen_types.begin(), seen_types.end(), ts) == seen_types.end()) seen_types.push_back(ts);
  }
  for (size_t i = 0; i < seen_types.size(); ++i) {
    if (i > 0) out << ", ";
    out << "\"" << seen_types[i] << "\"";
  }

  out << "]\n  }\n}";
  return out.str();
}

std::string PIIDetector::ApplyMasking(const std::string &text, const DetectionResult &result,
                                      const MaskingOptions &opts) const {
  if (result.entities.empty()) return text;

  std::string out;
  out.reserve(text.size());
  size_t cur = 0;

  for (const auto &ent : result.entities) {
    if (ent.start > cur) out += text.substr(cur, ent.start - cur);

    std::string replacement;
    switch (opts.policy) {
      case MaskingPolicy::REPLACE_WITH_TYPE:
        replacement = ReplaceWithType(ent.type);
        break;
      case MaskingPolicy::REPLACE_WITH_MASK:
        replacement = "***";
        break;
      case MaskingPolicy::HASH:
        replacement = HashText(ent.text);
        break;
      case MaskingPolicy::PRESERVE_FORMAT:
        replacement = PreserveFormatMask(ent.text, ent.type);
        break;
      case MaskingPolicy::CUSTOM:
        replacement = ApplyCustomRule(ent.text, ent.type, opts);
        break;
    }

    out += replacement;
    cur = ent.end;
  }

  if (cur < text.size()) out += text.substr(cur);
  return out;
}

std::string PIIDetector::ReplaceWithType(EntityType type) const { return "[" + EntityTypeToString(type) + "]"; }

std::string PIIDetector::HashText(const std::string &text) const {
  // DJB2 hash — lightweight, no external dependency.
  uint64_t hash = 5381;
  for (unsigned char c : text) hash = ((hash << 5) + hash) + c;

  std::ostringstream oss;
  oss << std::hex << std::setw(16) << std::setfill('0') << hash;
  return "[HASH:" + oss.str() + "]";
}

std::string PIIDetector::PreserveFormatMask(const std::string &text, EntityType type) const {
  switch (type) {
    case EntityType::EMAIL: {
      auto at_pos = text.find('@');
      if (at_pos == std::string::npos) return "***@***.***";
      auto dot_pos = text.rfind('.');
      std::string domain = (dot_pos != std::string::npos && dot_pos > at_pos)
                               ? text.substr(dot_pos)  // ".com", ".org", etc.
                               : ".***";
      return "***@***" + domain;
    }
    case EntityType::PHONE_NUMBER: {
      // Keep last 4 digits, mask rest with *s.
      std::string digits_only;
      for (char c : text)
        if (std::isdigit(static_cast<unsigned char>(c))) digits_only += c;
      std::string masked(digits_only.size() > 4 ? digits_only.size() - 4 : 0, '*');
      if (digits_only.size() >= 4) masked += digits_only.substr(digits_only.size() - 4);
      return masked;
    }
    case EntityType::CREDIT_CARD: {
      // Keep last 4 digits, mask rest.
      std::string digits_only;
      for (char c : text)
        if (std::isdigit(static_cast<unsigned char>(c))) digits_only += c;
      if (digits_only.size() < 4) return "****";
      return "****-****-****-" + digits_only.substr(digits_only.size() - 4);
    }
    case EntityType::SSN:
      return "***-**-" + text.substr(text.size() > 4 ? text.size() - 4 : 0);
    case EntityType::IP_ADDRESS:
      return "***.***.***.***";
    default:
      // Generic: mask all characters except punctuation structure.
      std::string masked = text;
      for (char &c : masked)
        if (std::isalnum(static_cast<unsigned char>(c))) c = '*';
      return masked;
  }
}

std::string PIIDetector::ApplyCustomRule(const std::string &text, EntityType type, const MaskingOptions &opts) const {
  std::string type_str = EntityTypeToString(type);
  auto it = opts.custom_rules.find(type_str);
  if (it == opts.custom_rules.end()) {
    // Fallback to replace_with_type for unspecified types.
    return ReplaceWithType(type);
  }

  const std::string &rule = it->second;

  // Handle special template placeholders.
  if (rule == "keep_last_4") {
    std::string digits;
    for (char c : text)
      if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
    return digits.size() >= 4 ? digits.substr(digits.size() - 4) : digits;
  }

  // Placeholder {last4}: embed last 4 digits of text.
  std::string result = rule;
  const std::string ph = "{last4}";
  auto ph_pos = result.find(ph);
  if (ph_pos != std::string::npos) {
    std::string digits;
    for (char c : text)
      if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
    std::string last4 = digits.size() >= 4 ? digits.substr(digits.size() - 4) : digits;
    result.replace(ph_pos, ph.size(), last4);
  }

  return result;
}

std::string PIIDetector::EntityTypeToString(EntityType type) {
  switch (type) {
    case EntityType::PERSON:
      return "PERSON";
    case EntityType::ORGANIZATION:
      return "ORGANIZATION";
    case EntityType::LOCATION:
      return "LOCATION";
    case EntityType::MISC:
      return "MISC";
    case EntityType::EMAIL:
      return "EMAIL";
    case EntityType::PHONE_NUMBER:
      return "PHONE_NUMBER";
    case EntityType::CREDIT_CARD:
      return "CREDIT_CARD";
    case EntityType::SSN:
      return "SSN";
    case EntityType::IP_ADDRESS:
      return "IP_ADDRESS";
    case EntityType::DATE_OF_BIRTH:
      return "DATE_OF_BIRTH";
    case EntityType::DATETIME:
      return "DATETIME";
    case EntityType::STREET_ADDRESS:
      return "STREET_ADDRESS";
    default:
      return "UNKNOWN";
  }
}

PIIDetector::EntityType PIIDetector::LabelToEntityType(const std::string &bio_label) {
  if (bio_label == "PER") return PIIDetector::EntityType::PERSON;
  if (bio_label == "ORG") return PIIDetector::EntityType::ORGANIZATION;
  if (bio_label == "LOC") return PIIDetector::EntityType::LOCATION;
  if (bio_label == "MISC") return PIIDetector::EntityType::MISC;
  // Some models use full names.
  if (bio_label == "PERSON") return PIIDetector::EntityType::PERSON;
  if (bio_label == "ORGANIZATION") return PIIDetector::EntityType::ORGANIZATION;
  if (bio_label == "LOCATION") return PIIDetector::EntityType::LOCATION;
  return PIIDetector::EntityType::UNKNOWN;
}

PIIDetector::EntityType PIIDetector::LabelIdToEntityType(int label_id) const {
  if (label_id < 0 || static_cast<size_t>(label_id) >= m_options.label_names.size())
    return PIIDetector::EntityType::UNKNOWN;
  const std::string &label = m_options.label_names[label_id];
  if (label.size() < 2 || label[1] != '-') return PIIDetector::EntityType::UNKNOWN;
  return LabelToEntityType(label.substr(2));
}

std::string PIIDetector::EscapeJSON(const std::string &s) {
  std::ostringstream oss;
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        oss << "\\\"";
        break;
      case '\\':
        oss << "\\\\";
        break;
      case '\b':
        oss << "\\b";
        break;
      case '\f':
        oss << "\\f";
        break;
      case '\n':
        oss << "\\n";
        break;
      case '\r':
        oss << "\\r";
        break;
      case '\t':
        oss << "\\t";
        break;
      default:
        if (c < 0x20) {
          oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
        } else {
          oss << static_cast<char>(c);
        }
    }
  }
  return oss.str();
}
}  // namespace ML
}  // namespace ShannonBase