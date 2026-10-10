#include "storage/rapid_engine/recovery/binlog_recovery.h"
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

namespace ShannonBase::Recovery {
namespace {
using namespace mysql::binlog::event;
using Record = Populate::change_record_buff_t;
std::string basename(const std::string &name) { return std::filesystem::path(name).filename().string(); }

// Hash the immutable prefix, including FDE and all bytes up to the exact cut.
// Reused filenames after RESET and truncated/replaced files cannot certify a
// checkpoint merely by carrying the same server UUID and filename.
bool prefix_digest(const char *path, uint64_t offset, std::string &digest) {
  std::ifstream in(path, std::ios::binary);
  char magic[4];
  if (offset < 4 || !in.read(magic, 4) ||
      std::memcmp(magic,
                  "\xfe"
                  "bin",
                  4))
    return false;
  in.seekg(0);
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) return false;
  std::array<char, 64 * 1024> bytes;
  bool first_chunk = true;
  while (offset) {
    const auto count = static_cast<size_t>(std::min<uint64_t>(offset, bytes.size()));
    if (!in.read(bytes.data(), count)) return false;
    // MySQL clears the FDE's IN_USE flag on rotation, clean shutdown and
    // crash recovery. Like its event CRC, identity hashing excludes this bit.
    if (first_chunk && count > BIN_LOG_HEADER_SIZE + FLAGS_OFFSET)
      bytes[BIN_LOG_HEADER_SIZE + FLAGS_OFFSET] &= ~LOG_EVENT_BINLOG_IN_USE_F;
    first_chunk = false;
    if (EVP_DigestUpdate(ctx.get(), bytes.data(), count) != 1) return false;
    offset -= count;
  }
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned length = 0;
  if (EVP_DigestFinal_ex(ctx.get(), hash, &length) != 1) return false;
  static const char hex[] = "0123456789abcdef";
  digest.clear();
  for (unsigned i = 0; i < length; ++i) {
    digest += hex[hash[i] >> 4];
    digest += hex[hash[i] & 15];
  }
  return true;
}

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

BinlogPosition BinlogRecovery::current_position() {
  // Never force additional source flushes merely to make a Rapid checkpoint.
  if (!mysql_bin_log.is_open() || sync_binlog_period != 1 || srv_flush_log_at_trx_commit != 1) return {};
  LOG_INFO current;
  if (mysql_bin_log.get_current_log(&current) || current.encrypted_header_size) return {};
  return {basename(current.log_file_name), static_cast<uint64_t>(current.pos), {}};
}

bool BinlogRecovery::certify(BinlogPosition &position) {
  if (!position.valid() || !mysql_bin_log.is_open()) return false;
  LOG_INFO info;
  if (mysql_bin_log.find_log_pos(&info, position.file.c_str(), true)) {
    // The observer supplies a basename, whereas the index can hold full paths.
    if (mysql_bin_log.find_log_pos(&info, nullptr, true)) return false;
    while (basename(info.log_file_name) != basename(position.file))
      if (mysql_bin_log.find_next_log(&info, true)) return false;
  }
  position.file = basename(info.log_file_name);
  return prefix_digest(info.log_file_name, position.offset, position.prefix_digest);
}

bool BinlogRecovery::replay(THD *thd, TABLE *table, const BinlogPosition &start,
                            const std::function<bool(Record &)> &apply) {
  if (!thd || !supported(table) || !mysql_bin_log.is_open()) return false;
  BinlogPosition verified = start;
  if (!certify(verified) || verified.prefix_digest != start.prefix_digest) return false;
  LOG_INFO current, info;
  if (mysql_bin_log.get_current_log(&current) || current.encrypted_header_size) return false;
  const std::string stop_file = basename(current.log_file_name);
  const uint64_t stop_offset = current.pos;
  // Pin from the oldest indexed file for this bounded replay. This avoids a
  // mutable pin while rotations are traversed; it is released on every exit.
  mysql_mutex_lock(mysql_bin_log.get_index_lock());
  const bool missing = mysql_bin_log.find_log_pos(&info, nullptr, false) != 0;
  if (!missing) mysql_bin_log.register_log_info(&info);
  auto unlock_index = create_scope_guard([&] { mysql_mutex_unlock(mysql_bin_log.get_index_lock()); });
  if (missing) return false;
  auto unpin = create_scope_guard([&] { mysql_bin_log.unregister_log_info(&info); });
  LOG_INFO scan = info;
  while (basename(scan.log_file_name) != basename(start.file))
    if (mysql_bin_log.find_next_log(&scan, false)) return false;

  std::string pinned_digest;
  if (!prefix_digest(scan.log_file_name, start.offset, pinned_digest) || pinned_digest != start.prefix_digest)
    return false;

  std::unordered_map<uint64_t, std::unique_ptr<Table_map_log_event>> maps;
  std::vector<Record> transaction;
  size_t buffered = 0;
  const size_t limit = std::min<size_t>(ShannonBase::shannon_rpd_engine_cfg.pop_buff_sz_max, 64 * 1024 * 1024);
  auto commit = [&]() {
    const uint64_t scn = TransactionCoordinator::instance().allocate_scn();
    for (auto &record : transaction) {
      record.m_commit_scn = scn;
      if (!apply(record)) return false;
    }
    transaction.clear();
    maps.clear();
    buffered = 0;
    return true;
  };
  uint64_t offset = start.offset;
  for (;;) {
    Binlog_file_reader reader(true, 64 * 1024 * 1024);
    if (reader.open(scan.log_file_name, offset)) return false;
    const bool last_file = basename(scan.log_file_name) == stop_file;
    while (!last_file || static_cast<uint64_t>(reader.position()) < stop_offset) {
      if (thd->killed) return false;
      std::unique_ptr<Log_event> event(reader.read_event_object());
      if (!event) {
        if (reader.has_fatal_error() || last_file) return false;
        break;
      }
      if (last_file && static_cast<uint64_t>(reader.position()) > stop_offset) return false;
      const auto type = event->get_type_code();
      switch (type) {
        case TABLE_MAP_EVENT: {
          auto *map = static_cast<Table_map_log_event *>(event.get());
          const uint64_t id = map->get_table_id();
          // Keep only the source table's metadata; all other rows are skipped.
          if (std::strcmp(map->get_db_name(), table->s->db.str) == 0 &&
              std::strcmp(map->get_table_name(), table->s->table_name.str) == 0) {
            maps[id].reset(static_cast<Table_map_log_event *>(event.release()));
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
            Record record(Populate::Source::COMMITTED_BINLOG, table->s->rec_buff_length);
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
            transaction.push_back(std::move(record));
            buffered += bytes;
          }
          break;
        }
        case XID_EVENT:
          if (!commit()) return false;
          break;
        case QUERY_EVENT: {
          auto *query = static_cast<Query_log_event *>(event.get());
          std::string text(query->query, query->q_len);
          if (text == "BEGIN") {
            if (!transaction.empty()) return false;
            maps.clear();
          } else if (text == "COMMIT") {
            if (!commit()) return false;
          } else if (text == "ROLLBACK") {
            transaction.clear();
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
          if (!transaction.empty()) return false;
          maps.clear();
          break;
        case FORMAT_DESCRIPTION_EVENT:
        case ROTATE_EVENT:
        case STOP_EVENT:
        case PREVIOUS_GTIDS_LOG_EVENT:
        case HEARTBEAT_LOG_EVENT:
        case ROWS_QUERY_LOG_EVENT:
          break;
        // Partial images/JSON diffs, compressed payloads, XA and unknown events
        // are safely rebuilt from InnoDB rather than partially replayed.
        default:
          return false;
      }
    }
    if (last_file) break;
    if (mysql_bin_log.find_next_log(&scan, false)) return false;
    maps.clear();
    offset = 4;
  }
  // Relevant rows without a complete transaction outcome never publish.
  return transaction.empty();
}
}  // namespace ShannonBase::Recovery
