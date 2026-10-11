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
*/
#include "storage/rapid_engine/recovery/table_persistence.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "storage/rapid_engine/include/rapid_arch_inf.h"

#ifdef SHANNON_POSIX_PLATFORM
#include <fcntl.h>   // O_WRONLY
#include <unistd.h>  // fdatasync, close
#endif

#include "sql/log.h"                          // sql_print_warning
#include "storage/innobase/include/ut0dbg.h"  // DBUG_PRINT, UNIV_SQL_NULL
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/recovery/binlog_recovery.h"
#include "storage/rapid_engine/utils/crc.h"

namespace ShannonBase {
namespace Imcs {

namespace fs = std::filesystem;

namespace {
template <typename T>
void append_pod(std::string &out, const T &v) {
  out.append(reinterpret_cast<const char *>(&v), sizeof(T));
}

template <typename T>
void write_pod(std::ostream &out, const T &v) {
  out.write(reinterpret_cast<const char *>(&v), sizeof(v));
}
template <typename T>
bool read_pod(std::istream &in, T &v) {
  return static_cast<bool>(in.read(reinterpret_cast<char *>(&v), sizeof(v)));
}
void append_str(std::string &out, const std::string &s) {
  append_pod<uint32_t>(out, static_cast<uint32_t>(s.size()));
  out.append(s.data(), s.size());
}

struct ByteReader {
  const char *p{nullptr};
  size_t n{0};
  size_t off{0};

  bool read(void *dst, size_t len) {
    if (off + len > n) return false;
    if (dst) std::memcpy(dst, p + off, len);
    off += len;
    return true;
  }

  template <typename T>
  bool read_pod(T &v) {
    return read(&v, sizeof(T));
  }

  bool read_str(std::string &s) {
    uint32_t len = 0;
    if (!read_pod(len)) return false;
    if (off + len > n) return false;
    s.assign(p + off, len);
    off += len;
    return true;
  }
};

// FNV-1a 64-bit: deterministic across restarts (unlike std::hash<std::string>).
uint64_t fnv1a64(const void *data, size_t len, uint64_t h = 1469598103934665603ull) {
  const auto *b = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; ++i) {
    h ^= b[i];
    h *= 1099511628211ull;
  }
  return h;
}

uint64_t compute_schema_fingerprint(const TableMetadata &meta) {
  uint64_t h = 1469598103934665603ull;
  auto feed = [&](const void *p, size_t n) { h = fnv1a64(p, n, h); };
  auto feed_pod = [&](const auto &v) { feed(&v, sizeof(v)); };
  auto feed_str = [&](const std::string &s) {
    const uint32_t len = static_cast<uint32_t>(s.size());
    feed_pod(len);
    feed(s.data(), s.size());
  };

  feed_pod(static_cast<uint32_t>(meta.num_columns));
  for (const auto &f : meta.fields) {
    feed_pod(f.field_id);
    feed_str(f.field_name);
    feed_pod(static_cast<uint32_t>(f.type));
    feed_pod(f.pack_length);
    feed_pod(f.normalized_length);
    feed_pod(static_cast<uint8_t>(f.nullable));
    feed_pod(static_cast<uint8_t>(f.is_key));
    feed_pod(static_cast<uint8_t>(f.is_secondary_field));
    feed_pod(static_cast<uint32_t>(f.encoding));
    feed_pod(static_cast<uint64_t>(f.charset ? f.charset->number : 0));
  }

  feed_pod(static_cast<uint32_t>(meta.keys.size()));
  for (const auto &k : meta.keys) {
    feed_str(k.key_name);
    feed_pod(k.key_length);
    feed_pod(static_cast<uint32_t>(k.key_parts.size()));
    for (const auto &kp : k.key_parts) {
      feed_pod(kp.null_bit);
      feed_pod(kp.key_field_ind);
      feed_pod(kp.key_part_flag);
      feed_pod(kp.length);
    }
  }
  return h;
}

}  // namespace
TablePersistenceManager::TablePersistenceManager(const std::string &dir, const std::string &db,
                                                 const std::string &table)
    : m_db_name(db),
      m_tbl_name(table),
      m_partition_dir(fs::path(dir) / db / table),
      m_recovery_taint_root(fs::path(dir).parent_path() / "rapid_taint"),
      m_store(Recovery::CheckpointStores::current()) {}
bool TablePersistenceManager::open() {
  if (!Recovery::DurableFileSystem::create_directories(m_partition_dir)) return false;
  bool restored = m_store->restore(m_db_name, m_tbl_name, m_partition_dir);
  DBUG_EXECUTE_IF("rapid_checkpoint_store_restore_error", { restored = false; });
  if (!restored) {
    require_recovery();
    return mark_recovery_taint();
  }
  return true;
}
bool TablePersistenceManager::reset_epoch() {
  std::lock_guard checkpoint(m_checkpoint_mutex);
  std::lock_guard gate(m_notifications.mutex());
  if (!m_store->erase(m_db_name, m_tbl_name)) return false;
  for (auto generation : list_manifest_generations())
    if (!remove_generation(generation)) return false;
  m_notifications.reset();
  m_recovery_required.store(false);
  clear_recovery_taint();
  return true;
}
bool TablePersistenceManager::revoke_fast_recovery() {
  m_notifications.invalidate();
  require_recovery();
  return mark_recovery_taint();
}
namespace {
/** Write a marker: content is irrelevant, durable existence is the fact. */
bool persist_taint_marker(const fs::path &path) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) return false;
  return Recovery::DurableFileSystem::persist_file(path, std::string());
}

