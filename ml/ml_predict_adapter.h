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

   The fundmental code for ML.

   Copyright (c) 2023-, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_ML_PREDICT_ADAPTER_H__
#define __SHANNONBASE_ML_PREDICT_ADAPTER_H__

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "sql/field.h"
#include "sql/table.h"

#include "ml_info.h"
#include "ml_utils.h"

namespace ShannonBase {
namespace ML {

// A numeric field is read through val_real(): no per-cell std::string and no
// std::stod. Temporal and string fields keep the text path so predictions stay
// bit-identical to the previous implementation.
inline bool predict_field_is_numeric(enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_YEAR:
      return true;
    default:
      return false;
  }
}

// One model feature bound to a table column. `dict` points into the caller's
// txt2numeric map, which must outlive the adapter.
struct Predict_feature {
  uint field_idx{0};
  const std::set<std::string> *dict{nullptr};
  bool numeric{false};
};

// Maps a table row to model input, mirroring IMLane's ToLaneFormat /
// FromLaneFormat: extract binds features to columns once per statement, encode
// appends one row's values to the batch buffer, assemble picks a row's score
// out of the batch result. EngineRow is the engine's row image (Field ** for a
// table record), ModelInput the encoded feature buffer.
template <typename EngineRow, typename ModelInput>
class PredictAdapter {
 public:
  // Extension point 1: bind the model's feature names to row positions.
  bool extract(TABLE *table, const std::vector<std::string> &feature_names, const txt2numeric_map_t &txt2numeric) {
    m_features.clear();
    m_features.reserve(feature_names.size());
    for (const auto &name : feature_names) {
      int idx{-1};
      for (uint i = 0; i < table->s->fields; ++i) {
        if (name == table->field[i]->field_name) {
          idx = static_cast<int>(i);
          break;
        }
      }
      if (idx < 0) return false;

      Predict_feature feature;
      feature.field_idx = static_cast<uint>(idx);
      auto dict_it = txt2numeric.find(name);
      if (dict_it != txt2numeric.end())
        feature.dict = &dict_it->second;
      else
        feature.numeric = predict_field_is_numeric(table->field[idx]->type());
      m_features.push_back(feature);
    }
    return true;
  }

  // Extension point 2: append width() encoded values of one row.
  void encode(const EngineRow &row, ModelInput &out) const {
    for (const auto &feature : m_features) {
      Field *field = row[feature.field_idx];
      const bool is_null = (field == nullptr || field->is_null());
      // Numeric columns are read directly: no per-cell string, no std::stod.
      if (feature.numeric) {
        out.push_back(is_null ? 0.0 : field->val_real());
        continue;
      }

      // A NULL cell used to be encoded as the text "0", which matters when a
      // categorical column also has a "0" category, so keep that value.
      const char *text_ptr{"0"};
      size_t text_len{1};
      String buffer;
      if (!is_null) {
        buffer.set_charset(field->charset());
        String *text = field->val_str(&buffer);
        if (text != nullptr) {
          text_ptr = text->ptr();
          text_len = text->length();
        }
      }

      if (feature.dict != nullptr && !feature.dict->empty()) {
        auto it = feature.dict->find(std::string(text_ptr, text_len));
        out.push_back(it != feature.dict->end() ? static_cast<double>(std::distance(feature.dict->begin(), it))
                                                : static_cast<double>(feature.dict->size()));
      } else if (feature.dict != nullptr) {
        out.push_back(0.0);  // empty dictionary: nothing to map to
      } else {
        out.push_back(Utils::to_double_or(std::string(text_ptr, text_len), 0.0));
      }
    }
  }

  // Extension point 3: fold the batch output back into one row's result.
  static double assemble(const std::vector<double> &scores, size_t row, size_t stride) { return scores[row * stride]; }

  size_t width() const { return m_features.size(); }

 private:
  std::vector<Predict_feature> m_features;
};

}  // namespace ML
}  // namespace ShannonBase

#endif  // __SHANNONBASE_ML_PREDICT_ADAPTER_H__
