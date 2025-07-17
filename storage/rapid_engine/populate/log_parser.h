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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.
*/

#ifndef __SHANNONBASE_LOG_PARSER_H__
#define __SHANNONBASE_LOG_PARSER_H__
#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

#include "storage/innobase/include/buf0buf.h"
#include "storage/innobase/include/log0types.h"
#include "storage/innobase/include/mtr0types.h"
#include "storage/innobase/include/trx0types.h"
#include "storage/innobase/include/univ.i"

#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"

using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::use_awaitable;

// clang-format off
class Field;
class TABLE;
namespace ShannonBase {
namespace Imcs{
class RapidTable;
}
class Rapid_load_context;
namespace Populate {
extern std::unordered_map<uint64, const dict_index_t *> g_index_cache;
extern std::unordered_map<uint64, std::pair<std::string, std::string>> g_index_names;
extern std::shared_mutex g_index_cache_mutex;

// co-routine utils class.
template <typename T>
class shared_promise {
public:
  void set_value(const T &v) {
    std::lock_guard<std::mutex> lck(m_mutex);
    if (!m_ready) {
      m_value = v;
      m_ready = true;
      m_cond.notify_all();
    }
  }

  boost::asio::awaitable<T> get_awaitable(boost::asio::any_io_executor executor) {
    while (true) {
      {
        std::lock_guard<std::mutex> lck(m_mutex);
        if (m_ready) co_return *m_value;
      }

      co_await boost::asio::post(executor, boost::asio::use_awaitable);
    }
  }
private:
  std::mutex m_mutex;
  std::condition_variable m_cond;
  std::optional<T> m_value;
  bool m_ready = false;
};

template <typename T>
T cowait_sync_with(boost::asio::thread_pool &pool, boost::asio::awaitable<T> aw) {
  auto prom = std::make_shared<std::promise<T>>();
  auto fut = prom->get_future();

  boost::asio::co_spawn(
      pool,
      [aw = std::move(aw), prom]() mutable -> boost::asio::awaitable<void> {
        try {
          T result = co_await std::move(aw);
          prom->set_value(std::move(result));
        } catch (...) {
          prom->set_exception(std::current_exception());
        }
        co_return;
      },
      boost::asio::detached);

  return fut.get();  // when the result is ready.
}

/**
 * To parse the redo log file, it used to populate the changes from ionnodb
 * to rapid.
 */
class LogParser {
 public:
  LogParser() = default;
  ~LogParser() = default;

  //store the field infor in mysql format.
  uint parse_redo(Rapid_load_context* context, byte *ptr, byte *end_ptr);
 private:
  ulint parse_log_rec(Rapid_load_context* context, mlog_id_t *type, byte *ptr, byte *end_ptr,
                      space_id_t *space_id, page_no_t *page_no, byte **body);

  byte *parse_or_apply_log_rec_body(Rapid_load_context* context,
                                    mlog_id_t type, byte *ptr,
                                    byte *end_ptr, space_id_t space_id,
                                    page_no_t page_no, buf_block_t *block,
                                    mtr_t *mtr, lsn_t start_lsn);

  int parse_cur_rec_change_apply_low(Rapid_load_context* context,
                                    const rec_t *rec, const dict_index_t *index,
                                    const dict_index_t *real_index,
                                    const ulint *offsets, mlog_id_t type, bool all,
                                    page_zip_des_t *page_zip = nullptr,              /**used for upd*/
                                    const upd_t *upd = nullptr, trx_id_t trxid = 0); /**upd vector for upd*/

  // parses the update log and apply.
  byte *parse_and_apply_upd_rec_in_place(Rapid_load_context* context,
                                        rec_t *rec,                /*!< in/out: record where replaced */
                                        const dict_index_t *index, /*!< in: the index the record belongs to */
                                        const dict_index_t *real_index, /*!< in: the real index the record belongs to */
                                        const ulint *offsets,      /*!< in: array returned by rec_get_offsets() */
                                        const upd_t *update,       /*!< in: update vector */
                                        page_zip_des_t *page_zip,  /*!< in: compressed page with enough space
                                                                            available, or NULL */
                                        trx_id_t trx_id);          /*!< in: new trx id*/