bool taint_marker_present(const fs::path &path) {
  std::error_code ec;
  const bool present = fs::exists(path, ec);
  return present && !ec;
}
}  // namespace

bool TablePersistenceManager::mark_recovery_taint() {
  if (!m_store->invalidate(m_db_name, m_tbl_name)) return false;
  bool primary_unwritable = false;
  DBUG_EXECUTE_IF("rapid_taint_marker_primary_error", { primary_unwritable = true; });
  if (!primary_unwritable && persist_taint_marker(recovery_taint_path())) return true;
  // The table's own directory could not record it; the separate tree may still
  // be able to, and one readable copy is all recovery needs.
  return persist_taint_marker(recovery_taint_alt_path());
}

bool TablePersistenceManager::recovery_tainted() const {
  return taint_marker_present(recovery_taint_path()) || taint_marker_present(recovery_taint_alt_path());
}

void TablePersistenceManager::clear_recovery_taint() {
  std::error_code ec;
  if (fs::remove(recovery_taint_path(), ec)) Recovery::DurableFileSystem::sync_directory(recovery_taint_path());
  ec.clear();
  if (fs::remove(recovery_taint_alt_path(), ec)) Recovery::DurableFileSystem::sync_directory(recovery_taint_alt_path());
}

fs::path TablePersistenceManager::snap_path(uint64_t generation, uint32_t imcu_id) const {
  std::ostringstream ss;
  ss << "checkpoint-" << generation << "/imcu_" << imcu_id << ".snap";
  return m_partition_dir / "snapshots" / ss.str();
}

fs::path TablePersistenceManager::manifest_path(uint64_t generation) const {
  std::ostringstream ss;
  ss << "checkpoint-" << generation << ".manifest";
  return m_partition_dir / "checkpoints" / ss.str();
}

uint64_t TablePersistenceManager::latest_generation() const {
  const auto gens = list_manifest_generations();
  return gens.empty() ? 0 : gens.back();
}

std::vector<uint64_t> TablePersistenceManager::list_manifest_generations() const {
  std::vector<uint64_t> gens;
  std::error_code ec;
  const fs::path dir = m_partition_dir / "checkpoints";
  if (!fs::is_directory(dir, ec)) return gens;
  for (const auto &e : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    const std::string stem = e.path().stem().string();  // "checkpoint-N"
    if (stem.rfind("checkpoint-", 0) != 0) continue;
    try {
      gens.push_back(std::stoull(stem.substr(11)));
    } catch (...) {
    }
  }
  std::sort(gens.begin(), gens.end());
  return gens;
}

bool TablePersistenceManager::remove_generation(uint64_t generation) {
  DBUG_EXECUTE_IF("rapid_reset_epoch_remove_generation_fail", { return false; });
  const std::string gen_dir = "checkpoint-" + std::to_string(generation);
  bool ok = true;
  const auto snapshot_dir = m_partition_dir / "snapshots" / gen_dir;
  std::error_code ec;
  const bool snapshot_exists = fs::exists(snapshot_dir, ec);
  if (ec) return false;
  if (snapshot_exists && !Recovery::DurableFileSystem::remove_directory(snapshot_dir)) ok = false;

  const auto manifest = m_partition_dir / "checkpoints" / (gen_dir + ".manifest");
  ec.clear();
  const bool manifest_exists = fs::exists(manifest, ec);
  if (ec) return false;
  if (manifest_exists) {
    fs::remove(manifest, ec);
    if (ec || !Recovery::DurableFileSystem::sync_directory(manifest)) ok = false;
  }
  const auto capture_meta = m_partition_dir / "checkpoints" / ("capture-" + std::to_string(generation) + ".meta");
  ec.clear();
  const bool removed_capture = fs::remove(capture_meta, ec);
  if (ec || (removed_capture && !Recovery::DurableFileSystem::sync_directory(capture_meta))) ok = false;
  return ok;
}

bool TablePersistenceManager::persist_manifest(const RecoveryManifest &manifest) {
  std::string out;
  out.reserve(64 + manifest.imcus.size() * 96);
  append_pod(out, MANIFEST_MAGIC);
  append_pod(out, MANIFEST_FORMAT_VER);
  append_pod(out, manifest.table_id);
  append_pod(out, manifest.generation);
  append_pod(out, manifest.schema_fingerprint);
  append_str(out, manifest.binlog.file);
  append_pod(out, manifest.binlog.offset);
  append_str(out, manifest.binlog.prefix_digest);
  append_pod(out, static_cast<uint32_t>(manifest.imcus.size()));
  for (const auto &e : manifest.imcus) {
    append_pod(out, e.imcu_id);
    append_pod(out, static_cast<uint8_t>(e.state));
    append_pod(out, e.snapshot_sequence);
    append_pod(out, e.snapshot_size);
    append_pod(out, e.snapshot_crc);
    append_str(out, e.snapshot_file);
  }
  const uint32_t crc = Utils::crc32c_compute(out.data(), out.size(), 0);
  append_pod(out, crc);
  return Recovery::DurableFileSystem::persist_file(manifest_path(manifest.generation), out);
}

