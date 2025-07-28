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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/imcs/imcs.h"

#include <threads.h>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>

#include "include/decimal.h"
#include "include/my_dbug.h"                     //DBUG_EXECUTE_IF
#include "include/row0pread-adapter.h"           //Parallel Reader
#include "sql/partitioning/partition_handler.h"  //partition handler

#include "storage/innobase/handler/ha_innopart.h"
#include "storage/innobase/include/data0type.h"
#include "storage/innobase/include/mach0data.h"
#include "storage/innobase/include/univ.i"    //UNIV_SQL_NULL
#include "storage/innobase/include/ut0dbg.h"  //ut_ad
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_status.h"
#include "storage/rapid_engine/populate/populate.h"
#include "storage/rapid_engine/utils/utils.h"  //Utils

namespace ShannonBase {
extern ulonglong rpd_para_load_threshold;
namespace Imcs {
Imcs *Imcs::m_instance{nullptr};
std::unique_ptr<boost::asio::thread_pool> Imcs::m_imcs_pool{nullptr};
std::once_flag Imcs::one;

SHANNON_THREAD_LOCAL Imcs *current_imcs_instance = Imcs::instance();

int Imcs::initialize() {
  if (!m_inited.load()) {
    m_inited.store(1);
    Imcs::m_imcs_pool = std::make_unique<boost::asio::thread_pool>(std::thread::hardware_concurrency());
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::deinitialize() {
  if (m_inited.load()) {
    Imcs::m_imcs_pool->stop();
    Imcs::m_imcs_pool->join();
    Imcs::m_imcs_pool.reset();

    m_tables.clear();  // unique_ptr will handle deletion
    m_parttables.clear();

    m_inited.store(0);
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::create_index_memo(const Rapid_load_context *context, RapidTable *rapid) {
  auto source = context->m_table;
  ut_a(source);
  // no.1: primary key. using row_id as the primary key when missing user-defined pk.
  if (source->s->is_missing_primary_key()) rapid->build_hidden_index_memo(context);

  // no.2: user-defined indexes.
  rapid->build_user_defined_index_memo(context);
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::create_table_memo(const Rapid_load_context *context, const TABLE *source) {
  ut_a(source);
  auto ret{ShannonBase::SHANNON_SUCCESS};
  std::unique_ptr<RapidTable> table{nullptr};
  if (context->m_extra_info.m_partition_infos.size())
    table = std::make_unique<PartTable>(source->s->db.str, source->s->table_name.str);
  else
    table = std::make_unique<Table>(source->s->db.str, source->s->table_name.str);
  // step 1: build the Cus meta info for every column.
  for (auto index = 0u; index < source->s->fields; index++) {
    auto fld = *(source->field + index);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;
    if ((ret = table.get()->build_field_memo(context, fld))) return ret;
  }

  // step 2: build indexes.
  ret = create_index_memo(context, table.get());

  // Adding the Table meta obj into m_tables/loaded tables meta information.
  std::string keypart;
  keypart.append(source->s->db.str).append(":").append(source->s->table_name.str);
  if (context->m_extra_info.m_partition_infos.size())
    m_parttables.emplace(keypart, std::move(table));
  else
    m_tables.emplace(keypart, std::move(table));

  return ret;
  /* in secondary load phase, the table not loaded into imcs. therefore, it can be seen
     by any transactions. If this table has been loaded into imcs. A new data such as
     insert/update/delete will associated with a SMU items to trace its visibility. Therefore
     the following DB_TRX_ID cu no need to build.
  key.clear();
  std::unique_ptr<Mock_field_trxid> trx_fld = std::make_unique<Mock_field_trxid>();
  trx_fld.get()->table = const_cast<TABLE *>(source);
  key.append(keypart).append(SHANNON_DB_TRX_ID);
  m_cus.emplace(key, std::make_unique<Cu>(trx_fld.get()));
  */
}

int Imcs::build_indexes_from_keys(const Rapid_load_context *context, std::map<std::string, key_info_t> &keys,
                                  row_id_t rowid) {
  std::string sch_tb(context->m_schema_name);
  sch_tb.append(":").append(context->m_table_name);

  for (auto &key : keys) {
    auto key_name = key.first;
    auto key_len = key.second.first;
    auto key_buff = key.second.second.get();
    m_tables[sch_tb].get()->m_indexes[key_name].get()->insert(key_buff, key_len, &rowid, sizeof(row_id_t));
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::build_indexes_from_log(const Rapid_load_context *context, std::map<std::string, mysql_field_t> &field_values,
                                 row_id_t rowid) {
  auto sch_tb = std::string(context->m_schema_name);
  sch_tb.append(":").append(context->m_table_name);
  auto rpd_table = m_tables[sch_tb].get();

  auto matched_keys = m_tables[sch_tb].get()->m_source_keys;
  ut_a(matched_keys.size() > 0);

  std::unique_ptr<uchar[]> key_buff{nullptr};

  for (auto &key : matched_keys) {
    auto key_name = key.first;
    auto key_info = key.second;
    key_buff.reset(new uchar[key_info.first]);
    memset(key_buff.get(), 0x0, key_info.first);
    uint key_offset{0u};
    for (auto &keykey : key_info.second) {
      ut_a(field_values.find(keykey) != field_values.end());
      if (field_values[keykey].has_nullbit) {
        *key_buff.get() = (field_values[keykey].is_null) ? 1 : 0;
        key_offset++;
      }

      auto cs = rpd_table->get_field(keykey) ? rpd_table->get_field(keykey)->header()->m_charset : nullptr;
      if (field_values[keykey].mtype == DATA_BLOB || field_values[keykey].mtype == DATA_VARCHAR ||
          field_values[keykey].mtype == DATA_VARMYSQL) {
        int2store(key_buff.get() + key_offset, field_values[keykey].plength);
        key_offset += HA_KEY_BLOB_LENGTH;
        std::memcpy(key_buff.get() + key_offset, field_values[keykey].data.get(), field_values[keykey].mlength);
        key_offset += field_values[keykey].mlength;
      } else {
        ut_a(field_values[keykey].mlength = field_values[keykey].plength);
        if (field_values[keykey].mtype == DATA_DOUBLE || field_values[keykey].mtype == DATA_FLOAT ||
            field_values[keykey].mtype == DATA_DECIMAL) {
          ut_a(field_values[keykey].mlength == 8);
          uchar encoding[8] = {0};
          auto val = *(double *)field_values[keykey].data.get();
          Utils::Encoder<double>::EncodeFloat(val, encoding);
          std::memcpy(key_buff.get() + key_offset, encoding, field_values[keykey].mlength);
          key_offset += field_values[keykey].mlength;
        } else {
          std::memcpy(key_buff.get() + key_offset, field_values[keykey].data.get(), field_values[keykey].mlength);
          key_offset += field_values[keykey].mlength;
          if (key_offset < key_info.first && cs)
            cs->cset->fill(cs, (char *)key_buff.get() + key_offset, key_info.first - key_offset, ' ');
        }
      }
    }

    ut_a(rpd_table->m_indexes.find(key_name) != rpd_table->m_indexes.end());
    rpd_table->m_indexes[key_name].get()->insert(key_buff.get(), key_info.first, &rowid, sizeof(row_id_t));
  }

  return ShannonBase::SHANNON_SUCCESS;
}

void Imcs::cleanup(std::string &sch_name, std::string &table_name) {
  std::string key(sch_name);
  key.append(":").append(table_name);
  if (!m_tables.size() || m_tables.find(key) == m_tables.end()) return;
  m_tables.erase(key);
}

int Imcs::load_innodb(const Rapid_load_context *context, ha_innobase *file) {
  auto m_thd = context->m_thd;
  // should be RC isolation level. set_tx_isolation(m_thd, ISO_READ_COMMITTED, true);
  if (file->inited == handler::NONE && file->ha_rnd_init(true)) {
    file->ha_rnd_end();
    my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name.c_str(), context->m_table_name.c_str());
    return HA_ERR_GENERIC;
  }

  int tmp{HA_ERR_GENERIC};
  m_thd->set_sent_row_count(0);
  std::string key_part;
  key_part.append(context->m_schema_name.c_str()).append(":").append(context->m_table_name.c_str());
  ut_a(m_tables.find(key_part) != m_tables.end());

  while ((tmp = file->ha_rnd_next(context->m_table->record[0])) != HA_ERR_END_OF_FILE) {
    /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
     without locks. Now, do full scan, but multi-thread scan will impl in future. */
    if (tmp == HA_ERR_KEY_NOT_FOUND) break;

    DBUG_EXECUTE_IF("secondary_engine_rapid_load_table_error", {
      my_error(ER_SECONDARY_ENGINE, MYF(0), context->m_schema_name.c_str(), context->m_table_name.c_str());
      file->ha_rnd_end();
      return HA_ERR_GENERIC;
    });

    // ref to `row_sel_store_row_id_to_prebuilt` in row0sel.cc
    if (m_tables[key_part].get()->write(context, context->m_table->record[0])) {
      std::string errmsg;
      errmsg.append("load data from ")
          .append(context->m_schema_name.c_str())
          .append(".")
          .append(context->m_table_name.c_str())
          .append(" to imcs failed.");
      my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
      break;
    }

    m_thd->inc_sent_row_count(1);

    if (tmp == HA_ERR_RECORD_DELETED && !m_thd->killed) continue;
  }
  // end of load the data from innodb to imcs.
  file->ha_rnd_end();
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::load_innodb_parallel(const Rapid_load_context *context, ha_innobase *file) {
  auto m_thd = context->m_thd;
  // should be RC isolation level. set_tx_isolation(m_thd, ISO_READ_COMMITTED, true);
  size_t num_threads;
  auto max_threads = thd_parallel_read_threads(m_thd);
  int tmp{HA_ERR_GENERIC};

  m_thd->set_sent_row_count(0);
  RapidTable *source_table{nullptr};
  std::string key_part;
  {
    std::shared_lock lk(this->m_table_mutex);
    key_part.append(context->m_schema_name.c_str()).append(":").append(context->m_table_name.c_str());
    if (m_tables.find(key_part) == m_tables.end()) {
      my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name.c_str(), context->m_table_name.c_str());
      return HA_ERR_GENERIC;
    }
    source_table = m_tables[key_part].get();
  }

  struct ScanCtxGuard {
    ha_innobase *file;
    void *ctx{nullptr};
    std::vector<void *> thread_ctxs;
    ScanCtxGuard(ha_innobase *f) : file(f) {}
    ~ScanCtxGuard() {
      if (ctx) file->parallel_scan_end(ctx);
    }
  } scan_ctx_guard(file);
  parall_scan_cookie_t scan_cookie;

  if (file->inited == handler::NONE && file->parallel_scan_init(scan_ctx_guard.ctx, &num_threads, true, max_threads)) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name.c_str(), context->m_table_name.c_str());
    return HA_ERR_GENERIC;
  }

  // to set the thread contexts. now set to nullptr,  you can use your own ctx. or resize(num_threads,
  // (void*)&scan_cookie);
  scan_ctx_guard.thread_ctxs.resize(num_threads, nullptr);
  scan_cookie.completion_latch = std::make_unique<Utils::latch>(num_threads);

  Parallel_reader_adapter::Init_fn init_fn = [&scan_cookie](void *cookie, ulong ncols, ulong row_len,
                                                            const ulong *col_offsets, const ulong *null_byte_offsets,
                                                            const ulong *null_bitmasks) -> bool {
    scan_cookie.scan_done = false;
    scan_cookie.n_cols = ncols;
    scan_cookie.row_len = row_len;
    scan_cookie.col_offsets = const_cast<ulong *>(col_offsets);
    scan_cookie.null_byte_offsets = const_cast<ulong *>(null_byte_offsets);
    scan_cookie.null_bitmasks = const_cast<ulong *>(null_bitmasks);
    return false;
  };

  Parallel_reader_adapter::Load_fn load_fn = [&key_part, &scan_cookie, &context, &file, &source_table](
                                                 void *cookie, uint nrows, void *rowdata,
                                                 uint64_t partition_id) -> bool {
    // ref to `row_sel_store_row_id_to_prebuilt` in row0sel.cc
    // auto scan_cookie = (parall_scan_cookie_t*) cookie, if you enable thread contexs.
    auto data_ptr = static_cast<uchar *>(rowdata);
    auto end_data_ptr = static_cast<uchar *>(rowdata) + ptrdiff_t(nrows * scan_cookie.row_len);
    for (auto index = 0u; index < nrows; data_ptr += ptrdiff_t(scan_cookie.row_len), index++) {
      if (source_table->write(context, (uchar *)data_ptr, scan_cookie.row_len, scan_cookie.col_offsets,
                              scan_cookie.n_cols, scan_cookie.null_byte_offsets, scan_cookie.null_bitmasks)) {
        scan_cookie.error_flag.store(true);
        std::string errmsg;
        errmsg.append("load data from ")
            .append(context->m_schema_name.c_str())
            .append(".")
            .append(context->m_table_name.c_str())
            .append(" to imcs failed.");
        my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
        return true;
      }
    }

    ut_a(data_ptr == end_data_ptr);
    scan_cookie.n_rows.fetch_add(nrows);
    return false;
  };

  Parallel_reader_adapter::End_fn end_fn = [&scan_cookie](void *cookie) {
    scan_cookie.scan_done.store(true);
    scan_cookie.completion_latch->count_down();
  };

  tmp = file->parallel_scan(scan_ctx_guard.ctx, scan_ctx_guard.thread_ctxs.data(), init_fn, load_fn, end_fn);
  // Wait for scan to complete or error
  scan_cookie.completion_latch->wait();

  /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
    without locks. Now, do full scan, but multi-thread scan will impl in future. */
  // if (tmp == HA_ERR_KEY_NOT_FOUND) return HA_ERR_KEY_NOT_FOUND;
  if (tmp || scan_cookie.error_flag.load()) {
    DBUG_EXECUTE_IF("secondary_engine_rapid_load_table_error", {
      my_error(ER_SECONDARY_ENGINE, MYF(0), context->m_schema_name.c_str(), context->m_table_name.c_str());
      return tmp ? tmp : HA_ERR_GENERIC;
    });
  }

  context->m_thd->inc_sent_row_count(scan_cookie.n_rows);
  // end of load the data from innodb to imcs.
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::load_innodbpart(const Rapid_load_context *context, ha_innopart *file) {
  std::string sch_name(context->m_schema_name.c_str()), table_name(context->m_table_name.c_str()), key;
  key.append(sch_name).append(":").append(table_name);
  ut_a(m_parttables.find(key) != m_parttables.end());

  context->m_thd->set_sent_row_count(0);
  for (auto &part : context->m_extra_info.m_partition_infos) {
    auto partkey(part.first);
    partkey.append("#").append(std::to_string(part.second));
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_active_part_key = partkey;
    // should be RC isolation level. set_tx_isolation(m_thd, ISO_READ_COMMITTED, true);
    if (file->inited == handler::NONE && file->rnd_init_in_part(part.second, true)) {
      my_error(ER_NO_SUCH_TABLE, MYF(0), sch_name, table_name);
      return HA_ERR_GENERIC;
    }

    int tmp{HA_ERR_GENERIC};
    while ((tmp = file->rnd_next_in_part(part.second, context->m_table->record[0])) != HA_ERR_END_OF_FILE) {
      /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
       without locks. Now, do full scan, but multi-thread scan will impl in future. */
      if (tmp == HA_ERR_KEY_NOT_FOUND) break;

      DBUG_EXECUTE_IF("secondary_engine_rapid_load_table_error", {
        my_error(ER_SECONDARY_ENGINE, MYF(0), sch_name, table_name);
        file->rnd_end_in_part(part.second, true);
        return HA_ERR_GENERIC;
      });

      // ref to `row_sel_store_row_id_to_prebuilt` in row0sel.cc
      if (m_parttables[key].get()->write(context, context->m_table->record[0])) {
        file->rnd_end_in_part(part.second, true);
        std::string errmsg;
        errmsg.append("load data from ").append(sch_name).append(".").append(table_name).append(" to imcs failed.");
        my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
      }

      context->m_thd->inc_sent_row_count(1);
      if (tmp == HA_ERR_RECORD_DELETED && !context->m_thd->killed) continue;
    }
    // end of load the data from innodb to imcs.
    file->rnd_end_in_part(part.second, true);
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::load_table(const Rapid_load_context *context, const TABLE *source) {
  if (create_table_memo(context, source)) {
    std::string sch(source->s->db.str), table(source->s->table_name.str), errmsg;
    cleanup(sch, table);

    errmsg.append("create table memo for ")
        .append(context->m_schema_name)
        .append(".")
        .append(context->m_table_name)
        .append(" failed.");
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return HA_ERR_GENERIC;
  }

  // if the rec count is more than threshold and has primary key, it can be use parallel load, otherwise not.
  auto parall_scan = (dynamic_cast<ha_innobase *>(source->file)->stats.records > ShannonBase::rpd_para_load_threshold &&
                      !context->m_table->s->is_missing_primary_key())
                         ? true
                         : false;
  return !parall_scan ? load_innodb(context, dynamic_cast<ha_innobase *>(source->file))
                      : load_innodb_parallel(context, dynamic_cast<ha_innobase *>(source->file));
}

int Imcs::load_parttable(const Rapid_load_context *context, const TABLE *source) {
  if (create_table_memo(context, source)) {
    std::string sch(source->s->db.str), table(source->s->table_name.str), errmsg;
    cleanup(sch, table);
    errmsg.append("create table memo for ")
        .append(context->m_schema_name)
        .append(".")
        .append(context->m_table_name)
        .append(" failed.");
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return HA_ERR_GENERIC;
  }

  auto ret{ShannonBase::SHANNON_SUCCESS};
  if ((ret = load_innodbpart(context, dynamic_cast<ha_innopart *>(source->file)))) {
    // if load partition table failed, then do normal load mode, therefore clear partition info.
    const_cast<Rapid_load_context *>(context)->m_extra_info.m_partition_infos.clear();
    ret = load_innodb(context, dynamic_cast<ha_innobase *>(source->file));
  }
  return ret;
}

int Imcs::unload_innodb(const Rapid_load_context *context, const char *db_name, const char *table_name,
                        bool error_if_not_loaded) {
  std::string key(db_name);
  key.append(":").append(table_name);
  if (m_tables.find(key) == m_tables.end() && error_if_not_loaded) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name, context->m_table_name);
    return HA_ERR_GENERIC;
  }

  std::unique_lock lock(m_table_mutex);
  m_tables.erase(key);

  shannon_loaded_tables->erase(db_name, table_name);
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::unload_innodbpart(const Rapid_load_context *context, const char *db_name, const char *table_name,
                            bool error_if_not_loaded) {
  std::string key(db_name);
  key.append(":").append(table_name);
  if (m_parttables.find(key) == m_parttables.end() && error_if_not_loaded) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name, context->m_table_name);
    return HA_ERR_GENERIC;
  }

  shannon_loaded_tables->erase(db_name, table_name);
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::unload_table(const Rapid_load_context *context, const char *db_name, const char *table_name,
                       bool error_if_not_loaded) {
  /** the key format: "db_name:table_name:field_name", all the ghost columns also should be
   *  removed*/
  int ret{ShannonBase::SHANNON_SUCCESS};
  auto partition_hanlder = context->m_table ? context->m_table->file->get_partition_handler() : nullptr;
  auto partition_names = context->m_thd->lex->query_block->get_table_list()
                             ? context->m_thd->lex->query_block->get_table_list()->partition_names
                             : nullptr;
  if (partition_names && partition_hanlder) {
    ret = unload_innodbpart(context, db_name, table_name, error_if_not_loaded);
  } else
    ret = unload_innodb(context, db_name, table_name, error_if_not_loaded);
  return ret;
}

int Imcs::insert_row(const Rapid_load_context *context, row_id_t rowid, uchar *buf) {
  ut_a(context && buf);

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::write_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                             std::unordered_map<std::string, mysql_field_t> &fields) {
  std::string sch_tb;
  sch_tb.append(context->m_schema_name).append(":").append(context->m_table_name);
  auto rpd_tb = m_tables[sch_tb].get();

  for (auto &field_val : fields) {
    auto key_name = field_val.first;
    // escape the db_trx_id field and the filed is set to NOT_SECONDARY[not loaded int imcs]
    if (key_name == SHANNON_DB_TRX_ID || rpd_tb->get_field(key_name) == nullptr) continue;
    // if data is nullptr, means it's 'NULL'.
    auto len = field_val.second.mlength;
    if (!rpd_tb->get_field(key_name)->write_row_from_log(context, rowid, field_val.second.data.get(), len))
      return HA_ERR_WRONG_IN_RECORD;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::delete_row(const Rapid_load_context *context, row_id_t rowid) {
  ut_a(context);
  auto sch_tb(context->m_schema_name);
  sch_tb.append(":").append(context->m_table_name);
  auto rpd_tb = m_tables[sch_tb].get();

  if (!rpd_tb->m_fields.size()) return SHANNON_SUCCESS;

  for (auto it = rpd_tb->m_fields.begin(); it != rpd_tb->m_fields.end();) {
    if (!it->second->delete_row(context, rowid)) {
      return HA_ERR_GENERIC;
    }
    ++it;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &rowids) {
  ut_a(context);
  auto sch_tb(context->m_schema_name);
  sch_tb.append(":").append(context->m_table_name);
  auto rpd_tb = m_tables[sch_tb].get();

  if (!rpd_tb->m_fields.size()) return SHANNON_SUCCESS;

  if (rowids.empty()) {  // delete all rows.
    for (auto &cu : rpd_tb->m_fields) {
      assert(cu.second);
      if (!cu.second->delete_row_all(context)) return HA_ERR_GENERIC;
    }

    return ShannonBase::SHANNON_SUCCESS;
  }

  for (auto &rowid : rowids) {
    for (auto &cu : rpd_tb->m_fields) {
      assert(cu.second);
      if (!cu.second->delete_row(context, rowid)) return HA_ERR_GENERIC;
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::update_row(const Rapid_load_context *context, row_id_t rowid, std::string &field_key,
                     const uchar *new_field_data, size_t nlen) {
  ut_a(context);
  auto sch_tb(context->m_schema_name);
  sch_tb.append(":").append(context->m_table_name);
  auto rpd_tb = m_tables[sch_tb].get();

  ut_a(rpd_tb->m_fields[field_key].get());
  auto ret = rpd_tb->m_fields[field_key]->update_row(context, rowid, const_cast<uchar *>(new_field_data), nlen);
  if (!ret) return HA_ERR_GENERIC;
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::update_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                              std::unordered_map<std::string, mysql_field_t> &upd_recs) {
  ut_a(context);
  auto sch_tb(context->m_schema_name);
  sch_tb.append(":").append(context->m_table_name);
  auto rpd_tb = m_tables[sch_tb].get();

  for (auto &field_val : upd_recs) {
    auto key_name = field_val.first;
    // escape the db_trx_id field and the filed is set to NOT_SECONDARY[not loaded int imcs]
    if (key_name == SHANNON_DB_TRX_ID || rpd_tb->get_field(key_name) == nullptr) continue;
    // if data is nullptr, means it's 'NULL'.
    auto len = field_val.second.mlength;
    if (!rpd_tb->get_field(key_name)->update_row_from_log(context, rowid, field_val.second.data.get(), len))
      return HA_ERR_WRONG_IN_RECORD;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::rollback_changes_by_trxid(Transaction::ID trxid) {
  for (auto &tb : m_tables) {
    for (auto &cu : tb.second.get()->m_fields) {
      auto chunk_sz = cu.second.get()->chunks();
      for (auto index = 0u; index < chunk_sz; index++) {
        auto &version_infos = cu.second.get()->chunk(index)->header()->m_smu->version_info();
        if (!version_infos.size()) continue;

        for (auto &ver : version_infos) {
          std::lock_guard<std::mutex> lock(ver.second.vec_mutex);
          auto rowid = ver.first;

          std::for_each(ver.second.items.begin(), ver.second.items.end(), [&](ReadView::SMU_item &item) {
            if (item.trxid == trxid) {
              // To update rows status.
              if (item.oper_type == OPER_TYPE::OPER_INSERT) {                      //
                if (!cu.second.get()->chunk(index)->header()->m_del_mask.get()) {  // the del mask not exists now.
                  cu.second.get()->chunk(index)->header()->m_del_mask =
                      std::make_unique<ShannonBase::bit_array_t>(SHANNON_ROWS_IN_CHUNK);
                }
                Utils::Util::bit_array_set(cu.second.get()->chunk(index)->header()->m_del_mask.get(), rowid);
              }
              if (item.oper_type == OPER_TYPE::OPER_DELETE) {
                Utils::Util::bit_array_reset(cu.second.get()->chunk(index)->header()->m_del_mask.get(), rowid);
              }
              item.tm_committed = ShannonBase::SHANNON_MAX_STMP;  // reset commit timestamp to max, mean it rollbacked.
                                                                  // has been rollbacked, invisible to all readview.
            }
          });
        }
      }
    }
  }
  return ShannonBase::SHANNON_SUCCESS;
}

}  // namespace Imcs
}  // namespace ShannonBase