  // parses the insert log and apply insertion to rapid.
  byte *parse_cur_and_apply_insert_rec(Rapid_load_context* context,
                                       bool is_short,       /*!< in: true if short inserts */
                                       const byte *ptr,     /*!< in: buffer */
                                       const byte *end_ptr, /*!< in: buffer end */
                                       buf_block_t *block,  /*!< in: block or NULL */
                                       page_t* page,        /*!< in: page or NULL */
                                       page_zip_des_t* page_zip, /*!< in: page_zip or NULL */
                                       dict_index_t *index, /*!< in: record descriptor */
                                       mtr_t *mtr);         /*!< in: mtr or NULL */

  byte *parse_cur_and_apply_delete_rec(Rapid_load_context* context,
                                       byte *ptr,           /*!< in: buffer */
                                       byte *end_ptr,       /*!< in: buffer end */
                                       buf_block_t *block,  /*!< in: page or NULL */
                                       dict_index_t *index, /*!< in: record descriptor */
                                       mtr_t *mtr);         /*!< in: mtr or NULL */

  byte *parse_cur_and_apply_delete_mark_rec(Rapid_load_context* context,
                                            byte *ptr,           /*!< in: buffer */
                                            byte *end_ptr,       /*!< in: buffer end */
                                            buf_block_t *block,  /*!< in: page or NULL */
                                            dict_index_t *index, /*!< in: record descriptor */
                                            mtr_t *mtr);         /*!< in: mtr or NULL */

  byte *parse_cur_update_in_place_and_apply(Rapid_load_context* context,
                                            byte *ptr,                /*!< in: buffer */
                                            byte *end_ptr,            /*!< in: buffer end */
                                            buf_block_t *block,       /*!< in: block or NULL */
                                            page_t* page,             /*!< in: page or NULL */
                                            page_zip_des_t *page_zip, /*!< in/out: compressed page, or NULL */
                                            dict_index_t *index);     /*!< in: index corresponding to page */

  /** Parses a log record of copying a record list end to a new created page.
  @return end of log record or NULL */
  byte *parse_copy_rec_list_to_created_page(Rapid_load_context* context,
                                            byte *ptr,           /*!< in: buffer */
                                            byte *end_ptr,       /*!< in: buffer end */
                                            buf_block_t *block,  /*!< in: block or NULL */
                                            page_t* page,        /*!< in: page or NULL */
                                            page_zip_des_t* page_zip, /*!< in: page or NULL */
                                            dict_index_t *index, /*!< in: record descriptor */
                                            mtr_t *mtr);         /*!< in: mtr or NULL */

  /** Parses a redo log record of reorganizing a page.
  @return end of log record or NULL */
  byte *parse_btr_page_reorganize(byte *ptr,           /*!< in: buffer */
                                  byte *end_ptr,       /*!< in: buffer end */
                                  dict_index_t *index, /*!< in: record descriptor */
                                  bool compressed,     /*!< in: true if compressed page */
                                  buf_block_t *block,  /*!< in: page to be reorganized, or NULL */
                                  mtr_t *mtr);         /*!< in: mtr or NULL */

  /** Parses the redo log record for delete marking or unmarking of a secondary
  index record.
  @return end of log record or NULL */
  byte *parse_btr_cur_del_mark_set_sec_rec(byte *ptr,     /*!< in: buffer */
                                           byte *end_ptr, /*!< in: buffer end */
                                           page_t *page,  /*!< in/out: page or NULL */
                                           page_zip_des_t *page_zip);

  byte *parse_trx_undo_add_undo_rec(byte *ptr,   /*!< in: buffer */
                                    byte *end_ptr, /*!< in: buffer end */
                                    page_t *page);  /*!< in: page or NULL */

  byte *parse_page_header(mlog_id_t type, const byte *ptr, const byte *end_ptr,
                          page_t *page, mtr_t *mtr);

  // get the field value from innodb format to mysql format. return 0 success.
  int store_field_in_mysql_format(const dict_index_t *index, const dict_col_t* col,
                                  const byte *dest, const byte *src, ulint mlen, ulint len);

  // only user's index be retrieved from dd_table.
  inline const dict_index_t *find_index(uint64 idx_id, std::string& db_name, std::string& table_name) {
    std::shared_lock slock(ShannonBase::Populate::g_index_cache_mutex);
    if (g_index_cache.find(idx_id) == g_index_cache.end()) {
      assert(false);
    } else {
      db_name = g_index_names[idx_id].first;
      table_name = g_index_names[idx_id].second;
      slock.unlock();
      assert(g_index_cache[idx_id]);
      return g_index_cache[idx_id];
    }

    assert(false);
    return nullptr;
  }

  // get the trxid in byte fmt and returns the length of PK found.
  inline uint get_trxid(const rec_t *rec, const dict_index_t *index,
                        const ulint *offsets, uchar *trx_id);