Result<RecoveryManifest> TablePersistenceManager::load_manifest(uint64_t generation) const {
  RecoveryManifest m;
  std::ifstream in(manifest_path(generation), std::ios::binary);
  if (!in.is_open()) return {ErrorCode::NOT_FOUND, m};

  in.seekg(0, std::ios::end);
  const std::streamoff file_size = in.tellg();
  if (file_size <= 0 || file_size > 64 * 1024 * 1024) return {ErrorCode::CORRUPTION, m};
  in.seekg(0, std::ios::beg);

  std::string data(static_cast<size_t>(file_size), '\0');
  if (!in.read(data.data(), file_size)) return {ErrorCode::CORRUPTION, m};

  ByteReader r{data.data(), data.size()};
  uint32_t magic = 0;
  uint16_t ver = 0;
  if (!r.read_pod(magic) || magic != MANIFEST_MAGIC) return {ErrorCode::CORRUPTION, m};
  if (!r.read_pod(ver) || ver != MANIFEST_FORMAT_VER) return {ErrorCode::CORRUPTION, m};
  if (!r.read_pod(m.table_id)) return {ErrorCode::CORRUPTION, m};
  if (!r.read_pod(m.generation)) return {ErrorCode::CORRUPTION, m};
  if (!r.read_pod(m.schema_fingerprint)) return {ErrorCode::CORRUPTION, m};
  if (!r.read_str(m.binlog.file) || !r.read_pod(m.binlog.offset) || !r.read_str(m.binlog.prefix_digest) ||
      !m.binlog.valid())
    return {ErrorCode::CORRUPTION, m};

  uint32_t count = 0;
  if (!r.read_pod(count)) return {ErrorCode::CORRUPTION, m};
  if (count > (1u << 20)) return {ErrorCode::CORRUPTION, m};
  m.imcus.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    ManifestImcuEntry e;
    uint8_t st = 0;
    if (!r.read_pod(e.imcu_id)) return {ErrorCode::CORRUPTION, m};
    if (!r.read_pod(st)) return {ErrorCode::CORRUPTION, m};
    e.state = static_cast<ManifestImcuState>(st);
    if (!r.read_pod(e.snapshot_sequence)) return {ErrorCode::CORRUPTION, m};
    if (!r.read_pod(e.snapshot_size)) return {ErrorCode::CORRUPTION, m};
    if (!r.read_pod(e.snapshot_crc)) return {ErrorCode::CORRUPTION, m};
    if (!r.read_str(e.snapshot_file)) return {ErrorCode::CORRUPTION, m};
    m.imcus.push_back(std::move(e));
  }

  if (r.off + sizeof(uint32_t) != data.size()) return {ErrorCode::CORRUPTION, m};
  uint32_t stored_crc = 0;
  if (!r.read_pod(stored_crc)) return {ErrorCode::CORRUPTION, m};
  const uint32_t computed_crc = Utils::crc32c_compute(data.data(), r.off - sizeof(uint32_t), 0);
  if (stored_crc != computed_crc) return {ErrorCode::CORRUPTION, m};

  return {ErrorCode::OK, std::move(m)};
}

// Snapshot file header layout (36 bytes):
//  [SNAP_MAGIC 4B][version 2B][imcu_id 4B][col_count 4B]
//  [snap_lsn 8B][timestamp_us 8B][reserved 6B]
bool TablePersistenceManager::write_snap_header(std::ostream &out, uint32_t imcu_id, uint32_t col_count,
                                                uint64_t snap_lsn) const {
  write_pod(out, SNAP_MAGIC);
  write_pod(out, SNAP_FORMAT_VER);
  write_pod(out, imcu_id);
  write_pod(out, col_count);
  write_pod(out, snap_lsn);

  // Timestamp: microseconds since Unix epoch.
  auto now_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
  write_pod(out, now_us);

  const uint8_t reserved[6] = {};
  out.write(reinterpret_cast<const char *>(reserved), 6);
  // total: 4+2+4+4+8+8+6 = 36 ✓
  return out.good();
}

bool TablePersistenceManager::read_snap_header(std::istream &in, uint32_t &imcu_id, uint32_t &col_count,
                                               uint64_t &snap_lsn) const {
  uint32_t magic = 0;
  uint16_t version = 0;
  if (!read_pod(in, magic) || magic != SNAP_MAGIC) return false;
  if (!read_pod(in, version) || version != SNAP_FORMAT_VER) return false;
  if (!read_pod(in, imcu_id)) return false;
  if (!read_pod(in, col_count)) return false;
  if (!read_pod(in, snap_lsn)) return false;
  uint64_t ts = 0;
  if (!read_pod(in, ts)) return false;
  uint8_t reserved[6] = {};
  in.read(reinterpret_cast<char *>(reserved), 6);
  return in.good();
}

