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
   To uses the specified embedding model to encode the specified text or query
   into a vector embedding.

   Copyright (c) 2023-, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_ML_EMBEDDING_H__
#define __SHANNONBASE_ML_EMBEDDING_H__

#include <memory>
#include <string>
#include <vector>

#include "sql-common/json_dom.h"  //Json_wrapper.

#include "ml_algorithm.h"

class Json_wrapper;
namespace ShannonBase {
namespace ML {

class ML_embedding : public ML_algorithm {
 public:
  ML_embedding() = default;
  virtual ~ML_embedding() override = default;
  virtual void GenerateEmbedding() = 0;
};

class ML_embedding_row : public ML_embedding {
  ML_embedding_row();
  virtual ~ML_embedding_row() override {}
  virtual void GenerateEmbedding() override;
};

class ML_embedding_table : public ML_embedding {
  ML_embedding_table();
  virtual ~ML_embedding_table() override {}
  virtual void GenerateEmbedding() override;
};

}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ML_EMBEDDING_H__