  // parse the signle log rec.
  ulint parse_single_rec(Rapid_load_context* context, byte *ptr, byte *end_ptr);

  ////parse the multi log rec. retunr parsed byte size.
  ulint parse_multi_rec(Rapid_load_context* context, byte *ptr, byte *end_ptr);

  // gets blocks by page no and space id
  inline buf_block_t *get_block(space_id_t, page_no_t);

  //is a valid data record.
  inline bool is_data_rec(rec_t* rec);

  inline bool check_key_field(std::unordered_map<std::string, ShannonBase::key_meta_t>& keys,
                              const char* field_name);

  // build the keys from field values.
  // @param field_values[in] , the map of field values. <field_name, field data>
  // @param key_values[in/out], the map of key data, <key_name, key data>.
  // @return 0 success, otherwise failed.
  int build_key(const Rapid_load_context *context, std::unordered_map<std::string, mysql_field_t> &field_values,
                 std::map<std::string, key_info_t>& keys);

  byte *advance_parseMetadataLog(table_id_t id, uint64_t version, byte *ptr,
                                 byte *end);

  [[nodiscard]] byte *parse_tablespace_redo_rename(byte *ptr, const byte *end,
                                                   const page_id_t &page_id,
                                                   ulint parsed_bytes,
                                                   bool parse_only);
  [[nodiscard]] byte *parse_tablespace_redo_delete(byte *ptr, const byte *end,
                                                   const page_id_t &page_id,
                                                   ulint parsed_bytes,
                                                   bool parse_only);
  [[nodiscard]] byte *parse_tablespace_redo_create(byte *ptr, const byte *end,
                                                   const page_id_t &page_id,
                                                   ulint parsed_bytes,
                                                   bool parse_only);
  [[nodiscard]] byte *parse_tablespace_redo_extend(byte *ptr, const byte *end,
                                                   const page_id_t &page_id,
                                                   ulint parsed_bytes,
                                                   bool parse_only);
  /** parse a rec and get all feidls data in mysql format and save these values
   * into a vector. all field values store in feild_values' and store key info
   * into context. [field_name, field_value] and [key_name, key_value]*/
  int parse_rec_fields(Rapid_load_context* context,
                       const rec_t *rec, const dict_index_t *index,
                       const ulint *offsets, const dict_index_t *real_index,
                       std::unordered_map<std::string, mysql_field_t>& field_values);

  /**to find a row in imcs via PK. a row divids into fields, and store int a map.
   * return position of first matched row.
   * key_name [in] main search column name in 'db:table_name:field_name' format.
   * field_values_to_find [in], the all fields values of a row to find
   * with_sys_col[in], sys col to do comparision or not. */
  row_id_t find_matched_row(Rapid_load_context* context, std::map<std::string, key_info_t>& keys);

  boost::asio::awaitable<int> parse_rec_fields_async(Rapid_load_context *context, const rec_t *rec,
                                                    const dict_index_t *index, const ulint *offsets,
                                                    const dict_index_t *real_index,
                                                    std::unordered_map<std::string, mysql_field_t> &field_values);

  boost::asio::awaitable<void> co_parse_field(Rapid_load_context *context, const rec_t *rec, const dict_index_t *index,
                                              const ulint *offsets, const dict_index_t *real_index,
                                              std::mutex &field_mutex, size_t idx,
                                              std::unordered_map<std::string, mysql_field_t> &field_values);

  bool rec_field_parse(Rapid_load_context *context, mem_heap_t *heap, Imcs::RapidTable *rpd_table, const rec_t *rec,
                        const dict_index_t *index, const ulint *offsets,
                        const dict_index_t *real_index, const ulint *real_offsets,
                        size_t idx, std::mutex &field_mutex,
                        std::unordered_map<std::string, mysql_field_t> &field_values);
 private:
  alignas(CACHE_LINE_SIZE) static SHANNON_THREAD_LOCAL uchar mysql_field_data_1[MAX_FIELD_WIDTH];
  static SHANNON_THREAD_LOCAL uchar padding[CACHE_LINE_SIZE - (MAX_FIELD_WIDTH % CACHE_LINE_SIZE)]; // padding
  // to indicate whether to use co-routine if # of fields is over this threshold. 
  static const uint MAX_N_FIELD_PARALLEL = 8;
};

}  // namespace Populate
}  // namespace ShannonBase
#endif  //__SHANNONBASE_LOG_PARSER_H__
// clang-format on