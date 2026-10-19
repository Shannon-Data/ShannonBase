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

   Committed binary-log change propagation.
*/
#include "storage/rapid_engine/populate/log_binlog_parser.h"
#include "storage/rapid_engine/utils/background_session.h"

#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>
#include "include/scope_guard.h"
#include "sql/binlog.h"
#include "sql/binlog/decompressing_event_object_istream.h"
#include "sql/binlog_reader.h"
#include "sql/debug_sync.h"
#include "sql/field.h"
#include "sql/log_event.h"
#include "sql/rpl_utility.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/table.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/populate/log_populate.h"
#include "storage/rapid_engine/utils/binlog_position.h"
#ifdef SHANNON_POSIX_PLATFORM
#include <sys/stat.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include "sql/log.h"
#include "sql/mysqld.h"
#include "sql/rpl_filter.h"
#include "sql/sql_lex.h"
#include "sql/sql_partition.h"  // get_part_for_delete, get_parts_for_update
#include "sql/xa.h"
#include "storage/innobase/include/os0thread-create.h"
#include "storage/innobase/include/srv0srv.h"
#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/imcs/table0persistence.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/populate/log_dml_notification.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Populate {
namespace {
using namespace mysql::binlog::event;
using Position = BinlogPosition;
constexpr size_t kBinlogReaderBufferSize = 64 * 1024 * 1024;

std::string basename(const char *file) { return std::filesystem::path(file).filename().string(); }
bool covered(const Position &floor, const std::string &file, uint64_t offset) {
  if (file == floor.file) return offset <= floor.offset;
  const auto sequence = [](const std::string &name) {
    uint64_t value = 0;
    const auto dot = name.rfind('.');
    if (dot != std::string::npos) std::from_chars(name.data() + dot + 1, name.data() + name.size(), value);
    return value;
  };
  return sequence(file) < sequence(floor.file);
}

// Field::unpack is the server's same-schema row decoder. Images and off-page
// values are detached before releasing the event's storage.
bool unpack(TABLE *table, table_def &definition, const MY_BITMAP *columns, const uchar *&cursor, const uchar *end,
            uchar *destination, change_record_buff_t::off_page_data_t &offpage) {
  if (!definition.is_valid() || definition.size() != table->s->fields || columns->n_bits != definition.size() ||
      !bitmap_is_set_all(columns))
    return false;

  const size_t null_bytes = (definition.size() + 7) / 8;
  if (static_cast<size_t>(end - cursor) < null_bytes) return false;

  const uchar *nulls = cursor;
  cursor += null_bytes;
  std::memcpy(table->record[0], table->s->default_values, table->s->rec_buff_length);
  for (uint i = 0; i < definition.size(); ++i) {
    auto *field = table->field[i];
    if (definition.binlog_type(i) != field->binlog_type()) return false;
    if (nulls[i / 8] & (1U << (i % 8))) {
      if (!field->is_nullable()) return false;
      field->set_null();
      continue;
    }
    field->set_notnull();

    std::array<uchar, 16> prefix{};
    std::memcpy(prefix.data(), cursor, std::min<size_t>(end - cursor, prefix.size()));
    const uint32 length = definition.calc_field_size(i, prefix.data());
    if (length == UINT_MAX || length > static_cast<uint64_t>(end - cursor)) return false;
    const uchar *next = field->unpack(field->field_ptr(), cursor, definition.field_metadata(i));
    if (!next || next != cursor + length) return false;
    cursor = next;

    // Blob pointers produced by unpack reference event bytes; retain owned data
    // before another row or compressed event reuses that storage.
    if (field->is_flag_set(BLOB_FLAG) || field->real_type() == MYSQL_TYPE_JSON ||
        field->real_type() == MYSQL_TYPE_GEOMETRY || field->real_type() == MYSQL_TYPE_VECTOR) {
      auto *blob = static_cast<Field_blob *>(field);
      const size_t bytes = blob->get_length();
      auto data = std::shared_ptr<uchar[]>(new uchar[bytes ? bytes : 1]);
      if (bytes) std::memcpy(data.get(), blob->get_blob_data(), bytes);
      offpage.emplace(i, std::make_pair(bytes, std::move(data)));
    }
  }

  std::memcpy(destination, table->record[0], table->s->rec_buff_length);
  return true;
}
}  // namespace

bool BinlogParser::supports(const TABLE *table) {
  // FK cascades are executed inside InnoDB and are not independent child row
  // events. Generated/array columns need an image reconstruction contract.
  if (!table || table->s->primary_key == MAX_KEY || table->s->foreign_keys || table->s->foreign_key_parents)
    return false;

  for (uint i = 0; i < table->s->fields; ++i) {
    if (table->field[i]->gcol_info || table->field[i]->is_array()) return false;
  }

  return true;
}

static bool resolve_change_partitions(TABLE *table, ShannonBase::Populate::change_record_buff_t::OperType oper,
                                      const uchar *old_row, const uchar *new_row, std::string &part_key,
                                      std::string &old_part_key) {
  using OperType = ShannonBase::Populate::change_record_buff_t::OperType;

  partition_info *part_info = table->part_info;
  assert(part_info != nullptr);

  uint32 part_id{NOT_A_PARTITION_ID}, old_part_id{NOT_A_PARTITION_ID};
  longlong func_value{0};
  int error{0};

  ShannonBase::Utils::ColumnMapGuard guard(table, ShannonBase::Utils::ColumnMapGuard::TYPE::READ);
  switch (oper) {
    case OperType::INSERT:
      error = part_info->get_partition_id(part_info, &part_id, &func_value);
      old_part_id = part_id;
      break;
    case OperType::DELETE:
      error = get_part_for_delete(old_row, table->record[0], part_info, &old_part_id);
      part_id = old_part_id;
      break;
    case OperType::UPDATE:
      error = get_parts_for_update(old_row, new_row, table->record[0], part_info, &old_part_id, &part_id, &func_value);
      break;
    default:
      error = HA_ERR_GENERIC;
      break;
  }

  if (error != 0) return false;

  part_key = ShannonBase::Utils::rapid_partition_key(part_info, part_id);
  old_part_key = ShannonBase::Utils::rapid_partition_key(part_info, old_part_id);
  return !part_key.empty() && !old_part_key.empty();
}

