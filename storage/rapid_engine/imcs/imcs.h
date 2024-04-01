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

#ifndef __SHANNONBASE_IMCS_H__
#define __SHANNONBASE_IMCS_H__

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include "my_inttypes.h"
#include "sql/handler.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"  //for local dictionary.
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"

class Field;
namespace ShannonBase {
class RapidContext;
extern std::map<std::string, std::unique_ptr<Compress::Dictionary>>
    loaded_dictionaries;
namespace Imcs {
// the memory size of allocation for imcs to store the loaded data.
extern unsigned long rapid_memory_size;
extern unsigned long rapid_chunk_size;
class Cu;
class Imcu;
class Imcs : public MemoryObject {
 public:
  using Cu_map_t = std::unordered_map<std::string, std::unique_ptr<Cu>>;
  using Imcu_map_t = std::multimap<std::string, std::unique_ptr<Imcu>>;
  inline static Imcs *get_instance() {
    std::call_once(one, [&] { m_instance = new Imcs(); });
    return m_instance;
  }
  // initialize the imcs.
  uint initialize();
  // deinitialize the imcs.
  uint deinitialize();
  // gets initialized flag.
  inline bool initialized() {
    return (m_inited == handler::NONE) ? false : true;
  }
  // scan oper initialization.
  uint rnd_init(bool scan);
  // end of scanning
  uint rnd_end();
  // writes a row of a column in.
  uint write_direct(ShannonBase::RapidContext *context, Field *field);
  // writes a row of a column in.
  uint write_direct(ShannonBase::RapidContext *context, const char* schema_name,
                    const char* table_name, const char*field_name,
                    const uchar* field_value, uint val_len);
  // reads the data by a rowid into a field.
  uint read_direct(ShannonBase::RapidContext *context, Field *field);
  // reads the data by a rowid into buffer.
  uint read_direct(ShannonBase::RapidContext *context, uchar *buffer);
  uint read_batch_direct(ShannonBase::RapidContext *context, uchar *buffer);
  // deletes the data by a rowid
  uint delete_direct(ShannonBase::RapidContext *context, Field *field);
  //deletes the data by key. pk_value is key value of row you want to delete.
  uint delete_direct(ShannonBase::RapidContext *context, const char* schema_name,
                    const char* table_name, const char*field_name,
                    const uchar* pk_value, uint pk_len);
  // deletes all the data.
  uint delete_all_direct(ShannonBase::RapidContext *context);
  uint update_direct(ShannonBase::RapidContext *context, const char* schema_name,
                    const char* table_name, const char*field_name,
                    const uchar* new_value, uint new_value_len, bool in_place_update = false);
  Cu *get_cu(std::string &key);
  void add_cu(std::string key, std::unique_ptr<Cu> &cu);
  ha_rows get_rows(TABLE *source_table);
  bool is_empty() { return m_cus.empty(); }
 private:
  // make ctor and dctor private.
  Imcs();
  virtual ~Imcs();

  Imcs(Imcs &&) = delete;
  Imcs(Imcs &) = delete;
  Imcs &operator=(const Imcs &) = delete;
  Imcs &operator=(const Imcs &&) = delete;

  /*if this field has been loaded into rapid, then return its key,
  or return empty string*/
  inline std::string get_key_name(const char* schema, const char* table, const char* field) {
    std::ostringstream ostr;
    ostr << schema << table << field;
    std::string key_name = ostr.str();
    auto elem = m_cus.find(key_name);
    if (elem == m_cus.end()) {  // a new field. not found. not  be loaded.
      return "";
    } else return key_name;

    return "";
  }
 private:
  // imcs instance
  static Imcs *m_instance;
  // initialization flag, only once.
  static std::once_flag one;
  // cus in this imcs. <db+table+col, cu*>
  Cu_map_t m_cus;
  // imcu in this imcs. <db name + table name, imcu*>
  Imcu_map_t m_imcus;
  // used to keep all allocated imcus. key string: db_name + table_name.
  // initialization flag.
  std::atomic<uint8> m_inited{handler::NONE};
};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_IMCS_H__