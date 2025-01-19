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

#ifndef __SHANNONBASE_RAPID_ML_H__
#define __SHANNONBASE_RAPID_ML_H__

#include <string>

class THD;
class JOIN;

namespace ShannonBase {
namespace ML {

class Query_arbitrator {
 public:
  enum class WHERE2GO { TO_PRIMARY, TO_SECONDARY };

  Query_arbitrator() = default;
  ~Query_arbitrator() = default;

  void set_model_path(const std::string &model_path) { m_model_path = model_path; }
  // load the trainned model, which was based on query information-`JOIN`.
  virtual void load_model();
  virtual WHERE2GO predict(JOIN *);

 private:
  std::string m_model_path;
};

}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_ML_H__