bool FinalizeBinlogRow(TABLE *table, change_record_buff_t &record) {
#ifndef NDEBUG
  record.m_schema_name = table->s->db.str;
  record.m_table_name = table->s->table_name.str;
#endif
  // The decoder leaves the current/after image in record[0]. MySQL's update
  // partition resolver requires that exact buffer as new_data.
  return !table->part_info || resolve_change_partitions(table, record.m_oper, record.m_buff0.get(), table->record[0],
                                                        record.m_part_key, record.m_old_part_key);
}

bool BinlogParser::read_batch(THD *thd, const std::vector<BinlogTableBinding> &bindings, const Position &start,
                              const Position &committed_end, std::vector<BinlogTransaction> &batch,
                              Position &assembled_end, const char **failure_reason, const std::atomic<bool> *cancel,
                              bool recovery) noexcept {
  if (failure_reason)
    *failure_reason =
        "Change propagation failed: missing, corrupt or unsupported committed binlog input; unload and reload the "
        "table";

  if (!thd || !start.valid() || !committed_end.valid() || !mysql_bin_log.is_open()) return false;

  if (DBUG_EVALUATE_IF("rapid_committed_decode_bad_alloc", true, false)) {
    if (failure_reason) *failure_reason = "binlog reader allocation or decoding failed; unload and reload the table";
    return false;
  }

  Position identity = start;
  std::vector<std::pair<std::string, std::unique_ptr<Binlog_file_reader>>> inputs;
  std::ifstream prefix_stream;
  {
    mysql_mutex_lock(mysql_bin_log.get_index_lock());
    auto unlock = create_scope_guard([&] { mysql_mutex_unlock(mysql_bin_log.get_index_lock()); });
    LOG_INFO scan;
    if (mysql_bin_log.find_log_pos(&scan, nullptr, false)) return false;
    while (basename(scan.log_file_name) != start.file)
      if (mysql_bin_log.find_next_log(&scan, false)) return false;
    if (!start.prefix_digest.empty()) {
      prefix_stream.open(scan.log_file_name, std::ios::binary);
      if (!prefix_stream) return false;
    }
    uint64_t offset = start.offset;
    for (;;) {
      if (cancel && cancel->load()) return false;
      auto reader = std::make_unique<Binlog_file_reader>(true, kBinlogReaderBufferSize);
      if (reader->open(scan.log_file_name, offset)) return false;
      inputs.emplace_back(basename(scan.log_file_name), std::move(reader));
      if (inputs.back().first == committed_end.file) break;
      if (mysql_bin_log.find_next_log(&scan, false)) return false;
      offset = 4;
    }
  }
  DBUG_SIGNAL_WAIT_FOR(thd, "rapid_pause_committed_decode", "rapid_decode_opened", "rapid_decode_continue");
  // Open handles preserve file identity across RESET as well as PURGE. Neither
  // hashing nor decoding holds LOCK_index, so rotation can proceed concurrently.
  if (!start.prefix_digest.empty() && (!Utils::BinlogPositionReader::certify_stream(prefix_stream, identity) ||
                                       identity.prefix_digest != start.prefix_digest))
    return false;

  struct Map {
    std::shared_ptr<Table_map_log_event> event;
    const BinlogTableBinding *binding;
  };
  std::unordered_map<uint64_t, Map> maps;
  BinlogTransaction transaction;
  size_t transaction_bytes = 0, batch_bytes = 0;
  size_t budget = std::min<size_t>(shannon_rpd_engine_cfg.pop_buff_sz_max, 64ULL * 1024 * 1024);
  DBUG_EXECUTE_IF("rapid_committed_small_budget", { budget = 1024 * 1024; });
  bool in_payload = false;
  assembled_end = start;
  uint64_t scanned = 0;
  for (auto &[file, input] : inputs) {
    auto &reader = *input;
    uint64_t previous_offset = reader.position();
    binlog::Decompressing_event_object_istream stream(reader);
    stream.set_verify_checksum(true);
    auto grow = stream.get_grow_calculator();
    grow.set_max_size(budget);
    stream.set_grow_calculator(grow);
    auto check_stream = create_scope_guard([&] { (void)stream.has_error(); });
    const bool last = file == committed_end.file;
    while (!last || static_cast<uint64_t>(reader.position()) < committed_end.offset || in_payload) {
      if (thd->killed || (cancel && cancel->load())) return false;
      std::shared_ptr<Log_event> event;
      stream >> event;
      if (!event) {
        if (stream.has_error() || last) return false;
        break;
      }
      const uint64_t end_offset = reader.position();
      scanned += end_offset - previous_offset;
      previous_offset = end_offset;
      if (last && end_offset > committed_end.offset) return false;

      bool terminal = false;
      switch (event->get_type_code()) {
        case TRANSACTION_PAYLOAD_EVENT:
          in_payload = true;
          break;
        case TABLE_MAP_EVENT: {
          auto map = std::static_pointer_cast<Table_map_log_event>(event);
          const BinlogTableBinding *binding = nullptr;
          for (const auto &candidate : bindings)
            if (std::strcmp(map->get_db_name(), candidate.table->s->db.str) == 0 &&
                std::strcmp(map->get_table_name(), candidate.table->s->table_name.str) == 0) {
              binding = &candidate;
              break;
            }
          const uint64_t id = static_cast<uint64_t>(map->get_table_id());
          if (binding)
            maps[id] = {std::move(map), binding};
          else
            maps.erase(id);  // A reused id supersedes even a previously tracked table.
          break;
        }
        case WRITE_ROWS_EVENT:
        case UPDATE_ROWS_EVENT:
        case DELETE_ROWS_EVENT: {
          auto *rows = static_cast<Rows_log_event *>(event.get());
          auto found = maps.find(static_cast<uint64_t>(rows->get_table_id()));
          if (found == maps.end()) break;
          const auto &binding = *found->second.binding;
          if (covered(binding.floor, file, end_offset)) break;
          auto *table = binding.table;
          auto &map = *found->second.event;
          Table_map_log_event::Optional_metadata_fields optional(map.m_optional_metadata, map.m_optional_metadata_len);
          table_def definition(map.m_coltype, map.m_colcnt, map.m_field_metadata, map.m_field_metadata_size,
                               map.m_null_bits, map.m_flags, optional.m_vector_dimensionality);
          const uchar *cursor = rows->get_rows_begin(), *end = rows->get_rows_end();
          while (cursor < end) {
            if (thd->killed || (cancel && cancel->load())) return false;
            change_record_buff_t record(Source::COPY_INFO, table->s->rec_buff_length);
            record.m_copy_info_mode = CopyInfoMode::COMMITTED_BINLOG;
            record.m_table_id = binding.id;
            const auto type = event->get_type_code();
            record.m_oper = type == WRITE_ROWS_EVENT    ? change_record_buff_t::OperType::INSERT
                            : type == UPDATE_ROWS_EVENT ? change_record_buff_t::OperType::UPDATE
                                                        : change_record_buff_t::OperType::DELETE;
            if (!unpack(table, definition, rows->get_cols(), cursor, end, record.m_buff0.get(), record.m_offpage_data0))
              return false;
            if (type == UPDATE_ROWS_EVENT && !unpack(table, definition, rows->get_cols_ai(), cursor, end,
                                                     record.m_buff1.get(), record.m_offpage_data1))
              return false;
            if (!FinalizeBinlogRow(table, record)) return false;
            const size_t bytes = record.payload_bytes();
            if (bytes > budget || transaction_bytes > budget - bytes) {
              if (failure_reason)
                *failure_reason =
                    "Change propagation failed: committed transaction exceeds the reader memory budget; unload and "
                    "reload the table";
              return false;
            }
            transaction_bytes += bytes;
            transaction.changes.push_back(std::move(record));
          }
          break;
        }
        case XID_EVENT:
          terminal = true;
          break;
        case QUERY_EVENT: {
          auto *query = static_cast<Query_log_event *>(event.get());
          const std::string text(query->query, query->q_len);
          if (text == "BEGIN") {
            if (!transaction.changes.empty()) return false;
            maps.clear();
          } else if (text == "COMMIT")
            terminal = true;
          else if (text.starts_with("SAVEPOINT ") || text.starts_with("RELEASE SAVEPOINT ")) {
            if (recovery) return false;
            // InnoDB rollback-to-savepoint truncates the transaction cache;
            // surviving savepoint markers do not end a committed transaction.
          } else if (text == "ROLLBACK") {
            transaction.changes.clear();
            terminal = true;
          } else {
            // Source DDL is serialized by MDL and current schema/id validation
            // on every batch. Statement-logged DML is caught by sentinels.
            // XA and mixed-engine savepoint streams have no supported lifecycle.
            if (recovery || !transaction.changes.empty() || text.starts_with("XA ") || text.starts_with("ROLLBACK TO "))
              return false;
            terminal = true;
          }
          break;
        }
        case GTID_LOG_EVENT:
        case ANONYMOUS_GTID_LOG_EVENT:
          if (!transaction.changes.empty()) return false;
          maps.clear();
          break;
        case FORMAT_DESCRIPTION_EVENT:
        case ROTATE_EVENT:
        case STOP_EVENT:
        case PREVIOUS_GTIDS_LOG_EVENT:
        case HEARTBEAT_LOG_EVENT:
        case ROWS_QUERY_LOG_EVENT:
          break;
        case INTVAR_EVENT:
        case RAND_EVENT:
        case USER_VAR_EVENT:
          if (recovery) return false;
          break;
        default:
          return false;
      }
      if (terminal) {
        in_payload = false;
        transaction.end = {file, end_offset, {}};
        assembled_end = transaction.end;
        batch_bytes += transaction_bytes;
        if (!transaction.changes.empty()) batch.push_back(std::move(transaction));
        transaction = {};
        maps.clear();
        transaction_bytes = 0;

        if (batch.size() >= 256 || batch_bytes >= budget / 2 || scanned >= budget) return true;
      }
    }

    if (last) {
      if (in_payload || !transaction.changes.empty()) return false;
      assembled_end = committed_end;
      return true;
    }
  }
  return false;
}

