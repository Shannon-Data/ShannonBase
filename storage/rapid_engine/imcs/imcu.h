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

#include "field_types.h" //for MYSQL_TYPE_XXX
#include "my_inttypes.h"

#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/imcs/cu.h"

namespace ShannonBase{
namespace Imcs{

class Imcu_header{

private:
 uint m_num_cu;
 //statistics information, just like: maximum, minmum, median,middle.
 ulonglong m_max_value, m_min_value, m_median, m_middle, m_avg;
 uint m_num_rows, m_num_columns;

 //has generated column or not.
 bool m_has_vcol;
};

class Imcu {
public:
 Imcu() {}
 virtual ~Imcu() {}

 Imcu_header& Get_header() { return m_headers; }
private:
 // use to proctect header.
 std::mutex m_mutex_header;
 Imcu_header m_headers;

 //The all CUs in this IMCU.
 std::map<std::string, Cu> m_cus;
};

} //ns: imcs
} //ns:shannonbase

#endif //__SHANNONBASE_IMCU_H__