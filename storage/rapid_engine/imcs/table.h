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
#include <ctime>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "storage/rapid_engine/imcs/index/index.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/trx/transaction.h"

class TABLE;
class Field;
namespace ShannonBase {
class Rapid_context;
class Rapid_load_context;
class Rapid_scan_context;
namespace Imcs {
class Cu;
class RapidTable : public MemoryObject {
 public:
  enum class LoadType {
    NOT_LOADED = 0,  // un-loaded
    SELF_LOADED,     // system-auto-loaded
    USER_LOADED      // user-loaded.
  };

  struct TableStats {
    std::shared_mutex m_stats_lock;                         // to protect the stats modification.
    std::atomic<uint64_t> mysql_access_count{0};            // MySQL access counts.
    std::atomic<uint64_t> heatwave_access_count{0};         // Rapid access counts.
    double importance{0.0};                                 // importance score.
    std::atomic<time_t> last_accessed{std::time(nullptr)};  // the laste access time.
    LoadType load_type{LoadType::NOT_LOADED};               // load type.
    // the phyiscal # of rows in this table.
    // physical row count. If you want to get logical rows, you should consider
    // MVCC to decide that whether this phyical row is visiable or not to this
    // transaction.
    std::atomic<row_id_t> prows{0};
    void update_access_time() { last_accessed.store(std::time(nullptr), std::memory_order_relaxed); }

    // to get homw many seconds last since the last access.
    // usage: if (stats.seconds_since_last_access() > 3600)
    // more than 1 hr, maybe cold data.
    time_t seconds_since_last_access() const {
      time_t now = std::time(nullptr);
      return now - last_accessed.load(std::memory_order_relaxed);
    }
    TableStats() = default;
  };

  enum class TYPE : uint8 { UNKONWN = 0, NORAMAL, PARTTABLE };

  RapidTable() = default;
  RapidTable(std::string schema, std::string table) : m_schema_name(schema), m_table_name(table) {}
  virtual ~RapidTable() = default;

  RapidTable(RapidTable &&) = default;
  RapidTable &operator=(RapidTable &&) = default;

  RapidTable(const RapidTable &) = delete;
  RapidTable &operator=(const RapidTable &) = delete;

  virtual RapidTable::TYPE type() = 0;
  virtual int create_fields_memo(const Rapid_load_context *) = 0;
  virtual int create_index_memo(const Rapid_load_context *) = 0;

  virtual int delete_row(const Rapid_load_context *, row_id_t) = 0;
  virtual int delete_rows(const Rapid_load_context *, const std::vector<row_id_t> &) = 0;

  virtual int write_row_from_log(const Rapid_load_context *, row_id_t,
                                 std::unordered_map<std::string, mysql_field_t> &) = 0;

  virtual int update_row(const Rapid_load_context *, row_id_t, std::string &, const uchar *, size_t) = 0;

  virtual int update_row_from_log(const Rapid_load_context *, row_id_t,
                                  std::unordered_map<std::string, mysql_field_t> &) = 0;

  virtual int build_index(const Rapid_load_context *, const KEY *, row_id_t) = 0;
  virtual int build_index(const Rapid_load_context *, const KEY *, row_id_t, uchar *, ulong *, ulong *, ulong *) = 0;

  virtual int write(const Rapid_load_context *, uchar *) = 0;
  virtual int write(const Rapid_load_context *, uchar *, size_t, ulong *, size_t, ulong *, ulong *) = 0;

  virtual int build_partitions(const Rapid_load_context *) = 0;

  // gets the # of physical rows.
  virtual row_id_t rows(const Rapid_context *) = 0;

  // to reserer a row place for this operation.
  virtual row_id_t reserve_id(const Rapid_load_context *) = 0;

  virtual Cu *first_field() = 0;

  virtual Cu *get_field(std::string field_name) = 0;

  virtual Index::Index<uchar, row_id_t> *get_index(std::string key_name) = 0;

  virtual std::unordered_map<std::string, std::unique_ptr<Cu>> &get_fields() = 0;

  virtual std::unordered_map<std::string, key_meta_t> &get_source_keys() = 0;