bool TablePersistenceManager::write_imcu_metadata(std::ostream &out, const Imcu *imcu) const {
  // The published count, not current_rows. The CUs below are serialized up to the
  // frontier, and the restore publishes exactly the number it reads back here, so
  // a slot that was allocated but never applied -- a failed insert, or one still
  // in flight -- must not appear in this number: it has no cell data to restore.
  write_pod(out, static_cast<uint64_t>(imcu->get_row_count()));
  write_pod(out, static_cast<uint64_t>(imcu->get_start_row()));
  write_pod(out, static_cast<uint64_t>(imcu->get_end_row()));
  write_pod(out, static_cast<uint64_t>(imcu->get_capacity()));
  write_pod(out, static_cast<uint8_t>(imcu->get_status()));

  // Delete mask (bit_array_t: rows, byte-size, then bytes).
  const bit_array_t *del = imcu->get_del_mask();
  write_pod(out, static_cast<uint64_t>(del ? del->rows : 0));
  write_pod(out, static_cast<uint64_t>(del ? del->size : 0));
  if (del && del->size > 0) out.write(reinterpret_cast<const char *>(del->data), del->size);

  // Per-column NULL masks.
  const auto &null_masks = imcu->get_null_masks();
  write_pod(out, static_cast<uint64_t>(null_masks.size()));
  for (const auto &nm : null_masks) {
    write_pod(out, static_cast<uint64_t>(nm ? nm->rows : 0));
    write_pod(out, static_cast<uint64_t>(nm ? nm->size : 0));
    if (nm && nm->size > 0) out.write(reinterpret_cast<const char *>(nm->data), nm->size);
  }

  return out.good();
}

bool TablePersistenceManager::read_imcu_metadata(std::istream &in, Imcu *imcu) const {
  uint64_t current_rows = 0, start_row = 0, end_row = 0, capacity = 0;
  uint8_t status = 0;
  if (!read_pod(in, current_rows)) return false;
  if (!read_pod(in, start_row)) return false;
  if (!read_pod(in, end_row)) return false;
  if (!read_pod(in, capacity)) return false;
  if (!read_pod(in, status)) return false;

  // A snapshot must match the live IMCU geometry.  Silently skipping masks on
  // a size mismatch can resurrect deleted/NULL rows while still reporting a
  // successful recovery.
  if (capacity != imcu->get_capacity() || current_rows > capacity || end_row < start_row ||
      end_row - start_row != capacity) {
    return false;
  }

  uint64_t del_rows = 0, del_bytes = 0;
  if (!read_pod(in, del_rows)) return false;
  if (!read_pod(in, del_bytes)) return false;
  bit_array_t *del = imcu->get_del_mask();
  if (!del || del->rows != del_rows || del->size != del_bytes) return false;
  std::vector<uint8_t> del_data(static_cast<size_t>(del_bytes));
  if (del_bytes > 0 && !in.read(reinterpret_cast<char *>(del_data.data()), static_cast<std::streamsize>(del_bytes)))
    return false;

  uint64_t null_mask_count = 0;
  if (!read_pod(in, null_mask_count)) return false;
  auto &null_masks = imcu->get_null_masks();
  if (null_mask_count != null_masks.size()) return false;

  struct NullMaskImage {
    uint64_t rows{0};
    uint64_t bytes{0};
    std::vector<uint8_t> data;
  };
  std::vector<NullMaskImage> images(null_masks.size());
  for (size_t i = 0; i < images.size(); ++i) {
    auto &img = images[i];
    if (!read_pod(in, img.rows)) return false;
    if (!read_pod(in, img.bytes)) return false;

    if (null_masks[i]) {
      if (null_masks[i]->rows != img.rows || null_masks[i]->size != img.bytes) return false;
    } else if (img.rows != 0 || img.bytes != 0) {
      return false;
    }

    img.data.resize(static_cast<size_t>(img.bytes));
    if (img.bytes > 0 && !in.read(reinterpret_cast<char *>(img.data.data()), static_cast<std::streamsize>(img.bytes)))
      return false;
  }

  // Apply only after the complete metadata image has been validated/read, so a
  // corrupt tail never leaves a partially mutated live IMCU.
  imcu->new_start_row(static_cast<row_id_t>(start_row));
  imcu->set_end_row(static_cast<row_id_t>(end_row));
  imcu->set_current_rows(static_cast<size_t>(current_rows));
  imcu->set_status(static_cast<Imcu::imcu_header_t::Status>(status));
  if (del_bytes > 0) std::memcpy(del->data, del_data.data(), static_cast<size_t>(del_bytes));
  for (size_t i = 0; i < images.size(); ++i) {
    if (null_masks[i] && images[i].bytes > 0)
      std::memcpy(null_masks[i]->data, images[i].data.data(), static_cast<size_t>(images[i].bytes));
  }

  imcu->rebuild_tombstone_counter();

  return in.good();
}