int EnqueueRowChange(change_record_buff_t &record) noexcept { return Populator::write(nullptr, 0, &record); }
}  // namespace Populate
}  // namespace ShannonBase

namespace ShannonBase {
namespace Populate {
namespace {
using Position = BinlogPosition;
using Clock = std::chrono::steady_clock;
// Keep the full byte offset: a transaction can take a binlog beyond 4 GiB
// before rotation. The source hook uses only this allocation-free watermark.
using CommitPosition = std::pair<uint64_t, uint64_t>;
CommitPosition commit_position(const char *file, uint64_t offset) noexcept {
  if (!file || offset < 4) return {};
  const char *suffix = std::strrchr(file, '.');
  if (!suffix || !*++suffix) return {};
  uint64_t sequence = 0;
  for (; *suffix; ++suffix) {
    if (*suffix < '0' || *suffix > '9') return {};
    const auto digit = static_cast<unsigned>(*suffix - '0');
    if (sequence > (UINT64_MAX - digit) / 10) return {};
    sequence = sequence * 10 + digit;
  }
  return {sequence, offset};
}
CommitPosition commit_position(const Position &pos) { return commit_position(pos.file.c_str(), pos.offset); }
class CommitWatermark {
 public:
  CommitPosition load() {
    std::lock_guard lock(mutex);
    return value;
  }
  void store(CommitPosition next) {
    std::lock_guard lock(mutex);
    value = next;
  }
  void advance(CommitPosition next) {
    std::lock_guard lock(mutex);
    value = std::max(value, next);
  }

