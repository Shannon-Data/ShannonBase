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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/

#ifndef __SHANNONBASE_IMCU_H__
#define __SHANNONBASE_IMCU_H__

#include <mutex>
#include <map>
#include <string>

#include "field_types.h" //for MYSQL_TYPE_XXX
#include "my_inttypes.h"
#include "my_list.h" //for LIST
#include "sql/table.h" //for TABLE

#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/include/rapid_object.h"

namespace ShannonBase{
namespace Imcs{

class Imcu : public MemoryObject{
public:

  class Imcu_header_t {
  public:
    uint m_num_cu;
    uint m_num_rows;
    //statistics information, just like: maximum, minmum, median,middle.
    double m_max_value, m_min_value, m_median, m_middle;
    double m_avg;
    //has generated column or not.
    bool m_has_vcol;
    //db name and table name which imcu belongs to.
    std::string m_db, m_table;
    //fields
    uint m_fields;
    std::vector<Field*> m_field;
  };
  using Imcu_header = Imcu_header_t;

  Imcu();
  Imcu(uint num_cus);
  virtual ~Imcu();

  uint Build_header (const TABLE& table_arg);
  Imcu_header& Get_header() { return m_headers; }
  Cu* Get_cu(std::string& field_name);
private:
  // use to proctect header.
  std::mutex m_mutex_header;
  Imcu_header m_headers;

  //The all CUs in this IMCU.
  std::map<std::string, Cu*> m_cus;
};

} //ns: imcs
} //ns:shannonbase

#endif //__SHANNONBASE_IMCU_H__