bool TablePersistenceManager::serialize_imcu(Imcu *imcu, uint64_t snapshot_sequence, std::string &out) const {
  const uint32_t imcu_id = imcu->get_imcu_id();
  const uint32_t col_count = static_cast<uint32_t>(imcu->get_column_count());

  std::ostringstream snap(std::ios::binary);
  if (!write_snap_header(snap, imcu_id, col_count, snapshot_sequence)) return false;
  if (!write_imcu_metadata(snap, imcu)) return false;

  const size_t row_count = imcu->get_row_count();
  for (uint32_t c = 0; c < col_count; ++c) {
    auto *cu = imcu->get_cu(c);
    if (!cu) {
      uint64_t sentinel = 0;
      snap.write(reinterpret_cast<const char *>(&sentinel), sizeof(sentinel));
      continue;
    }

    auto size_pos = snap.tellp();
    uint64_t cu_size_placeholder = 0;
    snap.write(reinterpret_cast<const char *>(&cu_size_placeholder), sizeof(cu_size_placeholder));

    auto data_start = snap.tellp();
    const int ser_ret = cu->serialize(snap, row_count);
    if (ser_ret != ShannonBase::SHANNON_SUCCESS) {
      DBUG_PRINT("cu_recovery", ("serialize_imcu: CU %u serialize failed", c));
      return false;
    }
    auto data_end = snap.tellp();

    // Go back and write the actual CU size (data_end - data_start).
    const uint64_t cu_size = static_cast<uint64_t>(data_end - data_start);
    snap.seekp(size_pos);
    snap.write(reinterpret_cast<const char *>(&cu_size), sizeof(cu_size));
    snap.seekp(data_end);
  }

  if (!snap.good()) {
    DBUG_PRINT("cu_recovery", ("serialize_imcu: stream error"));
    return false;
  }
  out = snap.str();
  return true;
}

