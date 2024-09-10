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
/**
 * The specification of IMCU, pls ref:
 * https://github.com/Shannon-Data/ShannonBase/issues/8
 * ------------------------------------------------+
 * |  | CU1 | | CU2 |                     |  CU |  |
 * |  |     | |     |  IMCU1              |     |  |
 * |  |     | |     |                     |     |  |
 * +-----------------------------------------------+
 * ------------------------------------------------+
 * |  | CU1 | | CU2 |                     |  CU |  |
 * |  |     | |     |  IMCU2              |     |  |
 * |  |     | |     |                     |     |  |
 * +-----------------------------------------------+
 * ...
 *  * ------------------------------------------------+
 * |  | CU1 | | CU2 |                     |  CU |  |
 * |  |     | |     |  IMCUN              |     |  |
 * |  |     | |     |                     |     |  |
 * +-----------------------------------------------+
 *
 */
#ifndef __SHANNONBASE_IMCU_H__
#define __SHANNONBASE_IMCU_H__

#include <atomic>  //std::atomic<T>
#include <map>
#include <mutex>
#include <string>

#include "field_types.h"  //for MYSQL_TYPE_XXX
#include "my_inttypes.h"
#include "my_list.h"    //for LIST
#include "sql/table.h"  //for TABLE

#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/include/rapid_object.h"

class Field;
namespace ShannonBase {
namespace Imcs {

class Imcu : public MemoryObject {
 public:
  class Imcu_header {
   public:
    // statistics information, just like: maximum, minmum, median,middle.
    std::atomic<double> m_max_value, m_min_value, m_median, m_middle;
    std::atomic<double> m_avg;
    // has generated column or not.
    bool m_has_vcol;
    // db name and table name which imcu belongs to.
    std::string m_db, m_table;
    // fields
    uint m_fields;
    std::vector<Field *> m_field;
  };  // Imcu_header
 public:
  Imcu(const TABLE &table_arg);
  virtual ~Imcu();

  Imcu_header &get_header() { return m_headers; }
  Cu *get_cu(const Field *);

  inline void set_previous(Imcu *prev) { this->m_prev = prev; }
  inline void set_next(Imcu *next) { this->m_next = next; }

  inline Imcu *get_next() { return m_next; }
  inline Imcu *get_prev() { return m_prev; }

 private:
  bool is_full();

 private:
  // use to proctect header.
  std::mutex m_mutex_header;
  Imcu_header m_headers;
  // use to protect cus in imcu.
  std::mutex m_mutex_cus;
  // The all CUs in this IMCU.
  std::unordered_map<Field *, std::unique_ptr<Cu>> m_cus;
  // name of this imcs. 'db_name + table_name'
  std::string m_name;
  // point to the next imcu.
  Imcu *m_prev, *m_next, *m_curr;
  // magic num and version
  uint m_version_num, m_magic_num;
};

// As the description above, Imcus consists of an imcu store.

class Imcu_store {};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_IMCU_H__