  virtual std::string &schema_name() = 0;
  virtual std::string &name() = 0;
  virtual row_id_t reserver_rowid() = 0;
  virtual int truncate() = 0;
  virtual int rollback_changes_by_trxid(Transaction::ID trxid) = 0;

  void set_load_type(LoadType load_type) { m_load_type = load_type; }

 protected:
  TYPE m_type{TYPE::UNKONWN};

  // self load or user load.
  LoadType m_load_type{LoadType::NOT_LOADED};

  // name of schema.
  std::string m_schema_name;

  // name of this table.
  std::string m_table_name;

  // the statistic of this table.
  TableStats m_stats;

  // the loaded cus. key format: field/column name.
  std::shared_mutex m_fields_mutex;
  std::unordered_map<std::string, std::unique_ptr<Cu>> m_fields;

  // key format: key_name
  // value format: vector<park part1 name , key part2 name>.
  std::unordered_map<std::string, key_meta_t> m_source_keys;

  // key format: key_name.
  std::shared_mutex m_key_buff_mutex;

  // indexes mutex for index writing.
  std::unordered_map<std::string, std::unique_ptr<std::mutex>> m_index_mutexes;
  std::unordered_map<std::string, std::unique_ptr<Index::Index<uchar, row_id_t>>> m_indexes;
};

class Table : public RapidTable {
 public:
  Table() = default;
  Table(std::string schema, std::string table) : RapidTable(schema, table) {}
  Table(std::string schema, std::string table, std::string partkey) : RapidTable(schema, table), m_part_key(partkey) {}
  virtual ~Table() {
    m_fields.clear();
    m_source_keys.clear();
    m_indexes.clear();
  }

  virtual TYPE type() final { return RapidTable::TYPE::NORAMAL; }
  virtual int create_fields_memo(const Rapid_load_context *context) final;
  virtual int create_index_memo(const Rapid_load_context *context) final;

  virtual int build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid) final;
  virtual int build_index(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                          ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks) final;
  virtual int write(const Rapid_load_context *context, uchar *data) final;
  virtual int write(const Rapid_load_context *context, uchar *rowdata, size_t len, ulong *col_offsets, size_t n_cols,
                    ulong *null_byte_offsets, ulong *null_bitmasks) final;
  virtual int delete_row(const Rapid_load_context *context, row_id_t rowid) final;
  virtual int delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &rowids) final;

  virtual int write_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                                 std::unordered_map<std::string, mysql_field_t> &fields) final;
  virtual int update_row(const Rapid_load_context *context, row_id_t rowid, std::string &field_key,
                         const uchar *new_field_data, size_t nlen) final;
  virtual int update_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                                  std::unordered_map<std::string, mysql_field_t> &upd_recs) final;
  virtual int truncate() final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int build_partitions(const Rapid_load_context *) final { return ShannonBase::SHANNON_SUCCESS; }

  // rollback a modified record.
  virtual int rollback_changes_by_trxid(Transaction::ID trxid) final;

  // gets the # of physical rows.
  virtual row_id_t rows(const Rapid_context *) final { return m_stats.prows.load(); }

  // to reserer a row place for this operation.
  virtual row_id_t reserve_id(const Rapid_load_context *) final { return m_stats.prows.fetch_add(1); }

  virtual Cu *first_field() final { return m_fields.begin()->second.get(); }

  virtual Cu *get_field(std::string field_name) final;

  virtual Index::Index<uchar, row_id_t> *get_index(std::string key_name) final {
    if (m_indexes.find(key_name) == m_indexes.end())
      return nullptr;
    else
      return m_indexes[key_name].get();
  }

  virtual std::unordered_map<std::string, std::unique_ptr<Cu>> &get_fields() final { return m_fields; }

  virtual std::unordered_map<std::string, key_meta_t> &get_source_keys() final { return m_source_keys; }

  virtual std::string &schema_name() final { return m_schema_name; }
  virtual std::string &name() final { return m_table_name; }
  virtual row_id_t reserver_rowid() final { return m_stats.prows.fetch_add(1); }

 private:
  bool is_field_null(int field_index, const uchar *rowdata, const ulong *null_byte_offsets,
                     const ulong *null_bitmasks) {
    ulong byte_offset = null_byte_offsets[field_index];
    ulong bitmask = null_bitmasks[field_index];

    // gets null byte.
    uchar null_byte = rowdata[byte_offset];

    // check null bit.
    return (null_byte & bitmask) != 0;
  }

  int build_hidden_index_memo(const Rapid_load_context *context);
  int build_user_defined_index_memo(const Rapid_load_context *context);

  int build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid);
  int build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                       ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks);

 private:
  std::string m_part_key;
};