bool TablePersistenceManager::checkpoint(Imcu *trigger, uint64_t snapshot_sequence) {
  if (!trigger || !trigger->owner()) {
    DBUG_PRINT("cu_recovery", ("checkpoint skipped: no trigger/owner"));
    return false;
  }
  if (m_recovery_required.load(std::memory_order_acquire)) {
    DBUG_PRINT("cu_recovery", ("checkpoint skipped: recovery required"));
    return false;
  }

  std::lock_guard checkpoint_guard(m_checkpoint_mutex);
  const fs::path snap_base = m_partition_dir / "snapshots";
  const fs::path ckpt_base = m_partition_dir / "checkpoints";
  if (!Recovery::DurableFileSystem::create_directories(snap_base)) return false;
  if (!Recovery::DurableFileSystem::create_directories(ckpt_base)) return false;

  uint64_t gen = latest_generation() + 1;
  for (;;) {
    std::error_code ec;
    const bool snap_exists = fs::exists(snap_base / ("checkpoint-" + std::to_string(gen)), ec);
    if (ec) return false;
    const bool manifest_exists = fs::exists(manifest_path(gen), ec);
    if (ec) return false;
    if (!snap_exists && !manifest_exists) break;
    ++gen;
  }

  const fs::path tmp_dir = snap_base / ("checkpoint-" + std::to_string(gen) + ".tmp");
  const fs::path final_dir = snap_base / ("checkpoint-" + std::to_string(gen));
  {
    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
  }
  if (!Recovery::DurableFileSystem::create_directories(tmp_dir)) return false;

  // Directory durability and generation allocation never hold the TP gate.
  std::unique_lock notification_guard(m_notifications.mutex());
  if (!m_notifications.quiescent() || !m_notifications.position().valid()) return false;
  auto *owner = trigger->owner();

  std::shared_lock table_list_lock(owner->m_table_mutex);
  auto imcus = owner->m_imcus;
  if (imcus.empty()) {
    DBUG_PRINT("cu_recovery", ("checkpoint skipped: no IMCUs"));
    return false;
  }

  std::sort(imcus.begin(), imcus.end(),
            [](const auto &a, const auto &b) { return a->get_imcu_id() < b->get_imcu_id(); });
  std::vector<std::unique_lock<std::shared_mutex>> freeze_locks;
  freeze_locks.reserve(imcus.size());
  for (const auto &im : imcus)
    if (im) freeze_locks.emplace_back(im->mutation_mutex());

  // A snapshot is only safe at a quiescent point. An uncommitted row's image is
  // already in the CUs, and the journal marking it uncommitted is not
  // serialized, so a restore would publish it as fully visible while InnoDB
  // rolls it away. Publish no generation instead; the next sweep retries.
  for (const auto &im : imcus) {
    if (im && im->has_uncommitted_changes()) {
      DBUG_PRINT("cu_recovery", ("checkpoint refused: IMCU %u has uncommitted changes", im->get_imcu_id()));
      return false;
    }
  }

  const uint64_t boundary = m_notifications.sequence();
  if (snapshot_sequence && snapshot_sequence != boundary) return false;
  auto position = m_notifications.position();
  // The cut is now fixed: all IMCUs are frozen and no active/pending source
  // notification preceded this boundary. Later producers may enqueue while
  // serialization runs; their mutations wait on the IMCU/table locks and are
  // replayed after this checkpoint's captured binlog position.
  notification_guard.unlock();
  RecoveryManifest manifest;
  manifest.table_id = owner->meta().table_id;
  manifest.generation = gen;
  manifest.schema_fingerprint = compute_schema_fingerprint(owner->meta());
  manifest.binlog = position;

  std::vector<std::pair<fs::path, std::string>> staged_snapshots;
  std::vector<fs::path> snap_files;
  snap_files.reserve(imcus.size());
  for (const auto &im : imcus) {
    if (!im) continue;
    std::string snap_data;
    if (!serialize_imcu(im.get(), boundary, snap_data)) {
      Recovery::DurableFileSystem::remove_directory(tmp_dir);
      return false;
    }

    const uint32_t imcu_id = im->get_imcu_id();
    const fs::path snap_file = tmp_dir / ("imcu_" + std::to_string(imcu_id) + ".snap");
    snap_files.push_back(snap_file);

    ManifestImcuEntry e;
    e.imcu_id = imcu_id;
    e.state = ManifestImcuState::CHECKPOINTED;
    e.snapshot_sequence = boundary;
    e.snapshot_size = snap_data.size();
    e.snapshot_crc = Utils::crc32c_compute(snap_data.data(), snap_data.size(), 0);
    e.snapshot_file = "checkpoint-" + std::to_string(gen) + "/imcu_" + std::to_string(imcu_id) + ".snap";
    manifest.imcus.push_back(std::move(e));
    staged_snapshots.emplace_back(snap_file, std::move(snap_data));
  }

  // The freeze is over: the snapshot bytes are fixed, so the per-file flushes --
  // the expensive part, one fdatasync per IMCU -- run with no IMCU lock held.
  freeze_locks.clear();
  table_list_lock.unlock();
  if (!Recovery::BinlogRecovery::certify(position)) {
    Recovery::DurableFileSystem::remove_directory(tmp_dir);
    return false;
  }
  manifest.binlog = position;
  for (const auto &[path, data] : staged_snapshots) {
    if (!Recovery::DurableFileSystem::write_file_buffered(path, data)) {
      Recovery::DurableFileSystem::remove_directory(tmp_dir);
      return false;
    }
  }
  staged_snapshots.clear();

  for (const auto &snap_file : snap_files) {
    if (!Recovery::DurableFileSystem::sync_file(snap_file)) {
      Recovery::DurableFileSystem::remove_directory(tmp_dir);
      return false;
    }
  }

  // All snapshot files are durable → fsync the generation dir, then atomically
  // rename it into place.  Only after that does the manifest get published.
  if (!Recovery::DurableFileSystem::sync_directory(tmp_dir)) {
    Recovery::DurableFileSystem::remove_directory(tmp_dir);
    return false;
  }
  if (!Recovery::DurableFileSystem::rename(tmp_dir, final_dir)) {
    Recovery::DurableFileSystem::remove_directory(tmp_dir);
    return false;
  }

  bool publish_manifest = true;
  DBUG_EXECUTE_IF("rapid_checkpoint_publish_error", { publish_manifest = false; });
  if (!publish_manifest || !persist_manifest(manifest)) {
    DBUG_PRINT("cu_recovery", ("checkpoint: manifest persist failed"));
    Recovery::DurableFileSystem::remove_directory(final_dir);
    return false;
  }

  if (!m_store->publish(m_db_name, m_tbl_name, gen, m_partition_dir)) {
    remove_generation(gen);
    return false;
  }
  {
    std::lock_guard gate(m_notifications.mutex());
    if (m_recovery_required.load() || m_notifications.disabled()) {
      (void)mark_recovery_taint();
      remove_generation(gen);
      return false;
    }
    m_notifications.checkpoint_published(boundary);
  }
  gc_old_generations();
  DBUG_PRINT("cu_recovery", ("checkpoint generation %llu committed (boundary=%llu)", (unsigned long long)gen,
                             (unsigned long long)boundary));
  return true;
}

