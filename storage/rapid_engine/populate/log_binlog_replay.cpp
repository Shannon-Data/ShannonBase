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

   Copyright (c) 2026, Shannon Data AI and/or its affiliates.
*/
#include "storage/rapid_engine/populate/log_binlog_replay.h"
#include <openssl/evp.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>
#include "include/scope_guard.h"
#include "sql/binlog.h"
#include "sql/binlog/decompressing_event_object_istream.h"
#include "sql/binlog_reader.h"
#include "sql/field.h"
#include "sql/log_event.h"
#include "sql/mysqld.h"
#include "sql/rpl_utility.h"
#include "sql/sql_class.h"
#include "sql/table.h"
#include "storage/innobase/include/srv0srv.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/trx/transaction.h"
#include "storage/rapid_engine/utils/binlog_position.h"

namespace ShannonBase {
namespace Populate {
namespace {
using namespace mysql::binlog::event;
using Record = change_record_buff_t;
std::string basename(const std::string &name) { return std::filesystem::path(name).filename().string(); }
// The initial recovery lane accepts only an exact schema and FULL row images.
// Unsupported schemas/events take the existing InnoDB reload lane.
bool supported(TABLE *table) {
  if (!table || table->part_info || table->s->primary_key == MAX_KEY || table->s->foreign_keys ||
      table->s->foreign_key_parents)
    return false;
  for (uint i = 0; i < table->s->fields; ++i) {
    const auto *f = table->field[i];
    if (f->is_virtual_gcol() || f->gcol_info || (f->all_flags() & BLOB_FLAG) || f->real_type() == MYSQL_TYPE_JSON ||
        f->real_type() == MYSQL_TYPE_GEOMETRY || f->real_type() == MYSQL_TYPE_VECTOR || f->is_array())
      return false;
  }
  return true;
}

bool unpack_full(TABLE *table, table_def &definition, const MY_BITMAP *columns, const uchar *&cursor, const uchar *end,
                 uchar *destination) {
  if (!definition.is_valid() || definition.size() != table->s->fields || columns->n_bits != definition.size() ||
      !bitmap_is_set_all(columns))
    return false;
  const size_t null_bytes = (definition.size() + 7) / 8;
  if (static_cast<size_t>(end - cursor) < null_bytes) return false;
  const uchar *nulls = cursor;
  cursor += null_bytes;
  std::memcpy(table->record[0], table->s->default_values, table->s->rec_buff_length);
  for (uint i = 0; i < definition.size(); ++i) {
    Field *field = table->field[i];
    if (definition.binlog_type(i) != field->binlog_type()) return false;
    if (nulls[i / 8] & (1u << (i % 8))) {
      if (!field->is_nullable()) return false;
      field->set_null();
      continue;
    }
    field->set_notnull();
    // calc_field_size may inspect an encoded length prefix. Supply padded
    // scratch storage, then check its answer against the actual event bounds.
    std::array<uchar, 16> prefix{};
    std::memcpy(prefix.data(), cursor, std::min<size_t>(end - cursor, prefix.size()));
    const uint32 length = definition.calc_field_size(i, prefix.data());
    if (length == UINT_MAX || length > static_cast<uint64_t>(end - cursor)) return false;
    const uchar *next = field->unpack(field->field_ptr(), cursor, definition.field_metadata(i));
    if (!next || next != cursor + length) return false;
    cursor = next;
  }
  std::memcpy(destination, table->record[0], table->s->rec_buff_length);
  return true;
}
}  // namespace

bool BinlogReplay::replay(THD *thd, TABLE *table, const BinlogPosition &start,
                          const std::function<bool(Record &)> &apply) {
  if (!thd || !supported(table) || !mysql_bin_log.is_open()) return false;
  if (!start.valid() || start.prefix_digest.empty()) return false;
  // Recovery holds source MDL_SHARED_NO_WRITE through replay and attachment.
  // The target table therefore cannot gain rows between these physical cuts;
  // unrelated commits may advance the log. A live-consumer watermark can be
  // absent at startup or lag behind the checkpoint, so it is not a safe cut here.
  LOG_INFO current, info;
  if (mysql_bin_log.get_current_log(&current) || current.encrypted_header_size) return false;
  const std::string stop_file = basename(current.log_file_name);
  const uint64_t stop_offset = current.pos;
  // Open the bounded input under LOCK_index, then release it before hashing,
  // decoding or applying rows. Keeping the file handles open also preserves
  // their identity across RESET (a purge pin alone does not protect RESET).
  std::vector<std::unique_ptr<Binlog_file_reader>> readers;
  std::ifstream prefix;
  {
    mysql_mutex_lock(mysql_bin_log.get_index_lock());
    auto unlock_index = create_scope_guard([&] { mysql_mutex_unlock(mysql_bin_log.get_index_lock()); });
    if (mysql_bin_log.find_log_pos(&info, nullptr, false)) return false;
    while (basename(info.log_file_name) != basename(start.file))
      if (mysql_bin_log.find_next_log(&info, false)) return false;
    prefix.open(info.log_file_name, std::ios::binary);
    if (!prefix) return false;
    uint64_t offset = start.offset;
    for (;;) {
      auto reader = std::make_unique<Binlog_file_reader>(true, 64 * 1024 * 1024);
      if (reader->open(info.log_file_name, offset)) return false;
      readers.push_back(std::move(reader));
      if (basename(info.log_file_name) == stop_file) break;
      if (mysql_bin_log.find_next_log(&info, false)) return false;
      offset = 4;
    }
  }
  auto verified = start;
  if (!Utils::BinlogPositionReader::certify_stream(prefix, verified) || verified.prefix_digest != start.prefix_digest)
    return false;

  std::unordered_map<uint64_t, std::shared_ptr<Table_map_log_event>> maps;
  std::vector<Record> changes;
  size_t buffered = 0;
  const size_t limit = std::min<size_t>(ShannonBase::shannon_rpd_engine_cfg.pop_buff_sz_max, 64 * 1024 * 1024);
  auto commit = [&]() {
    const uint64_t scn = changes.empty() ? 0 : TransactionCoordinator::instance().allocate_scn();
    for (auto &record : changes) {
      record.m_commit_scn = scn;
      if (!apply(record)) return false;
    }
    changes.clear();
    maps.clear();
    buffered = 0;
    return true;
  };
  for (size_t file_index = 0; file_index < readers.size(); ++file_index) {
    auto &reader = *readers[file_index];
    const bool last_file = file_index + 1 == readers.size();
    binlog::Decompressing_event_object_istream stream(reader);
    stream.set_verify_checksum(true);
    auto grow = stream.get_grow_calculator();
    grow.set_max_size(limit);
    stream.set_grow_calculator(grow);
    auto check_stream = create_scope_guard([&] { (void)stream.has_error(); });
    bool in_payload = false;
    while (!last_file || static_cast<uint64_t>(reader.position()) < stop_offset || in_payload) {
      if (thd->killed) return false;
      std::shared_ptr<Log_event> event;
      stream >> event;
      if (!event) {
        if (stream.has_error() || last_file || in_payload) return false;
        break;
      }
      if (last_file && static_cast<uint64_t>(reader.position()) > stop_offset) return false;
      const auto type = event->get_type_code();
      switch (type) {
        case TRANSACTION_PAYLOAD_EVENT:
          in_payload = true;
          break;
        case TABLE_MAP_EVENT: {
          auto *map = static_cast<Table_map_log_event *>(event.get());
          const uint64_t id = map->get_table_id();
          // Keep only the source table's metadata; all other rows are skipped.
          if (std::strcmp(map->get_db_name(), table->s->db.str) == 0 &&
              std::strcmp(map->get_table_name(), table->s->table_name.str) == 0) {
            maps[id] = std::static_pointer_cast<Table_map_log_event>(event);
          } else
            maps.erase(id);
          break;
        }
        case WRITE_ROWS_EVENT:
        case UPDATE_ROWS_EVENT:
        case DELETE_ROWS_EVENT: {
          auto *rows = static_cast<Rows_log_event *>(event.get());
          auto found = maps.find(static_cast<uint64_t>(rows->get_table_id()));
          if (found == maps.end()) break;
          auto &map = *found->second;
          table_def definition(map.m_coltype, map.m_colcnt, map.m_field_metadata, map.m_field_metadata_size,
                               map.m_null_bits, map.m_flags, {});
          const uchar *cursor = rows->get_rows_begin(), *end = rows->get_rows_end();
          while (cursor < end) {
            const size_t bytes = 2 * static_cast<size_t>(table->s->rec_buff_length);
            if (bytes > limit || buffered > limit - bytes) return false;
            Record record(Source::COPY_INFO, table->s->rec_buff_length);
            record.m_copy_info_mode = CopyInfoMode::COMMITTED_BINLOG;
            record.m_oper = type == WRITE_ROWS_EVENT
                                ? Record::OperType::INSERT
                                : (type == DELETE_ROWS_EVENT ? Record::OperType::DELETE : Record::OperType::UPDATE);
            if (!unpack_full(table, definition, rows->get_cols(), cursor, end, record.m_buff0.get())) return false;
            if (type == UPDATE_ROWS_EVENT &&
                !unpack_full(table, definition, rows->get_cols_ai(), cursor, end, record.m_buff1.get()))
              return false;
#ifndef NDEBUG
            record.m_schema_name = table->s->db.str;
            record.m_table_name = table->s->table_name.str;
#endif
            changes.push_back(std::move(record));
            buffered += bytes;
          }
          break;
        }
        case XID_EVENT:
          in_payload = false;
          if (!commit()) return false;
          break;
        case QUERY_EVENT: {
          auto *query = static_cast<Query_log_event *>(event.get());
          std::string text(query->query, query->q_len);
          if (text == "BEGIN") {
            if (!changes.empty()) return false;
            maps.clear();
          } else if (text == "COMMIT") {
            in_payload = false;
            if (!commit()) return false;
          } else if (text == "ROLLBACK") {
            in_payload = false;
            changes.clear();
            maps.clear();
            buffered = 0;
          }
          // DDL and statement-logged DML are deliberately not guessed from SQL.
          else
            return false;
          break;
        }
        case GTID_LOG_EVENT:
        case ANONYMOUS_GTID_LOG_EVENT:
          if (!changes.empty()) return false;
          maps.clear();
          break;
        case FORMAT_DESCRIPTION_EVENT:
        case ROTATE_EVENT:
        case STOP_EVENT:
        case PREVIOUS_GTIDS_LOG_EVENT:
        case HEARTBEAT_LOG_EVENT:
        case ROWS_QUERY_LOG_EVENT:
          break;
        // Partial images/JSON diffs, XA and unknown events
        // are safely rebuilt from InnoDB rather than partially replayed.
        default:
          return false;
      }
    }
    if (last_file) break;
    maps.clear();
  }
  // Relevant rows without a complete transaction outcome never publish.
  return changes.empty();
}
}  // namespace Populate
}  // namespace ShannonBase
