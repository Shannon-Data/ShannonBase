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

   The ML_GENERATE routine uses the specified large language model (LLM) to
   generate text-based content as a response for the given natural-language
   query.
   Copyright (c) 2023-, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_ML_GENERATE_H__
#define __SHANNONBASE_ML_GENERATE_H__

#include <memory>
#include <string>
#include <vector>

#include "sql-common/json_dom.h"  //Json_wrapper.

#include "ml_algorithm.h"

class Json_wrapper;
namespace ShannonBase {
namespace ML {

class ML_generate : public ML_algorithm {
 public:
  ML_generate() = default;
  virtual ~ML_generate() override = default;
  virtual void Generate() = 0;
};

class ML_generate_row : public ML_generate {
 public:
  ML_generate_row() = default;
  virtual ~ML_generate_row() override = default;
  virtual void Generate() override;
};

class ML_generate_table : public ML_generate {
 public:
  ML_generate_table() = default;
  virtual ~ML_generate_table() override = default;
  virtual void Generate() override;
};

}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ML_GENERATE_H__