void TablePersistenceManager::gc_old_generations() {
  auto gens = list_manifest_generations();  // ascending
  if (gens.size() > kMaxRetainedGenerations) {
    const size_t drop = gens.size() - kMaxRetainedGenerations;
    for (size_t i = 0; i < drop; ++i) remove_generation(gens[i]);
    gens.erase(gens.begin(), gens.begin() + static_cast<std::ptrdiff_t>(drop));
  }

  // A checkpoint that failed after its directory was renamed into place (or a crash
  // in that window) leaves checkpoint-<N>/ with no manifest, and interrupted ones
  // leave checkpoint-<N>.tmp/. Nothing else collects them, and each is a full copy
  // of the table. Runs under m_checkpoint_mutex, so no generation is in flight.
  const std::unordered_set<uint64_t> live(gens.begin(), gens.end());
  const fs::path snap_base = m_partition_dir / "snapshots";
  static const std::string kPrefix = "checkpoint-";
  std::vector<fs::path> orphans;
  std::error_code ec;
  for (fs::directory_iterator it(snap_base, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.compare(0, kPrefix.size(), kPrefix) != 0) continue;
    const bool is_tmp = name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0;
    const std::string digits = name.substr(kPrefix.size(), name.size() - kPrefix.size() - (is_tmp ? 4 : 0));
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); }))
      continue;
    if (is_tmp || live.count(std::strtoull(digits.c_str(), nullptr, 10)) == 0) orphans.push_back(it->path());
  }
  if (orphans.empty()) return;
  for (const auto &path : orphans) {
    std::error_code rm;
    fs::remove_all(path, rm);
  }
  (void)Recovery::DurableFileSystem::sync_directory(snap_base / "checkpoint-orphan");  // fsyncs snap_base
}

struct MemStreamBuf : std::streambuf {
  MemStreamBuf(const char *data, size_t size) {
    char *p = const_cast<char *>(data);
    setg(p, p, p + size);
  }
};

Result<uint64_t> TablePersistenceManager::load_snapshot(Imcu *imcu, uint64_t generation) {
  if (!imcu) return {ErrorCode::INTERNAL, 0};

  uint32_t imcu_id = imcu->get_imcu_id();
  fs::path path = snap_path(generation, imcu_id);

  std::ifstream snap(path, std::ios::binary);
  if (!snap.is_open()) {
    // No snapshot file — cold start requiring primary reload.
    return {ErrorCode::NOT_FOUND, 0};
  }

  uint32_t snap_imcu_id = 0, col_count = 0;
  uint64_t snap_lsn = 0;
  if (!read_snap_header(snap, snap_imcu_id, col_count, snap_lsn)) {
    DBUG_PRINT("cu_recovery", ("snapshot header corrupt: %s", path.string().c_str()));
    return {ErrorCode::CORRUPTION, 0};
  }
  if (snap_imcu_id != imcu_id) {
    DBUG_PRINT("cu_recovery", ("snapshot IMCU id mismatch: file=%u expected=%u", snap_imcu_id, imcu_id));
    return {ErrorCode::CORRUPTION, 0};
  }

  if (!read_imcu_metadata(snap, imcu)) {
    DBUG_PRINT("cu_recovery", ("snapshot IMCU metadata corrupt: %s", path.string().c_str()));
    return {ErrorCode::CORRUPTION, 0};
  }

  uint32_t actual_cols = static_cast<uint32_t>(imcu->get_column_count());
  // A snapshot written under a different field count cannot be partially
  // restored: min(col_count) would silently produce column-incomplete rows.
  if (col_count != actual_cols) {
    DBUG_PRINT("cu_recovery", ("snapshot col_count=%u != actual=%u — conflict", col_count, actual_cols));
    return {ErrorCode::CONFLICT, 0};
  }

  for (uint32_t c = 0; c < col_count; ++c) {
    uint64_t cu_size = 0;
    if (!snap.read(reinterpret_cast<char *>(&cu_size), sizeof(cu_size))) {
      return {ErrorCode::CORRUPTION, 0};  // truncated
    }

    if (cu_size == 0) continue;  // placeholder (non-secondary column, no CU)

    // Bound the allocation BEFORE trusting the persisted length.
    if (cu_size > MAX_CU_SNAPSHOT_SIZE) {
      DBUG_PRINT("cu_recovery", ("snapshot CU %u size %llu exceeds cap", c, (unsigned long long)cu_size));
      return {ErrorCode::CORRUPTION, 0};
    }

    auto *cu = imcu->get_cu(c);
    if (!cu) {
      DBUG_PRINT("cu_recovery", ("snapshot CU %u has data but no live CU — conflict", c));
      return {ErrorCode::CONFLICT, 0};
    }

    // Read exactly cu_size bytes into a buffer, then wrap in istringstream.
    std::vector<char> cu_buf(cu_size);
    if (!snap.read(cu_buf.data(), static_cast<std::streamsize>(cu_size))) {
      return {ErrorCode::CORRUPTION, 0};
    }

    MemStreamBuf msb(cu_buf.data(), cu_size);
    std::istream cu_in(&msb);
    int deser_ret = cu->deserialize(cu_in);
    if (deser_ret != ShannonBase::SHANNON_SUCCESS) {
      DBUG_PRINT("cu_recovery", ("CU %u deserialize failed", c));
      return {ErrorCode::CORRUPTION, 0};
    }
  }

  DBUG_PRINT("cu_recovery",
             ("loaded snapshot for IMCU %u  snap_lsn=%llu  cols=%u", imcu_id, (unsigned long long)snap_lsn, col_count));
  return {ErrorCode::OK, snap_lsn};
}