 private:
  std::mutex mutex;
  CommitPosition value{};
};
std::string schema_signature(const TABLE *table) {
  std::string result = std::to_string(table->s->rec_buff_length);
  for (uint i = 0; i < table->s->fields; ++i) {
    auto *f = table->field[i];
    result += ':' + std::string(f->field_name) + ':' + std::to_string(f->real_type()) + ':' +
              std::to_string(f->pack_length()) + ':' + std::to_string(f->field_length) + ':' +
              std::to_string(f->all_flags()) + ':' + std::to_string(f->charset()->number);
  }

  return result;
}

struct Loaded {
  table_id_t id;
  uint64_t generation;
  std::string db, table, schema;
  Position floor;
  const char *failure_reason{nullptr};
  Clock::time_point open_failed_since{};
};

struct Consumer {
  std::recursive_mutex lifecycle;
  std::mutex mutex;
  std::condition_variable cv;
  std::unordered_map<table_id_t, Loaded> loaded;
  IB_thread worker;
  std::atomic<bool> stop{false}, fault{false}, reset{false}, demand{false};
  CommitWatermark committed, assembled;
  CommitPosition visible{};
  uint64_t next_generation{0}, view_scn{0};
  Position cursor;
  std::shared_ptr<void> publication_pin;
  std::string prefix;
  bool running{false};

  // Coalesce requests without taking the catalog mutex on the commit path.
  // Atomic wait observes requests published before the worker starts waiting.
  void request_work() {
    if (!demand.exchange(true)) demand.notify_one();
  }

