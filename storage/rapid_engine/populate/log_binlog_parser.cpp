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

#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include "include/scope_guard.h"
#include "sql/binlog.h"
#include "sql/binlog/decompressing_event_object_istream.h"
#include "sql/binlog_reader.h"
#include "sql/field.h"
#include "sql/log_event.h"
#include "sql/rpl_utility.h"
#include "sql/sql_class.h"
#include "sql/table.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/populate/log_populate.h"
#include "storage/rapid_engine/recovery/binlog_recovery.h"
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
#include "sql/xa.h"
#include "storage/innobase/include/srv0srv.h"
#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/populate/log_dml_notification.h"
#include "storage/rapid_engine/recovery/recovery.h"
#include "storage/rapid_engine/recovery/table_persistence.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase::Populate {
namespace {
using namespace mysql::binlog::event;
using Position = Recovery::BinlogPosition;
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
  for (uint i = 0; i < table->s->fields; ++i)
    if (table->field[i]->gcol_info || table->field[i]->is_array()) return false;
  return true;
}

bool BinlogParser::read_batch(THD *thd, const std::vector<BinlogTableBinding> &bindings, const Position &start,
                              const Position &committed_end, std::vector<BinlogTransaction> &batch,
                              Position &assembled_end, const char **failure_reason) {
  if (failure_reason)
    *failure_reason =
        "Change propagation failed: missing, corrupt or unsupported committed binlog input; unload and reload the "
        "table";
  if (!thd || !start.valid() || !committed_end.valid() || !mysql_bin_log.is_open()) return false;
  Position identity = start;
  if (!start.prefix_digest.empty() &&
      (!Recovery::BinlogRecovery::certify(identity) || identity.prefix_digest != start.prefix_digest))
    return false;
  mysql_mutex_lock(mysql_bin_log.get_index_lock());
  auto unlock = create_scope_guard([&] { mysql_mutex_unlock(mysql_bin_log.get_index_lock()); });
  LOG_INFO pin, scan;
  if (mysql_bin_log.find_log_pos(&pin, nullptr, false)) return false;
  mysql_bin_log.register_log_info(&pin);
  auto unpin = create_scope_guard([&] { mysql_bin_log.unregister_log_info(&pin); });
  scan = pin;
  while (basename(scan.log_file_name) != start.file)
    if (mysql_bin_log.find_next_log(&scan, false)) return false;
  // Close the certify-to-pin RESET/purge race while the index is locked.
  if (!start.prefix_digest.empty() && (!Recovery::BinlogRecovery::certify_file(scan.log_file_name, identity) ||
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
  uint64_t offset = start.offset;
  bool in_payload = false;
  assembled_end = start;
  for (;;) {
#ifdef SHANNON_POSIX_PLATFORM
    // The local binlog prefix is immutable during normal server operation.
    // Anchor it once at LOAD, then check the file incarnation and every event's
    // CRC. Rehashing all previous bytes per batch makes consumption quadratic.
    struct FileIdentity {
      std::string path;
      uint64_t device{0}, inode{0};
    };
    static thread_local FileIdentity previous;
    struct stat status;
    if (::stat(scan.log_file_name, &status) || status.st_size < 0 || static_cast<uint64_t>(status.st_size) < offset)
      return false;
    const uint64_t device = status.st_dev, inode = status.st_ino;
    if (previous.path == scan.log_file_name && (previous.device != device || previous.inode != inode)) return false;
    previous = {scan.log_file_name, device, inode};
#endif
    Binlog_file_reader reader(true, 64 * 1024 * 1024);
    if (reader.open(scan.log_file_name, offset)) return false;
    binlog::Decompressing_event_object_istream stream(reader);
    stream.set_verify_checksum(true);
    auto grow = stream.get_grow_calculator();
    grow.set_max_size(budget);
    stream.set_grow_calculator(grow);
    auto check_stream = create_scope_guard([&] { (void)stream.has_error(); });
    const std::string file = basename(scan.log_file_name);
    const bool last = file == committed_end.file;
    while (!last || static_cast<uint64_t>(reader.position()) < committed_end.offset || in_payload) {
      if (thd->killed) return false;
      std::shared_ptr<Log_event> event;
      stream >> event;
      if (!event) {
        if (stream.has_error() || last) return false;
        break;
      }
      const uint64_t end_offset = reader.position();
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
          if (binding) {
            const uint64_t id = static_cast<uint64_t>(map->get_table_id());
            maps[id] = {std::move(map), binding};
          }
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
            change_record_buff_t record(Source::COMMITTED_BINLOG, table->s->rec_buff_length);
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
            size_t bytes = 2 * record.m_size;
            for (auto *image : {&record.m_offpage_data0, &record.m_offpage_data1})
              for (const auto &entry : *image) bytes += entry.second.first;
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
            // InnoDB rollback-to-savepoint truncates the transaction cache;
            // surviving savepoint markers do not end a committed transaction.
          } else if (text == "ROLLBACK") {
            transaction.changes.clear();
            terminal = true;
          } else {
            // Source DDL is serialized by MDL and current schema/id validation
            // on every batch. Statement-logged DML is caught by sentinels.
            // XA and mixed-engine savepoint streams have no supported lifecycle.
            if (!transaction.changes.empty() || text.starts_with("XA ") || text.starts_with("ROLLBACK TO "))
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
        case INTVAR_EVENT:
        case RAND_EVENT:
        case USER_VAR_EVENT:
          break;
        default:
          return false;
      }
      if (terminal) {
        in_payload = false;
        transaction.end = {file, end_offset, {}};
        assembled_end = transaction.end;
        batch_bytes += transaction_bytes;
        batch.push_back(std::move(transaction));
        transaction = {};
        maps.clear();
        transaction_bytes = 0;
        if (batch.size() >= 256 || batch_bytes >= budget / 2) return true;
      }
    }
    if (last) {
      if (in_payload || !transaction.changes.empty()) return false;
      assembled_end = committed_end;
      return true;
    }
    if (mysql_bin_log.find_next_log(&scan, false)) return false;
    offset = 4;
  }
}

int EnqueueRowChange(change_record_buff_t &record) { return Populator::write(nullptr, 0, &record); }
}  // namespace ShannonBase::Populate