Result<size_t> TablePersistenceManager::recover(const std::vector<Imcu *> &imcus, uint64_t *restored_generation) {
  if (imcus.empty()) return {ErrorCode::OK, 0};

  // Select the newest generation whose manifest AND referenced snapshot files
  // form a complete, valid checkpoint.  Fall back through older generations on
  // any corruption / missing file — never mix two generations.
  uint64_t generation = 0;
  RecoveryManifest manifest;
  bool has_manifest = false;
  {
    const auto gens = list_manifest_generations();  // ascending
    for (auto it = gens.rbegin(); it != gens.rend(); ++it) {
      auto mres = load_manifest(*it);
      if (!mres.ok()) continue;  // corrupt manifest → try older generation
      if (mres.value.generation != *it) continue;

      bool generation_valid = true;
      for (const auto &e : mres.value.imcus) {
        if (e.state != ManifestImcuState::CHECKPOINTED) continue;
        std::ifstream snap(snap_path(*it, e.imcu_id), std::ios::binary);
        if (!snap.is_open()) {
          generation_valid = false;
          break;
        }
        std::error_code size_ec;
        const uint64_t actual_size = fs::file_size(snap_path(*it, e.imcu_id), size_ec);
        if (size_ec || actual_size != e.snapshot_size) {
          generation_valid = false;
          break;
        }

        uint32_t file_crc = 0;
        char crc_buf[64 * 1024];
        while (snap.good()) {
          snap.read(crc_buf, sizeof(crc_buf));
          const std::streamsize n = snap.gcount();
          if (n > 0) file_crc = Utils::crc32c_compute(crc_buf, static_cast<size_t>(n), file_crc);
        }
        if (!snap.eof() || file_crc != e.snapshot_crc) {
          generation_valid = false;
          break;
        }

        snap.clear();
        snap.seekg(0, std::ios::beg);
        uint32_t snap_imcu_id = 0, col_count = 0;
        uint64_t snap_lsn = 0;
        if (!read_snap_header(snap, snap_imcu_id, col_count, snap_lsn) || snap_imcu_id != e.imcu_id ||
            snap_lsn != e.snapshot_sequence) {
          generation_valid = false;
          break;
        }
      }
      if (!generation_valid) continue;  // incomplete generation → fallback

      generation = *it;
      manifest = std::move(mres.value);
      has_manifest = true;
      break;
    }
  }

  if (!has_manifest) return {ErrorCode::NOT_FOUND, 0};
  if (has_manifest) {
    uint64_t current_table_id = 0;
    uint64_t current_fp = 0;
    for (Imcu *imcu : imcus) {
      if (imcu && imcu->owner()) {
        current_table_id = imcu->owner()->meta().table_id;
        current_fp = compute_schema_fingerprint(imcu->owner()->meta());
        break;
      }
    }
    if (manifest.table_id != 0 && current_table_id != 0 && manifest.table_id != current_table_id) {
      DBUG_PRINT("cu_recovery", ("table id mismatch — recovery aborted (CONFLICT)"));
      return {ErrorCode::CONFLICT, 0};
    }
    if (manifest.schema_fingerprint != 0 && current_fp != 0 && current_fp != manifest.schema_fingerprint) {
      DBUG_PRINT("cu_recovery", ("schema fingerprint mismatch — recovery aborted (CONFLICT)"));
      return {ErrorCode::CONFLICT, 0};
    }
  }

  // Phase 1: load snapshots, record per-IMCU checkpoint LSN
  // imcu_id → checkpoint LSN (0 if no snapshot found)
  std::unordered_map<uint32_t, uint64_t> checkpoint_lsn;

  for (Imcu *imcu : imcus) {
    if (!imcu) continue;
    uint32_t iid = imcu->get_imcu_id();
    auto snap_result = load_snapshot(imcu, generation);

    if (snap_result.error == ErrorCode::CORRUPTION || snap_result.error == ErrorCode::IO_ERROR ||
        snap_result.error == ErrorCode::CONFLICT) {
      DBUG_PRINT("cu_recovery", ("IMCU %u snapshot load failed — recovery aborted", iid));
      return {snap_result.error, 0};
    }

    if (snap_result.error == ErrorCode::NOT_FOUND && has_manifest) {
      // A manifest that declares this IMCU CHECKPOINTED but whose snapshot file
      // is missing is corruption, NOT a legitimate cold start.
      for (const auto &e : manifest.imcus) {
        if (e.imcu_id == iid && e.state == ManifestImcuState::CHECKPOINTED) {
          DBUG_PRINT("cu_recovery", ("IMCU %u snapshot missing but manifest says CHECKPOINTED — corruption", iid));
          return {ErrorCode::CORRUPTION, 0};
        }
      }
    }

    uint64_t lsn = snap_result.ok() ? snap_result.value : 0;
    checkpoint_lsn[iid] = lsn;
    DBUG_PRINT("cu_recovery", ("IMCU %u checkpoint_lsn=%llu", iid, (unsigned long long)lsn));
  }

  if (restored_generation) *restored_generation = generation;
  return {ErrorCode::OK, 0};
}

}  // namespace Imcs
}  // namespace ShannonBase
