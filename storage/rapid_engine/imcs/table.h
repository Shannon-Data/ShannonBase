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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. Rapid Table.
*/
#ifndef __SHANNONBASE_RAPID_TABLE_H__
#define __SHANNONBASE_RAPID_TABLE_H__

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "storage/rapid_engine/imcs/index/index.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"

class TABLE;
class Field;
namespace ShannonBase {
class Rapid_load_context;
namespace Imcs {
class Cu;
class RapidTable : public MemoryObject {
 public:
  RapidTable() = default;
  RapidTable(std::string schema, std::string table) : m_schema_name(schema), m_table_name(table) {}
  virtual ~RapidTable() = default;

  RapidTable(RapidTable &&) = default;
  RapidTable &operator=(RapidTable &&) = default;

  RapidTable(const RapidTable &) = delete;
  RapidTable &operator=(const RapidTable &) = delete;

  virtual int build_field_memo(const Rapid_load_context *context, Field *field);
  virtual int build_hidden_index_memo(const Rapid_load_context *context);
  virtual int build_user_defined_index_memo(const Rapid_load_context *context);

  int build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid);

  virtual int write(const Rapid_load_context *, uchar *) { return 0; }

  virtual Cu *first_field() { return m_fields.begin()->second.get(); }
  virtual Cu *get_field(std::string field_name);

  // name of schema.
  std::string m_schema_name;

  // name of this table.
  std::string m_table_name;

  // the loaded cus. key format: field/column name.
  std::shared_mutex m_fields_mutex;
  std::unordered_map<std::string, std::unique_ptr<Cu>> m_fields;

  // key format: key_name
  // value format: vector<park part1 name , key part2 name>.
  std::unordered_map<std::string, key_meta_t> m_source_keys;

  // key format: key_name.
  std::unordered_map<std::string, std::unique_ptr<Index::Index<uchar, row_id_t>>> m_indexes;
};

class Table : public RapidTable {
 public:
  Table() = default;
  Table(std::string schema, std::string table) : RapidTable(schema, table) {}
  virtual ~Table() {}

  virtual int build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid) final;
  virtual int write(const Rapid_load_context *context, uchar *data) final;

 private:
  int build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid);
};

class PartTable : public RapidTable {
 public:
  PartTable() = default;
  PartTable(std::string schema, std::string table) : RapidTable(schema, table) {}
  virtual ~PartTable() {}

  virtual int build_field_memo(const Rapid_load_context *context, Field *field) final;
  virtual int build_hidden_index_memo(const Rapid_load_context *context) final;
  virtual int build_user_defined_index_memo(const Rapid_load_context *context) final;

  virtual int write(const Rapid_load_context *context, uchar *data) final;
  virtual int build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid) final;

 private:
  int build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid);
};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_TABLE_H__