namespace ShannonBase::Populate {
namespace {
using Position = Recovery::BinlogPosition;
using Clock = std::chrono::steady_clock;
// Supported local files are <=4GiB. Packing the numeric file suffix and byte
// offset permits a nonallocating monotone commit tracker in the source hook.
uint64_t packed(const char *file, uint64_t offset) noexcept {
  if (!file || offset < 4 || offset > UINT32_MAX) return 0;
  const char *suffix = std::strrchr(file, '.');
  if (!suffix || !*++suffix) return 0;
  uint64_t sequence = 0;
  for (; *suffix; ++suffix) {
    if (*suffix < '0' || *suffix > '9') return 0;
    sequence = sequence * 10 + (*suffix - '0');
    if (sequence > UINT32_MAX) return 0;
  }
  return (sequence << 32) | offset;
}
uint64_t packed(const Position &pos) { return packed(pos.file.c_str(), pos.offset); }
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
};
struct Consumer {
  std::recursive_mutex lifecycle;
  std::mutex mutex;
  std::condition_variable cv;
  std::unordered_map<table_id_t, Loaded> loaded;
  std::thread worker;
  std::atomic<bool> stop{false}, fault{false}, reset{false}, hint{false}, demand{false};
  std::atomic<uint64_t> committed{0}, assembled{0};
  uint64_t next_generation{0}, visible{0}, view_scn{0};
  Position cursor;
  std::string prefix;
  bool running{false};
  ~Consumer() { CommittedBinlog::shutdown(); }
};
Consumer consumer;
Position position(uint64_t value, const std::string &prefix) {
  char suffix[24];
  std::snprintf(suffix, sizeof(suffix), "%06u", static_cast<unsigned>(value >> 32));
  return {prefix + suffix, value & UINT32_MAX, {}};
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
void consume() {
  ResMgmt::WorkerRegistration resource_registration;
  for (;;) {
    std::vector<Loaded> catalog;
    Position start;
    std::string prefix;
    uint64_t target = 0;
    {
      std::unique_lock lock(consumer.mutex);
      consumer.cv.wait_for(lock, std::chrono::milliseconds(POP_MAX_WAIT_TIMEOUT),
                           [] { return consumer.stop.load() || consumer.demand.load(); });
      consumer.demand.store(false);
      consumer.hint.exchange(false, std::memory_order_relaxed);
      if (consumer.stop.load()) return;
      for (const auto &[id, table] : consumer.loaded)
        if (!IsPropagationBroken(id)) catalog.push_back(table);
      start = consumer.cursor;
      prefix = consumer.prefix;
      target = consumer.committed.load(std::memory_order_acquire);
    }
    if (connection_events_loop_aborted()) return;
    if (catalog.empty()) continue;
    if (consumer.fault.load() || global_configuration()) {
      quarantine(catalog, consumer.reset.load()
                              ? "Source binlog was reset; unload and reload the table"
                              : "Source configuration or commit position is unsupported; unload and reload the table");
      continue;
    }
    if (target <= packed(start)) continue;
    try {
      // Source metadata is opened BEFORE any apply/queue lock. Never block DDL
      // or unload while holding those locks. Each batch closes its TABLEs.
      std::vector<BinlogTransaction> batch;
      Position assembled;
      {
        Recovery::RecoveryAdminSession session;
        if (DBUG_EVALUATE_IF("rapid_committed_reader_stall", true, false)) continue;
        THD *thd = session.thd();
        if (!thd) {
          quarantine(catalog, "cannot create reader session");
          continue;
        }
        ShannonBase::ResMgmt::WorkScope resource_scope;
        if (const char *error = resource_scope.enter(thd, ShannonBase::ResMgmt::WorkClass::SYNC, true,
                                                     [] { return consumer.stop.load(); })) {
          if (!consumer.stop.load() && !ResMgmt::work_governor().stopping()) quarantine(catalog, error);
          continue;
        }
        // open_table_by_name takes external handler locks outside the server's
        // normal MYSQL_LOCK list. Release them before session table cleanup.
        auto unlock_tables = create_scope_guard([&] {
          for (TABLE *table = thd->open_tables; table; table = table->next)
            if (table->file) table->file->ha_external_lock(thd, F_UNLCK);
        });
        thd->variables.lock_wait_timeout = 1;
        thd->variables.use_secondary_engine = SECONDARY_ENGINE_OFF;
        std::vector<BinlogTableBinding> bindings;
        bool retry = false;
        for (const auto &entry : catalog) {
          TABLE *source = Utils::Util::open_table_by_name(thd, entry.db, entry.table, TL_READ_WITH_SHARED_LOCKS);
          if (!source) {
            retry = true;
            break;
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
        if (retry) continue;
        const char *failure_reason = nullptr;
        if (!BinlogParser::read_batch(thd, bindings, start, position(target, prefix), batch, assembled,
                                      &failure_reason)) {
          quarantine(catalog, failure_reason);
          continue;
        }
      }  // Release source TABLE/MDL and decode grant before queue/apply waits.
      // The parser has released the index lock. The bounded batch owns all
      // payloads; queue saturation now waits only in this background reader.
      // Enqueue the bounded decoded batch in transaction order, then wait for
      // each table's final barrier once. Keep all transaction SCNs hidden until
      // every table completes; publishing per row/transaction serializes apply
      // admission and CPU-group switching unnecessarily.
      Recovery::RecoveryAdminSession enqueue_session;
      if (!enqueue_session.thd()) {
        quarantine(catalog, "cannot create enqueue session");
        continue;
      }
      ResMgmt::WorkScope enqueue_scope;
      auto admit_enqueue = [&]() {
        return enqueue_scope.enter(enqueue_session.thd(), ResMgmt::WorkClass::SYNC, true,
                                   [] { return consumer.stop.load(); });
      };
      if (const char *error = admit_enqueue()) {
        if (!consumer.stop.load() && !ResMgmt::work_governor().stopping()) quarantine(catalog, error);
        continue;
      }
      bool success = true;
      uint64_t batch_scn = 0;
      std::vector<table_id_t> batch_touched;
      auto catalog_entry = [&](table_id_t id) {
        return std::find_if(catalog.begin(), catalog.end(), [&](const Loaded &entry) { return entry.id == id; });
      };
      auto current_unlocked = [&](table_id_t id) {
        auto entry = catalog_entry(id);
        auto current = consumer.loaded.find(id);
        return entry != catalog.end() && current != consumer.loaded.end() &&
               current->second.generation == entry->generation;
      };
      for (auto &transaction : batch) {
        if (consumer.stop.load() || ResMgmt::work_governor().stopping() || connection_events_loop_aborted()) return;
        const uint64_t scn = TransactionCoordinator::instance().allocate_scn();
        batch_scn = scn;
        std::vector<table_id_t> touched;
        std::vector<std::shared_ptr<table_pop_buffer_t>> fenced;
        {
          // Pair generation validation with enqueue/notification publication.
          // Detach must not redirect an old batch into a newly loaded table.
          std::lock_guard lock(consumer.mutex);
          for (auto &record : transaction.changes) {
            if (!current_unlocked(record.m_table_id) || IsPropagationBroken(record.m_table_id)) continue;
            if (std::find(touched.begin(), touched.end(), record.m_table_id) == touched.end())
              touched.push_back(record.m_table_id);
            record.m_commit_scn = scn;
            auto table = Imcs::Imcs::instance()->get_rpd_table_shared(record.m_table_id);
            auto *mgr = table ? table->recovery_manager() : nullptr;
            auto *tracker = mgr ? mgr->notifications() : nullptr;
            if (tracker) record.m_notification_sequence = tracker->notify(scn);
          }
          fenced.reserve(touched.size());
          for (auto id : touched) {
            auto &shard = get_pop_shard(id);
            std::unique_lock shard_lock(shard.mutex);
            auto &buffer = shard.buffers[id];
            if (!buffer) buffer = std::make_shared<table_pop_buffer_t>();
            fenced.push_back(buffer);
          }
          for (auto id : touched) {
            auto table = Imcs::Imcs::instance()->get_rpd_table_shared(id);
            auto *mgr = table ? table->recovery_manager() : nullptr;
            if (mgr && mgr->notifications()) mgr->notifications()->committed(scn, transaction.end);
          }
          for (auto &buffer : fenced) buffer->publish_fence.fetch_add(1, std::memory_order_acq_rel);
        }
        // Own the original buffer incarnation through exceptions and unload.
        // A table-id lookup at release could decrement a replacement buffer.
        auto release_fences = create_scope_guard([&] {
          for (auto &buffer : fenced) {
            if (buffer->publish_fence.fetch_sub(1, std::memory_order_acq_rel) == 1) {
              { std::lock_guard lock(buffer->barrier_mutex); }
              buffer->barrier_cv.notify_all();
            }
          }
        });
        for (auto &record : transaction.changes) {
          if (!record.m_commit_scn) continue;
          for (;;) {
            if (consumer.stop.load() || ResMgmt::work_governor().stopping() || connection_events_loop_aborted()) return;
            std::unique_lock lock(consumer.mutex);
            if (!current_unlocked(record.m_table_id)) break;
            const int result = EnqueueRowChange(record);
            if (result == static_cast<int>(PROPAGATION_FAILED::BUFFER_FULL)) {
              // With SYNC=1, apply must be able to drain this queue before the
              // reader reserves another grant. Never wait holding source MDL
              // or the consumer mutex, nor keep the CPU scope while sleeping.
              lock.unlock();
              enqueue_scope.reset();
              lock.lock();
              consumer.cv.wait_for(lock, std::chrono::milliseconds(20));
              lock.unlock();
              if (const char *error = admit_enqueue()) {
                if (consumer.stop.load() || ResMgmt::work_governor().stopping()) return;
                quarantine(catalog, error);
                success = false;
                break;
              }
              continue;
            }
            if (result != SHANNON_SUCCESS) success = false;
            break;
          }
          if (!success) break;
        }
        release_fences.reset();
        for (auto id : touched)
          if (std::find(batch_touched.begin(), batch_touched.end(), id) == batch_touched.end())
            batch_touched.push_back(id);
        if (!success) break;
      }
      enqueue_scope.reset();  // Allow apply to run before waiting for its barriers.
      for (auto id : batch_touched) {
        auto entry = catalog_entry(id);
        if (entry == catalog.end() || !still_loaded(*entry)) continue;
        PropagationBarrier barrier;
        {
          std::lock_guard lock(consumer.mutex);
          if (!current_unlocked(id)) continue;
          barrier = Populator::request_table_barrier(id);
        }
        while (success && barrier.needs_wait()) {
          if (consumer.stop.load() || ResMgmt::work_governor().stopping() || connection_events_loop_aborted()) return;
          if (!still_loaded(*entry)) break;
          const auto result = Populator::wait_table_applied_for(
              id, barrier.required_change_id, QUERY_PROPAGATION_WAIT_SLICE_MS, barrier.buffer_generation);
          if (result == TablePropagationWaitResult::APPLIED) break;
          if (result == TablePropagationWaitResult::BROKEN || result == TablePropagationWaitResult::GONE) {
            if (still_loaded(*entry)) success = false;
            break;
          }
        }
        if (barrier.state == TablePropagationState::BROKEN && still_loaded(*entry)) success = false;
      }
      if (consumer.stop.load() || ResMgmt::work_governor().stopping() || connection_events_loop_aborted()) return;
      if (!success) {
        // The remainder of this decoded batch has not been enqueued. Do not
        // advance its cursor or expose partial changes in any loaded table.
        quarantine(catalog, "committed batch propagation failed; reload required");
        continue;
      }
      // Hidden system versions become query-visible only after every table's
      // portion has applied. Quarantined tables cannot expose partial results.
      if (batch_scn) TransactionCoordinator::instance().observe_commit_scn(batch_scn);
      {
        std::lock_guard lock(consumer.mutex);
        consumer.visible = packed(assembled);
        if (batch_scn) consumer.view_scn = batch_scn;
      }
      consumer.cv.notify_all();
#ifndef SHANNON_POSIX_PLATFORM
      // Platforms without a stable inode identifier retain full certification.
      if (!Recovery::BinlogRecovery::certify(assembled)) {
        quarantine(catalog, "assembled source position was lost");
        continue;
      }
#endif
      {
        std::lock_guard lock(consumer.mutex);
        consumer.cursor = assembled;
        consumer.assembled.store(packed(assembled), std::memory_order_release);
        consumer.visible = packed(assembled);
        if (consumer.committed.load() > consumer.visible) consumer.demand.store(true);
      }
      consumer.cv.notify_all();
    } catch (...) {
      quarantine(catalog, "binlog reader allocation or decoding failed");
    }
  }
}
}  // namespace

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
  if (check_configuration(thd) || !BinlogParser::supports(table) || !packed(baseline)) return false;
  TransactionManager::instance().start();
  if (!TransactionManager::instance().subscribed()) return false;
  Position certified = baseline;
  if (!Recovery::BinlogRecovery::certify(certified)) return false;
  const auto id = table->file->get_table_id();
  std::lock_guard lock(consumer.mutex);
  consumer.loaded[id] = {
      id, ++consumer.next_generation, table->s->db.str, table->s->table_name.str, schema_signature(table), certified};
  if (!consumer.running) {
    consumer.stop.store(false);
    consumer.fault.store(false);
    consumer.reset.store(false);
    consumer.committed.store(packed(certified));
    consumer.visible = packed(certified);
    consumer.assembled.store(consumer.visible);
    consumer.view_scn = TransactionCoordinator::instance().get_current_scn();
    consumer.cursor = certified;
    consumer.prefix = certified.file.substr(0, certified.file.rfind('.') + 1);
    consumer.running = true;
    consumer.worker = std::thread(consume);
  }
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
  consumer.cv.notify_all();
  if (consumer.worker.joinable()) consumer.worker.join();
  std::lock_guard lock(consumer.mutex);
  consumer.loaded.clear();
  consumer.running = false;
}
void CommittedBinlog::publish_commit(const char *file, uint64_t offset) noexcept {
  if (!file || offset < 4) return;  // read-only commits have no source position
  const uint64_t value = packed(file, offset);
  if (!value) {
    consumer.fault.store(true);
    consumer.demand.store(true);
    consumer.cv.notify_all();
    return;
  }
  uint64_t old = consumer.committed.load(std::memory_order_relaxed);
  while (old < value &&
         !consumer.committed.compare_exchange_weak(old, value, std::memory_order_release, std::memory_order_relaxed)) {
  }
  const uint64_t read = consumer.assembled.load(std::memory_order_acquire);
  if ((value >> 32) == (read >> 32) && value > read && value - read >= CHANGE_PROPAGATION_BUFFER_TRIGGER_BYTES) {
    consumer.demand.store(true);
    consumer.cv.notify_all();
  }
}
void CommittedBinlog::notify_source_reset() noexcept {
  consumer.reset.store(true);
  consumer.fault.store(true);
  consumer.demand.store(true);
  consumer.cv.notify_all();
}
void CommittedBinlog::notify_dml(THD *thd, TABLE *table) noexcept {
  // This is only a DML hint. Transaction caches and row-hook positions are not
  // committed boundaries; after_commit publishes the authoritative position.
  struct Seen {
    THD *thd{};
    query_id_t query{};
    std::array<const TABLE *, 8> tables{};
    unsigned count{};
  };
  static thread_local Seen seen;
  if (seen.thd != thd || seen.query != thd->query_id) seen = {thd, thd->query_id, {}, 0};
  for (unsigned i = 0; i < seen.count; ++i)
    if (seen.tables[i] == table) return;
  if (seen.count < seen.tables.size()) seen.tables[seen.count++] = table;
  consumer.hint.store(true, std::memory_order_relaxed);
  // The periodic, capacity and query-demand triggers wake the consumer. Avoid
  // a foreground scheduler wakeup for every short source transaction.
  const bool covered = !check_configuration(thd) && thd->is_current_stmt_binlog_format_row() && !table->no_replicate &&
                       binlog_filter->db_ok(table->s->db.str) && (!thd->slave_thread || opt_log_replica_updates) &&
                       thd->get_transaction()->xid_state()->get_state() == XID_STATE::XA_NOTR;
  if (!covered) {
    auto loaded = Imcs::Imcs::instance()->get_rpd_table_by_name(table->s->db.str, table->s->table_name.str);
    if (loaded) {
      {
        std::lock_guard lock(consumer.mutex);
        auto it = consumer.loaded.find(loaded->meta().table_id);
        if (it != consumer.loaded.end())
          it->second.failure_reason =
              "source DML was not covered by ROW/FULL binlog logging; unload and reload the table";
      }
      QuarantinePropagationTable(loaded->meta().table_id);
    } else
      Recovery::note_unregistered_source_change(table->s->db.str, table->s->table_name.str);
  }
}
const char *CommittedBinlog::prepare_query(THD *thd, LEX *lex, uint64_t &view_scn) {
  if (thd_test_options(thd, OPTION_NOT_AUTOCOMMIT))
    return "Cannot offload query to Rapid: autocommit is disabled; set autocommit=1";
  if (thd->get_transaction()->xid_state()->get_state() != XID_STATE::XA_NOTR)
    return "Cannot offload query to Rapid: XA transactions are unsupported in COMMITTED_BINLOG mode";
  if (thd_test_options(thd, OPTION_BEGIN))
    return "Cannot offload query to Rapid: query is part of a multi-statement transaction; end the transaction first";
  if (thd->in_sub_stmt) return "Cannot offload query to Rapid: query is executed inside a stored function or trigger";
  if (const char *error = check_configuration(thd)) return error;
  const uint64_t target = consumer.committed.load(std::memory_order_acquire);
  const auto deadline = Clock::now() + std::chrono::seconds(30);
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
      view_scn = consumer.view_scn;
      return nullptr;
    }
    if (thd->killed) return "COMMITTED_BINLOG synchronization was killed";
    if (Clock::now() >= deadline) return "COMMITTED_BINLOG synchronization timed out";
    consumer.demand.store(true);
    consumer.cv.notify_all();
    consumer.cv.wait_for(lock, std::chrono::milliseconds(QUERY_PROPAGATION_WAIT_SLICE_MS));
  }
}
uint64_t CommittedBinlog::query_scn(THD *thd) {
  if (thd && thd->lex) {
    auto *context = dynamic_cast<Rapid_execution_context *>(thd->lex->secondary_engine_execution_context());
    if (context && context->m_committed_binlog_view) return context->m_committed_binlog_scn;
  }
  return TransactionCoordinator::instance().get_current_scn();
}
const char *CommittedBinlog::table_failure_reason(table_id_t id) {
  std::lock_guard lock(consumer.mutex);
  auto it = consumer.loaded.find(id);
  return it == consumer.loaded.end() ? nullptr : it->second.failure_reason;
}
}  // namespace ShannonBase::Populate