class PartTable : public RapidTable {
 public:
  PartTable() = default;
  PartTable(std::string schema, std::string table, std::string part_key)
      : RapidTable(schema, table), m_part_key(part_key) {}
  virtual ~PartTable() {
    m_fields.clear();
    m_source_keys.clear();
    m_indexes.clear();
  }

  virtual TYPE type() final { return RapidTable::TYPE::PARTTABLE; }

  virtual int create_fields_memo(const Rapid_load_context *) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int create_index_memo(const Rapid_load_context *) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int write(const Rapid_load_context *, uchar *) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int write(const Rapid_load_context *, uchar *, size_t, ulong *, size_t, ulong *, ulong *) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int delete_row(const Rapid_load_context *, row_id_t) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int delete_rows(const Rapid_load_context *, const std::vector<row_id_t> &) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int build_index(const Rapid_load_context *, const KEY *, row_id_t) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int build_index(const Rapid_load_context *, const KEY *, row_id_t, uchar *, ulong *, ulong *, ulong *) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int write_row_from_log(const Rapid_load_context *, row_id_t,
                                 std::unordered_map<std::string, mysql_field_t> &) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int update_row(const Rapid_load_context *, row_id_t, std::string &, const uchar *, size_t) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int update_row_from_log(const Rapid_load_context *, row_id_t,
                                  std::unordered_map<std::string, mysql_field_t> &) final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int truncate() final {
    assert(false);
    return ShannonBase::SHANNON_SUCCESS;
  }

  virtual int build_partitions(const Rapid_load_context *context) final;

  // rollback a modified record.
  virtual int rollback_changes_by_trxid(Transaction::ID) final { assert(false); }

  // gets the # of physical rows.
  virtual row_id_t rows(const Rapid_context *) final { return m_stats.prows.load(); }

  // to reserer a row place for this operation.
  virtual row_id_t reserve_id(const Rapid_load_context *) final { return m_stats.prows.fetch_add(1); }

  virtual Cu *first_field() final { return m_fields.begin()->second.get(); }

  virtual Cu *get_field(std::string) final {
    assert(false);
    return nullptr;
  }

  virtual Index::Index<uchar, row_id_t> *get_index(std::string) final {
    assert(false);
    return nullptr;
  }

  virtual std::unordered_map<std::string, std::unique_ptr<Cu>> &get_fields() final { return m_fields; }

  virtual std::unordered_map<std::string, key_meta_t> &get_source_keys() final { return m_source_keys; }

  virtual std::string &schema_name() final { return m_schema_name; }
  virtual std::string &name() final { return m_table_name; }
  virtual row_id_t reserver_rowid() final { return m_stats.prows.fetch_add(1); }

  inline RapidTable *get_partition(std::string part_key) {
    if (m_partitions.find(part_key) == m_partitions.end()) return nullptr;
    return m_partitions[part_key].get();
  }

 private:
  bool is_field_null(int field_index, const uchar *rowdata, const ulong *null_byte_offsets,
                     const ulong *null_bitmasks) {
    ulong byte_offset = null_byte_offsets[field_index];
    ulong bitmask = null_bitmasks[field_index];

    // gets null byte.
    uchar null_byte = rowdata[byte_offset];

    // check null bit.
    return (null_byte & bitmask) != 0;
  }

  int build_hidden_index_memo(const Rapid_load_context *context);
  int build_user_defined_index_memo(const Rapid_load_context *context);

  int build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid);
  int build_index_impl(const Rapid_load_context *context, const KEY *key, row_id_t rowid, uchar *rowdata,
                       ulong *col_offsets, ulong *null_byte_offsets, ulong *null_bitmasks);

 private:
  // all the partition sub-tables.
  std::unordered_map<std::string, std::unique_ptr<RapidTable>> m_partitions;

  // part_name+"#"+ part_id
  std::string m_part_key;
};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_TABLE_H__