  ~Consumer() { CommittedBinlog::shutdown(); }
};

Consumer consumer;

Position position(CommitPosition value, const std::string &prefix) {
  char suffix[24] = {0};
  std::snprintf(suffix, sizeof(suffix), "%06llu", static_cast<unsigned long long>(value.first));
  return {prefix + suffix, value.second, {}};
}

bool still_loaded(const Loaded &entry) {
  std::lock_guard lock(consumer.mutex);
  auto it = consumer.loaded.find(entry.id);
  return it != consumer.loaded.end() && it->second.generation == entry.generation;
}

void quarantine(const std::vector<Loaded> &loaded, const char *reason) {
  for (const auto &table : loaded) {
    std::lock_guard lock(consumer.mutex);
    auto it = consumer.loaded.find(table.id);
    if (it != consumer.loaded.end() && it->second.generation == table.generation) {
      it->second.failure_reason = reason;
      QuarantinePropagationTable(table.id);
    }
  }
  sql_print_warning("Rapid COMMITTED_BINLOG: %s; affected tables require reload", reason);
  consumer.cv.notify_all();
}

const char *global_configuration() {
  if (!mysql_bin_log.is_open()) return "COMMITTED_BINLOG requires an open binary log";
  if (sync_binlog_period != 1 || srv_flush_log_at_trx_commit != 1)
    return "COMMITTED_BINLOG requires sync_binlog=1 and innodb_flush_log_at_trx_commit=1";
  if (!opt_binlog_order_commits) return "COMMITTED_BINLOG requires binlog_order_commits=ON";
  return nullptr;
}

// Row decoding needs a stable definition, not a primary-engine data read.
// MDL_SHARED excludes DDL but is compatible with LOCK TABLES WRITE, so a
// locked unrelated table cannot stall propagation for the whole catalog.
TABLE *open_decode_table(THD *thd, const Loaded &entry) {
  const char *db = strmake_root(thd->mem_root, entry.db.c_str(), entry.db.size());
  const char *name = strmake_root(thd->mem_root, entry.table.c_str(), entry.table.size());
  auto *ref = new (thd->mem_root) Table_ref(db, name, TL_READ);
  MDL_REQUEST_INIT(&ref->mdl_request, MDL_key::TABLE, db, name, MDL_SHARED, MDL_TRANSACTION);
  uint count = 0;
  if (open_tables(thd, &ref, &count, MYSQL_OPEN_GET_NEW_TABLE | MYSQL_OPEN_IGNORE_FLUSH | MYSQL_OPEN_FORCE_SHARED_MDL))
    return nullptr;
  return ref->table;
}

// One propagation pass. Returns a quarantine reason, or nullptr when the pass
// ended deliberately (stop, retry backoff, or nothing to do).
static const char *committed_binlog_propagation_pass(Utils::BackgroundSession &session,
                                                     const std::vector<Loaded> &catalog, const Position &start,
                                                     const std::string &prefix, CommitPosition target);

// Returns false when the consumer must be fenced for every query.
static bool committed_binlog_propagation_loop() {
  while (!consumer.stop.load()) {
    consumer.demand.wait(false);
    consumer.demand.store(false);
    if (consumer.stop.load()) return true;
    // Initialize MySQL thread-local state before catalog allocation or DBUG hooks.
    Utils::BackgroundSession session;
    // A failed catalog allocation leaves no table to quarantine.
    if (DBUG_EVALUATE_IF("rapid_committed_catalog_bad_alloc", true, false)) return false;
    std::vector<Loaded> catalog;
    Position start;
    std::string prefix;
    CommitPosition target{};
    {
      std::lock_guard lock(consumer.mutex);
      for (const auto &[id, table] : consumer.loaded)
        if (!IsPropagationBroken(id)) catalog.push_back(table);
      start = consumer.cursor;
      prefix = consumer.prefix;
      target = consumer.committed.load();
    }
    if (catalog.empty()) continue;
    if (consumer.fault.load() || global_configuration()) {
      quarantine(catalog, consumer.reset.load()
                              ? "Source binlog was reset; unload and reload the table"
                              : "Source configuration or commit position is unsupported; unload and reload the table");
      continue;
    }
    if (target <= commit_position(start)) continue;
    if (const char *failure = committed_binlog_propagation_pass(session, catalog, start, prefix, target))
      quarantine(catalog, failure);
  }
  return true;
}

static const char *committed_binlog_propagation_pass(Utils::BackgroundSession &session,
                                                     const std::vector<Loaded> &catalog, const Position &start,
                                                     const std::string &prefix, CommitPosition target) {
  // Source metadata is opened BEFORE any apply/queue lock. Never block DDL
  // or unload while holding those locks. Each batch closes its TABLEs.
  if (DBUG_EVALUATE_IF("rapid_committed_reader_stall", true, false)) return nullptr;
  THD *thd = session.thd();
  if (!thd) {
    quarantine(catalog, "cannot create reader session");
    return nullptr;
  }
  auto close_metadata = [&] {
    if (thd->open_tables) close_thread_tables(thd);
    thd->mdl_context.release_statement_locks();
    thd->mdl_context.release_transactional_locks();
  };
  auto metadata_guard = create_scope_guard(close_metadata);
  thd->variables.lock_wait_timeout = 1;
  thd->variables.use_secondary_engine = SECONDARY_ENGINE_OFF;
  std::vector<BinlogTableBinding> bindings;
  bool retry = false;
  for (const auto &entry : catalog) {
    TABLE *source = open_decode_table(thd, entry);
    if (!source) {
      bool expired = false;
      {
        std::lock_guard lock(consumer.mutex);
        auto found = consumer.loaded.find(entry.id);
        if (found == consumer.loaded.end() || found->second.generation != entry.generation) continue;
        auto &since = found->second.open_failed_since;
        if (since == Clock::time_point{}) since = Clock::now();
        expired = Clock::now() - since >= std::chrono::milliseconds(shannon_rpd_engine_cfg.query_sync_timeout_ms);
      }
      const auto error = thd->get_stmt_da()->mysql_errno();
      if (expired || error == ER_NO_SUCH_TABLE || error == ER_BAD_DB_ERROR) {
        quarantine({entry}, "source table is missing or unavailable; unload and reload the table");
        thd->clear_error();
        continue;
      }
      retry = true;
      break;
    }
    {
      std::lock_guard lock(consumer.mutex);
      auto found = consumer.loaded.find(entry.id);
      if (found != consumer.loaded.end() && found->second.generation == entry.generation)
        found->second.open_failed_since = {};
    }
    if (source->file->get_table_id() != entry.id || !BinlogParser::supports(source) ||
        schema_signature(source) != entry.schema) {
      quarantine({entry}, "source schema or table identity changed");
      continue;
    }
    bitmap_set_all(source->read_set);
    bitmap_set_all(source->write_set);
    bindings.push_back({source, entry.id, entry.floor});
  }
  if (retry) {
    // Unfinished input remains work even without a new commit. Back off
    // transient metadata failures, then retry with fresh TABLE handles.
    std::unique_lock lock(consumer.mutex);
    consumer.cv.wait_for(lock, std::chrono::milliseconds(20), [] { return consumer.stop.load(); });
    consumer.request_work();
    return nullptr;
  }
  std::vector<BinlogTransaction> batch;
  Position assembled;
  const char *failure_reason = nullptr;
  if (!BinlogParser::read_batch(thd, bindings, start, position(target, prefix), batch, assembled, &failure_reason,
                                &consumer.stop)) {
    if (consumer.stop.load()) return nullptr;
    return failure_reason ? failure_reason : "binlog reader allocation or decoding failed";
  }
  // Stamp a bounded batch, enqueue it in source order, and wait once per
  // table. All versions remain behind the publication fence until every
  // participant has applied its entire portion.
  uint64_t last_scn = 0;
  std::vector<table_id_t> touched;
  for (auto &transaction : batch) {
    const uint64_t scn = TransactionCoordinator::instance().allocate_scn();
    last_scn = scn;
    std::vector<table_id_t> transaction_tables;
    for (auto &record : transaction.changes) {
      auto found =
          std::find_if(catalog.begin(), catalog.end(), [&](const Loaded &t) { return t.id == record.m_table_id; });
      if (found == catalog.end() || !still_loaded(*found) || IsPropagationBroken(record.m_table_id)) continue;
      if (std::find(touched.begin(), touched.end(), record.m_table_id) == touched.end())
        touched.push_back(record.m_table_id);
      if (std::find(transaction_tables.begin(), transaction_tables.end(), record.m_table_id) ==
          transaction_tables.end())
        transaction_tables.push_back(record.m_table_id);
      record.m_commit_scn = scn;
      auto table =
          Imcs::Imcs::instance()->get_rpd_table<std::shared_ptr<ShannonBase::Imcs::RpdTable>>(record.m_table_id);
      auto *mgr = table ? table->recovery_manager() : nullptr;
      if (mgr && mgr->notifications()) record.m_notification_sequence = mgr->notifications()->notify(scn);
    }
    for (auto id : transaction_tables) {
      auto table = Imcs::Imcs::instance()->get_rpd_table<std::shared_ptr<ShannonBase::Imcs::RpdTable>>(id);
      auto *mgr = table ? table->recovery_manager() : nullptr;
      if (mgr && mgr->notifications()) mgr->notifications()->committed(scn, transaction.end);
    }
  }
  if (DBUG_EVALUATE_IF("rapid_committed_prepare_failure", true, false) && !touched.empty()) {
    quarantine(catalog, "committed batch propagation failed; reload required");
    return nullptr;
  }
  BeginCommittedTransactionPublish(touched);
  // Caller holds consumer.mutex. Pair every post-decode queue operation
  // with the attachment generation: unload/reload can reuse the table id.
  auto current_entry = [&](table_id_t id) {
    auto old = std::find_if(catalog.begin(), catalog.end(), [id](const Loaded &t) { return t.id == id; });
    auto now = consumer.loaded.find(id);
    return old != catalog.end() && now != consumer.loaded.end() && old->generation == now->second.generation;
  };
  bool fenced = true;
  auto finish_publish = [&] {
    std::lock_guard lock(consumer.mutex);
    std::erase_if(touched, [&](table_id_t id) { return !current_entry(id); });
    EndCommittedTransactionPublish(touched);
  };
  auto release_fence = create_scope_guard([&] {
    if (fenced) finish_publish();
  });
  // Records own detached row images. Release source metadata before queue
  // admission or apply waits, so SECONDARY_UNLOAD cannot deadlock with them.
  close_metadata();
  if (!touched.empty())
    DBUG_SIGNAL_WAIT_FOR(thd, "rapid_pause_committed_enqueue", "rapid_batch_decoded", "rapid_enqueue_continue");
  auto enqueue_current = [&](change_record_buff_t &record) {
    std::lock_guard lock(consumer.mutex);
    return current_entry(record.m_table_id) ? EnqueueRowChange(record) : SHANNON_SUCCESS;
  };
  const auto stall_timeout = std::chrono::milliseconds(shannon_rpd_engine_cfg.query_sync_timeout_ms);
  auto last_progress = Clock::now();
  bool success = true;
  for (auto &transaction : batch) {
    for (auto &record : transaction.changes) {
      if (!record.m_commit_scn) continue;
      int result;
      uint64_t progress = 0;
      while ((result = enqueue_current(record)) == static_cast<int>(PROPAGATION_FAILED::BUFFER_FULL)) {
        if (consumer.stop.load()) return nullptr;
        const auto current = Populator::request_table_barrier(record.m_table_id).applied_change_id;
        if (current != progress) {
          progress = current;
          last_progress = Clock::now();
        }
        if (Clock::now() - last_progress >= stall_timeout) {
          success = false;
          break;
        }
        std::unique_lock lock(consumer.mutex);
        consumer.cv.wait_for(lock, std::chrono::milliseconds(20));
      }
      if (!success || result != SHANNON_SUCCESS) {
        success = false;
        break;
      }
      last_progress = Clock::now();
    }
    if (!success) break;
  }
  finish_publish();
  fenced = false;
  for (auto id : touched) {
    PropagationBarrier barrier;
    {
      std::lock_guard lock(consumer.mutex);
      if (!current_entry(id)) continue;
      barrier = Populator::request_table_barrier(id);
    }
    last_progress = Clock::now();
    auto progress = barrier.applied_change_id;
    while (success && barrier.needs_wait()) {
      if (consumer.stop.load()) return nullptr;
      const auto result = Populator::wait_table_applied_for(id, barrier.required_change_id,
                                                            QUERY_PROPAGATION_WAIT_SLICE_MS, barrier.buffer_generation);
      if (result == TablePropagationWaitResult::APPLIED) break;
      if (result == TablePropagationWaitResult::BROKEN || result == TablePropagationWaitResult::GONE) {
        std::lock_guard lock(consumer.mutex);
        // Deliberate unload retires this participant, not the other tables.
        if (current_entry(id)) success = false;
        break;
      }
      const auto current = Populator::request_table_barrier(id).applied_change_id;
      if (current != progress) {
        progress = current;
        last_progress = Clock::now();
      }
      if (Clock::now() - last_progress >= stall_timeout) {
        success = false;
        break;
      }
    }
    if (barrier.state == TablePropagationState::BROKEN) success = false;
  }
  if (!success) {
    std::lock_guard lock(consumer.mutex);
    for (auto id : touched)
      if (current_entry(id)) QuarantinePropagationTable(id);
  }
  if (last_scn) {
    TransactionCoordinator::instance().observe_commit_scn(last_scn);
    std::lock_guard lock(consumer.mutex);
    consumer.publication_pin = TransactionCoordinator::instance().pin_scn(last_scn);
    consumer.view_scn = last_scn;
  }
#ifndef SHANNON_POSIX_PLATFORM
  // Platforms without a stable inode identifier retain full certification.
  if (!Utils::BinlogPositionReader::certify(assembled)) return "assembled source position was lost";
#endif
  {
    std::lock_guard lock(consumer.mutex);
    consumer.cursor = assembled;
    consumer.assembled.store(commit_position(assembled));
    consumer.visible = commit_position(assembled);
    if (consumer.committed.load() > consumer.visible) consumer.request_work();
  }
  for (const auto &entry : catalog) {
    std::lock_guard lock(consumer.mutex);
    if (!current_entry(entry.id) || IsPropagationBroken(entry.id)) continue;
    auto table = Imcs::Imcs::instance()->get_rpd_table<std::shared_ptr<ShannonBase::Imcs::RpdTable>>(entry.id);
    auto *mgr = table ? table->recovery_manager() : nullptr;
    if (mgr && mgr->notifications()) mgr->notifications()->advance_quiescent(assembled);
  }
  consumer.cv.notify_all();
  return nullptr;
}

// Thread entry: the loop reports failures through its return value.
static void committed_binlog_propagation_worker() noexcept {
  if (committed_binlog_propagation_loop()) return;
  consumer.fault.store(true);
  consumer.cv.notify_all();
  // No allocation is required to fence every query after thread setup failure.
  sql_print_warning("Rapid COMMITTED_BINLOG: reader thread failed; unload and reload the table");
}

// Recovery rebuilds from the primary, so it rejects table shapes the online
// reader could otherwise decode incrementally.
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
}  // namespace

bool BinlogReplay::replay(THD *thd, TABLE *table, const BinlogPosition &start,
                          const std::function<bool(change_record_buff_t &)> &apply) {
  if (!thd || !supported(table) || !mysql_bin_log.is_open() || !start.valid() || start.prefix_digest.empty())
    return false;
  // The caller retains write-excluding source MDL. Preserve recovery's strict
  // rejection of statement events and its fixed physical end, independently
  // of the online reader's (possibly absent) committed watermark.
  LOG_INFO current;
  if (mysql_bin_log.get_current_log(&current) || current.encrypted_header_size) return false;
  const BinlogPosition stop{basename(current.log_file_name), current.pos, {}};
  const std::vector<BinlogTableBinding> bindings{{table, table->file->get_table_id(), start}};
  auto cursor = start;
  const auto anchor_valid = [&] {
    auto anchor = start;
    return Utils::BinlogPositionReader::certify(anchor) && anchor.prefix_digest == start.prefix_digest;
  };
  if (!anchor_valid()) return false;
  while (cursor.file != stop.file || cursor.offset != stop.offset) {
    if (thd->killed) return false;
    std::vector<BinlogTransaction> batch;
    BinlogPosition end;
    if (!BinlogParser::read_batch(thd, bindings, cursor, stop, batch, end, nullptr, nullptr, true)) return false;
    if (end.file == cursor.file && end.offset == cursor.offset) return false;
    // Each bounded batch reopens input. Certify both the original anchor and
    // the next cut so RESET/reused names between batches cannot splice epochs.
    if (!anchor_valid() || !Utils::BinlogPositionReader::certify(end)) return false;
    for (auto &transaction : batch) {
      const uint64_t scn = TransactionCoordinator::instance().allocate_scn();
      for (auto &record : transaction.changes) {
        if (thd->killed) return false;
        record.m_commit_scn = scn;
        if (!apply(record)) return false;
      }
      // Allocation alone does not publish committed visibility for attachment.
      TransactionCoordinator::instance().observe_commit_scn(scn);
    }
    cursor = std::move(end);
  }
  return anchor_valid();
}

const char *CommittedBinlog::check_configuration(THD *thd) {
  if (const char *error = global_configuration()) return error;
  if (!thd || thd->variables.binlog_format != BINLOG_FORMAT_ROW ||
      thd->variables.binlog_row_image != BINLOG_ROW_IMAGE_FULL || thd->variables.binlog_row_value_options)
    return "COMMITTED_BINLOG requires ROW format, FULL row images and no partial JSON updates";
  if (!thd->variables.sql_log_bin) return "COMMITTED_BINLOG requires sql_log_bin=ON";
  return nullptr;
}

bool CommittedBinlog::attach(THD *thd, TABLE *table, const Position &baseline) {
  std::lock_guard lifecycle(consumer.lifecycle);
  if (check_configuration(thd) || !BinlogParser::supports(table) || commit_position(baseline).second == 0) return false;
  SourceTxnTracker::instance().start();
  if (!SourceTxnTracker::instance().subscribed()) return false;

  // Caller retains source write-excluding MDL from baseline capture through attachment.
  Position certified = baseline;
  if (!Utils::BinlogPositionReader::certify(certified)) return false;
  const auto id = table->file->get_table_id();
  std::lock_guard lock(consumer.mutex);
  consumer.loaded[id] = {
      id, ++consumer.next_generation, table->s->db.str, table->s->table_name.str, schema_signature(table), certified};
  if (!consumer.running) {
    consumer.stop.store(false);
    consumer.fault.store(false);
    consumer.reset.store(false);
    consumer.committed.store(commit_position(certified));
    consumer.visible = commit_position(certified);
    consumer.assembled.store(consumer.visible);
    consumer.view_scn = TransactionCoordinator::instance().get_current_scn();
    consumer.publication_pin = TransactionCoordinator::instance().pin_scn(consumer.view_scn);
    consumer.cursor = certified;
    consumer.prefix = certified.file.substr(0, certified.file.rfind('.') + 1);
    consumer.running = true;
    consumer.worker = os_thread_create(rapid_populate_thread_key, 0, committed_binlog_propagation_worker);
    consumer.worker.start();
  }
  consumer.request_work();
  consumer.cv.notify_all();
  return true;
}

void CommittedBinlog::detach(table_id_t id) {
  std::lock_guard lifecycle(consumer.lifecycle);
  bool last;
  {
    std::lock_guard lock(consumer.mutex);
    consumer.loaded.erase(id);
    last = consumer.loaded.empty();
  }
  consumer.cv.notify_all();
  if (last) shutdown();
}

void CommittedBinlog::shutdown() {
  std::lock_guard lifecycle(consumer.lifecycle);
  consumer.stop.store(true);
  consumer.request_work();
  consumer.cv.notify_all();

  if (consumer.worker.state() != IB_thread::State::INVALID) consumer.worker.join();

  std::lock_guard lock(consumer.mutex);
  consumer.loaded.clear();
  consumer.publication_pin.reset();
  consumer.running = false;
}

void CommittedBinlog::publish_commit(const char *file, uint64_t offset) noexcept {
  if (!file || offset < 4) return;  // read-only commits have no source position
  const auto value = commit_position(file, offset);
  if (!value.second) {
    consumer.fault.store(true);
    consumer.request_work();
    consumer.cv.notify_all();
    return;
  }
  consumer.committed.advance(value);
  if (value > consumer.assembled.load()) consumer.request_work();
}

void CommittedBinlog::notify_source_reset() noexcept {
  consumer.reset.store(true);
  consumer.fault.store(true);
  consumer.request_work();
  consumer.cv.notify_all();
}

void CommittedBinlog::notify_dml(THD *thd, TABLE *table, table_id_t table_id) noexcept {
  // Validate logging coverage only. Transaction caches and row-hook positions
  // are not committed boundaries; after_commit publishes the source position.
  struct Seen {
    THD *thd{};
    query_id_t query{};
    std::array<const TABLE *, 8> tables{};
    unsigned count{};
  };

  static thread_local Seen seen;
  if (seen.thd != thd || seen.query != thd->query_id) seen = {thd, thd->query_id, {}, 0};
  for (unsigned i = 0; i < seen.count; ++i) {
    if (seen.tables[i] == table) return;
  }

  if (seen.count < seen.tables.size()) seen.tables[seen.count++] = table;
  // The authoritative commit hook wakes the consumer after logging completes;
  // individual row notifications do not schedule background work.
  if (!check_configuration(thd) && thd->is_current_stmt_binlog_format_row() && !table->no_replicate &&
      binlog_filter->db_ok(table->s->db.str) && (!thd->slave_thread || opt_log_replica_updates) &&
      thd->get_transaction()->xid_state()->get_state() == XID_STATE::XA_NOTR)
    return;

  // The notification caller pins the loaded table; use that identity rather
  // than looking it up again while a propagation failure may unpublish it.

  {
    std::lock_guard lock(consumer.mutex);
    auto it = consumer.loaded.find(table_id);
    if (it != consumer.loaded.end())
      it->second.failure_reason = "source DML was not covered by ROW/FULL binlog logging; unload and reload the table";
  }
  QuarantinePropagationTable(table_id);
}

const char *CommittedBinlog::prepare_query(THD *thd, LEX *lex, uint64_t &view_scn) {
  if (thd_test_options(thd, OPTION_NOT_AUTOCOMMIT))
    return "Cannot offload query to Rapid: autocommit is disabled; set autocommit=1";
  if (thd->get_transaction()->xid_state()->get_state() != XID_STATE::XA_NOTR)
    return "Cannot offload query to Rapid: XA transactions are unsupported in COMMITTED_BINLOG mode";
  if (thd_test_options(thd, OPTION_BEGIN))
    return "Cannot offload query to Rapid: query is part of a multi-statement transaction; end the transaction first";
  if (thd->in_sub_stmt) return "Cannot offload query to Rapid: query is executed inside a stored function or trigger";

  if (const char *error = global_configuration()) return error;

  const auto target = consumer.committed.load();
  const auto deadline = Clock::now() + std::chrono::milliseconds(shannon_rpd_engine_cfg.query_sync_timeout_ms);
  std::unique_lock lock(consumer.mutex);
  for (;;) {
    if (!consumer.running || consumer.stop.load()) return "COMMITTED_BINLOG consumer is stopped";
    if (consumer.fault.load())
      return consumer.reset.load() ? "Source binlog was reset; unload and reload the table"
                                   : "COMMITTED_BINLOG source position is unsupported; unload and reload the table";
    for (auto *t = lex->query_tables; t; t = t->next_global) {
      if (t->is_placeholder()) continue;
      const auto share = shannon_loaded_tables->get(t->db, t->table_name);
      if (!share || !consumer.loaded.contains(share->m_tableid)) return "table is not admitted to COMMITTED_BINLOG";
      if (IsPropagationBroken(share->m_tableid)) {
        const auto &entry = consumer.loaded.at(share->m_tableid);
        return entry.failure_reason ? entry.failure_reason : "Change propagation failed; unload and reload the table";
      }
    }

    if (consumer.visible >= target) {
      auto *context = dynamic_cast<Rapid_execution_context *>(lex->secondary_engine_execution_context());
      if (!context) return "missing committed-binlog query context";
      context->m_committed_binlog_pin = TransactionCoordinator::instance().pin_scn(consumer.view_scn);
      view_scn = consumer.view_scn;
      return nullptr;
    }

    if (thd->killed) return "COMMITTED_BINLOG synchronization was killed";
    if (Clock::now() >= deadline) return "COMMITTED_BINLOG synchronization timed out";
    consumer.request_work();
    consumer.cv.notify_all();
    consumer.cv.wait_for(lock, std::chrono::milliseconds(QUERY_PROPAGATION_WAIT_SLICE_MS));
  }
}

uint64_t CommittedBinlog::query_scn(THD *thd) {
  if (thd && thd->lex) {
    auto *context = dynamic_cast<Rapid_execution_context *>(thd->lex->secondary_engine_execution_context());
    if (context && context->m_committed_binlog_view) return context->m_committed_binlog_scn;
  }
  if (thd && configured_change_propagation_mode.load() == CopyInfoMode::COMMITTED_BINLOG &&
      thd->secondary_engine_optimization() == Secondary_engine_optimization::SECONDARY) {
    my_error(ER_SECONDARY_ENGINE, MYF(0), "missing committed-binlog query snapshot");
    return 0;
  }
  // Primary loads/recovery are internal readers and have no secondary context.
  return TransactionCoordinator::instance().get_current_scn();
}

const char *CommittedBinlog::table_failure_reason(table_id_t id) {
  std::lock_guard lock(consumer.mutex);
  auto it = consumer.loaded.find(id);
  return it == consumer.loaded.end() ? nullptr : it->second.failure_reason;
}
}  // namespace Populate
}  // namespace ShannonBase
