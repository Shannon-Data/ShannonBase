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

   Generates SQL queries using natural-language statements. The routine also
   runs the generated SQL statement and displays the result set. You can use
   this routine for generating and running SQL queries only for databases
   and tables that you have access to.

   The LLM-generated SQL statements might contain syntax errors.
   The routine automatically detects these errors, and retries the SQL
   generation until a syntactically valid SQL statement is generated,
   with a maximum of 3 generation attempts.

   Copyright (c) 2023-, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_ML_NL2SQL_H__
#define __SHANNONBASE_ML_NL2SQL_H__

#include <memory>
#include <string>
#include <vector>

#include "sql-common/json_dom.h"  //Json_wrapper.

#include "ml_algorithm.h"

class Json_wrapper;
namespace ShannonBase {
namespace ML {
class ML_chat : public ML_algorithm {
 public:
  ML_chat() = default;
  virtual ~ML_chat() override = default;
  virtual void Generate();
};

class ML_nl2sql : public ML_algorithm {
 public:
  ML_nl2sql() = default;
  virtual ~ML_nl2sql() override = default;
  virtual void Generate();
};
}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ML_NL2SQL_H__