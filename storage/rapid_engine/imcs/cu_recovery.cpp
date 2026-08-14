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
#include "storage/rapid_engine/imcs/cu_recovery.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#ifndef _WIN32
#include <fcntl.h>   // O_WRONLY
#include <unistd.h>  // fdatasync, close
#endif

#include "storage/innobase/include/ut0dbg.h"  // DBUG_PRINT, UNIV_SQL_NULL
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/utils/crc.h"

namespace ShannonBase {
namespace Imcs {

namespace fs = std::filesystem;

namespace {
template <typename T>
void append_pod(std::string &out, const T &v) {
  out.append(reinterpret_cast<const char *>(&v), sizeof(T));
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

uint32_t compute_operation_crc(const std::vector<WalCell> &cells) {
  uint32_t crc = 0;
  for (const auto &cell : cells) {
    crc = Utils::crc32c_compute(&cell.col_id, sizeof(cell.col_id), crc);
    const uint8_t is_null = cell.is_null ? 1u : 0u;
    crc = Utils::crc32c_compute(&is_null, sizeof(is_null), crc);
    const uint64_t len = cell.is_null ? 0 : cell.value.size();
    crc = Utils::crc32c_compute(&len, sizeof(len), crc);
    if (!cell.is_null && !cell.value.empty()) crc = Utils::crc32c_compute(cell.value.data(), cell.value.size(), crc);
  }
  return crc;
}
}  // namespace

CURecoveryManager::CURecoveryManager(const std::string &data_dir, const std::string &db_name,
                                     const std::string &tbl_name)
    : m_db_name(db_name), m_tbl_name(tbl_name) {
  m_partition_dir = fs::path(data_dir) / db_name / tbl_name;
  m_wal_path = m_partition_dir / "cu_wal.log";
}

CURecoveryManager::~CURecoveryManager() { close(); }

bool CURecoveryManager::open() {
  std::lock_guard lock(m_wal_mutex);

  // Ensure directory exists.
  std::error_code ec;
  fs::create_directories(m_partition_dir, ec);
  if (ec) {
    DBUG_PRINT("cu_recovery", ("mkdir failed: %s — %s", m_partition_dir.string().c_str(), ec.message().c_str()));
    return false;
  }

  uint64_t max_lsn = 0;
  {
    std::ifstream wal_in(m_wal_path, std::ios::binary);
    if (wal_in.is_open()) {
      WalRecord rec;
      WalReadStatus status;
      std::streamoff last_good_offset = 0;
      while ((status = read_record(wal_in, rec)) == WalReadStatus::OK) {
        if (rec.lsn > max_lsn) max_lsn = rec.lsn;
        last_good_offset = wal_in.tellg();
      }
      if (status == WalReadStatus::BAD_MAGIC || status == WalReadStatus::CRC_MISMATCH ||
          status == WalReadStatus::IO_ERROR) {
        DBUG_PRINT("cu_recovery", ("WAL corruption detected at %s — open failed", m_wal_path.string().c_str()));
        return false;
      }
      if (status == WalReadStatus::TRUNCATED_TAIL) {
        wal_in.close();
#ifndef _WIN32
        int fd = ::open(m_wal_path.c_str(), O_WRONLY);
        if (fd < 0) return false;
        bool ok = (::ftruncate(fd, static_cast<off_t>(last_good_offset)) == 0);
        if (ok) ok = (::fsync(fd) == 0);
        const int saved_errno = errno;
        ::close(fd);
        errno = saved_errno;
        if (!ok) {
          DBUG_PRINT("cu_recovery", ("WAL torn-tail truncate failed at %s", m_wal_path.string().c_str()));
          return false;
        }
        DBUG_PRINT("cu_recovery", ("WAL torn tail truncated at %s (offset %lld)", m_wal_path.string().c_str(),
                                   (long long)last_good_offset));
#else
        return false;  // unsupported: must repair torn tail before appending
#endif
      }
    }
  }

  // Open WAL in append mode (creates if absent).
  if (!m_wal_file.open(m_wal_path, /*append=*/true)) return false;

  if (max_lsn >= m_written_lsn.load()) m_written_lsn.store(max_lsn + 1);
  m_last_appended_lsn = max_lsn;
  m_durable_lsn.store(0);
  m_applied_lsn.store(0);

  DBUG_PRINT("cu_recovery", ("WAL opened at %s  next_lsn=%llu", m_wal_path.string().c_str(),
                             (unsigned long long)m_written_lsn.load()));
  return true;
}

void CURecoveryManager::close_locked() {
  // caller has already acquired m_wal_mutex
  m_wal_file.close();
}
void CURecoveryManager::close() {
  std::lock_guard lock(m_wal_mutex);
  m_wal_file.close();
}

bool CURecoveryManager::sync() {
  std::lock_guard lock(m_wal_mutex);
  if (!m_wal_file.is_open()) return false;
  if (!m_wal_file.flush_data()) return false;
  const uint64_t durable = m_durable_lsn.load(std::memory_order_relaxed);
  m_durable_lsn.store(std::max(durable, m_last_appended_lsn), std::memory_order_release);
  return true;
}

//  WAL record encoding
//
//  Record layout (all multi-byte values host byte order / LE):
//
//   [magic   4 B]  WAL_MAGIC
//   [lsn     8 B]
//   [op_type 1 B]
//   [imcu_id 4 B]
//   [col_id  4 B]
//   [row_id  8 B]
//   [txn_id  8 B]
//   [scn     8 B]
//   [val_len 8 B]
//   [val_data N B]  (absent when val_len == 0 or UNIV_SQL_NULL)
//   [crc32   4 B]  of all preceding bytes
//
//  Total fixed overhead: 4+8+1+4+4+8+8+8+8+4 = 57 bytes.

std::vector<uint8_t> CURecoveryManager::encode_record(const WalRecord &rec) const {
  std::vector<uint8_t> buf;
  buf.reserve(64 + (rec.cells.empty() ? rec.val_data.size() : 0));

  auto push = [&](const void *p, size_t n) {
    const auto *b = static_cast<const uint8_t *>(p);
    buf.insert(buf.end(), b, b + n);
  };

  auto push_u8 = [&](uint8_t v) { push(&v, 1); };
  auto push_u32 = [&](uint32_t v) { push(&v, 4); };
  auto push_u64 = [&](uint64_t v) { push(&v, 8); };

  push_u32(WAL_MAGIC);
  push_u64(rec.lsn);
  push_u8(static_cast<uint8_t>(rec.op_type));

  switch (rec.op_type) {
    case WalOpType::ROW_PREPARE: {
      push_u64(rec.op_id);
      push_u32(rec.imcu_id);
      push_u64(rec.row_id);
      push_u64(rec.txn_id);
      push_u64(rec.scn);
      push_u8(rec.mut_type);
      push_u32(static_cast<uint32_t>(rec.cells.size()));
      for (const auto &cell : rec.cells) {
        push_u32(cell.col_id);
        push_u8(cell.is_null ? 1u : 0u);
        const uint64_t cell_len = (cell.is_null || cell.value.empty()) ? 0 : cell.value.size();
        push_u64(cell_len);
        if (cell_len > 0) push(cell.value.data(), cell.value.size());
      }
    } break;
    case WalOpType::ROW_COMMIT: {
      push_u64(rec.op_id);
      push_u32(rec.imcu_id);
      push_u64(rec.commit_lsn);
      push_u32(rec.redo_count);
      push_u32(rec.operation_crc);
    } break;
    default: {  // legacy single-cell record
      push_u32(rec.imcu_id);
      push_u32(rec.col_id);
      push_u64(rec.row_id);
      push_u64(rec.txn_id);
      push_u64(rec.scn);
      push_u64(static_cast<uint64_t>(rec.val_len));
      const size_t data_bytes = (rec.val_len != UNIV_SQL_NULL && rec.val_len > 0) ? rec.val_len : 0;
      if (data_bytes > 0) push(rec.val_data.data(), data_bytes);
    } break;
  }

  uint32_t chk = Utils::crc32c_compute(buf.data(), buf.size(), 0);
  push_u32(chk);

  return buf;
}

WalReadStatus CURecoveryManager::read_record(std::istream &in, WalRecord &rec) const {
  uint32_t running_crc = 0;

  uint32_t magic = 0;
  in.read(reinterpret_cast<char *>(&magic), sizeof(magic));
  if (!in) {
    if (in.bad()) return WalReadStatus::IO_ERROR;
    return (in.gcount() == 0) ? WalReadStatus::EOF_REACHED : WalReadStatus::TRUNCATED_TAIL;
  }
  running_crc = Utils::crc32c_compute(&magic, sizeof(magic), running_crc);
  if (magic != WAL_MAGIC) return WalReadStatus::BAD_MAGIC;

  auto rd = [&](void *p, size_t n) -> WalReadStatus {
    in.read(reinterpret_cast<char *>(p), static_cast<std::streamsize>(n));
    if (in) {
      running_crc = Utils::crc32c_compute(p, n, running_crc);
      return WalReadStatus::OK;
    }
    if (in.bad()) return WalReadStatus::IO_ERROR;
    return WalReadStatus::TRUNCATED_TAIL;
  };

  WalReadStatus status = WalReadStatus::OK;

  uint64_t lsn = 0;
  if ((status = rd(&lsn, 8)) != WalReadStatus::OK) return status;
  uint8_t op = 0;
  if ((status = rd(&op, 1)) != WalReadStatus::OK) return status;

  rec = WalRecord{};
  rec.lsn = lsn;
  rec.op_type = static_cast<WalOpType>(op);

  switch (rec.op_type) {
    case WalOpType::ROW_PREPARE: {
      uint64_t op_id = 0, row_id = 0, txn_id = 0, scn = 0;
      uint32_t imcu_id = 0, col_count = 0;
      uint8_t mut_type = 0;
      if ((status = rd(&op_id, 8)) != WalReadStatus::OK) return status;
      if ((status = rd(&imcu_id, 4)) != WalReadStatus::OK) return status;
      if ((status = rd(&row_id, 8)) != WalReadStatus::OK) return status;
      if ((status = rd(&txn_id, 8)) != WalReadStatus::OK) return status;
      if ((status = rd(&scn, 8)) != WalReadStatus::OK) return status;
      if ((status = rd(&mut_type, 1)) != WalReadStatus::OK) return status;
      if ((status = rd(&col_count, 4)) != WalReadStatus::OK) return status;
      if (col_count > MAX_WAL_COLUMN_COUNT) return WalReadStatus::BAD_MAGIC;

      rec.op_id = op_id;
      rec.imcu_id = imcu_id;
      rec.row_id = row_id;
      rec.txn_id = txn_id;
      rec.scn = scn;
      rec.mut_type = mut_type;
      rec.cells.reserve(col_count);

      for (uint32_t i = 0; i < col_count; ++i) {
        WalCell cell;
        uint32_t cid = 0;
        uint8_t is_null = 0;
        uint64_t vl = 0;
        if ((status = rd(&cid, 4)) != WalReadStatus::OK) return status;
        if ((status = rd(&is_null, 1)) != WalReadStatus::OK) return status;
        if ((status = rd(&vl, 8)) != WalReadStatus::OK) return status;
        if (is_null && vl != 0) return WalReadStatus::BAD_MAGIC;
        if (vl > MAX_WAL_VALUE_SIZE) return WalReadStatus::BAD_MAGIC;
        cell.col_id = cid;
        cell.is_null = (is_null != 0);
        if (!cell.is_null && vl > 0) {
          cell.value.resize(static_cast<size_t>(vl));
          if ((status = rd(cell.value.data(), cell.value.size())) != WalReadStatus::OK) return status;
        }
        rec.cells.push_back(std::move(cell));
      }
    } break;

    case WalOpType::ROW_COMMIT: {
      uint64_t op_id = 0;
      uint32_t imcu_id = 0;
      if ((status = rd(&op_id, 8)) != WalReadStatus::OK) return status;
      if ((status = rd(&imcu_id, 4)) != WalReadStatus::OK) return status;
      if ((status = rd(&rec.commit_lsn, 8)) != WalReadStatus::OK) return status;
      if ((status = rd(&rec.redo_count, 4)) != WalReadStatus::OK) return status;
      if ((status = rd(&rec.operation_crc, 4)) != WalReadStatus::OK) return status;
      rec.op_id = op_id;
      rec.imcu_id = imcu_id;
    } break;

    case WalOpType::INSERT:
    case WalOpType::UPDATE:
    case WalOpType::DELETE:
    case WalOpType::NULL_INSERT:
    case WalOpType::NULL_UPDATE: {
      uint32_t iid = 0, cid = 0;
      uint64_t rid = 0, tid = 0, scn = 0, vl = 0;
      if ((status = rd(&iid, 4)) != WalReadStatus::OK) return status;
      if ((status = rd(&cid, 4)) != WalReadStatus::OK) return status;
      if ((status = rd(&rid, 8)) != WalReadStatus::OK) return status;
      if ((status = rd(&tid, 8)) != WalReadStatus::OK) return status;
      if ((status = rd(&scn, 8)) != WalReadStatus::OK) return status;
      if ((status = rd(&vl, 8)) != WalReadStatus::OK) return status;
      if (vl != UNIV_SQL_NULL && vl > MAX_WAL_VALUE_SIZE) return WalReadStatus::BAD_MAGIC;

      rec.imcu_id = iid;
      rec.col_id = cid;
      rec.row_id = rid;
      rec.txn_id = tid;
      rec.scn = scn;
      rec.val_len = static_cast<size_t>(vl);

      const size_t data_bytes = (vl != UNIV_SQL_NULL && vl > 0) ? static_cast<size_t>(vl) : 0;
      if (data_bytes > 0) {
        rec.val_data.resize(data_bytes);
        if ((status = rd(rec.val_data.data(), data_bytes)) != WalReadStatus::OK) return status;
      }
    } break;

    default:
      return WalReadStatus::BAD_MAGIC;
  }

  uint32_t stored_crc = 0;
  if (!in.read(reinterpret_cast<char *>(&stored_crc), sizeof(stored_crc))) {
    if (in.bad()) return WalReadStatus::IO_ERROR;
    return WalReadStatus::TRUNCATED_TAIL;
  }

  if (running_crc != stored_crc) {
    DBUG_PRINT("cu_recovery", ("WAL CRC mismatch at LSN %llu", (unsigned long long)lsn));
    return WalReadStatus::CRC_MISMATCH;
  }

  return WalReadStatus::OK;
}

bool CURecoveryManager::append_record(WalRecord &rec) {
  std::lock_guard lock(m_wal_mutex);
  if (!m_wal_file.is_open()) return false;
  if (m_recovery_required.load(std::memory_order_acquire)) return false;

  rec.lsn = m_written_lsn.fetch_add(1, std::memory_order_relaxed);
  if (rec.op_type == WalOpType::ROW_PREPARE) rec.op_id = rec.lsn;
  if (rec.op_type == WalOpType::ROW_COMMIT) rec.commit_lsn = rec.lsn;

  auto buf = encode_record(rec);
  if (!m_wal_file.write(buf.data(), buf.size())) {
    m_recovery_required.store(true, std::memory_order_release);
    return false;
  }
  m_last_appended_lsn = rec.lsn;
  return true;
}

bool CURecoveryManager::log_write(uint32_t imcu_id, uint32_t col_id, uint64_t row_id, uint64_t txn_id, uint64_t scn,
                                  const uint8_t *val_data, size_t val_len) {
  WalRecord rec;
  rec.op_type = (val_len == UNIV_SQL_NULL) ? WalOpType::NULL_INSERT : WalOpType::INSERT;
  rec.imcu_id = imcu_id;
  rec.col_id = col_id;
  rec.row_id = row_id;
  rec.txn_id = txn_id;
  rec.scn = scn;
  rec.val_len = val_len;
  if (val_len != UNIV_SQL_NULL && val_len > 0 && val_data) rec.val_data.assign(val_data, val_data + val_len);

  return append_record(rec);
}

bool CURecoveryManager::log_update(uint32_t imcu_id, uint32_t col_id, uint64_t row_id, uint64_t txn_id, uint64_t scn,
                                   const uint8_t *new_val, size_t val_len) {
  WalRecord rec;
  rec.op_type = (val_len == UNIV_SQL_NULL) ? WalOpType::NULL_UPDATE : WalOpType::UPDATE;
  rec.imcu_id = imcu_id;
  rec.col_id = col_id;
  rec.row_id = row_id;
  rec.txn_id = txn_id;
  rec.scn = scn;
  rec.val_len = val_len;
  if (val_len != UNIV_SQL_NULL && val_len > 0 && new_val) rec.val_data.assign(new_val, new_val + val_len);
  return append_record(rec);
}

uint64_t CURecoveryManager::log_delete(uint32_t imcu_id, uint32_t col_id, uint64_t row_id, uint64_t txn_id,
                                       uint64_t scn) {
  WalRecord rec;
  rec.op_type = WalOpType::DELETE;
  rec.imcu_id = imcu_id;
  rec.col_id = col_id;
  rec.row_id = row_id;
  rec.txn_id = txn_id;
  rec.scn = scn;
  rec.val_len = 0;
  return append_record(rec) ? rec.lsn : 0;
}

uint64_t CURecoveryManager::log_row_prepare(uint32_t imcu_id, uint64_t row_id, uint64_t txn_id, uint64_t scn,
                                            uint8_t mut_type, const std::vector<WalCell> &cells,
                                            uint32_t *out_operation_crc) {
  WalRecord rec;
  rec.op_type = WalOpType::ROW_PREPARE;
  rec.imcu_id = imcu_id;
  rec.row_id = row_id;
  rec.txn_id = txn_id;
  rec.scn = scn;
  rec.mut_type = mut_type;
  rec.cells = cells;
  if (out_operation_crc) *out_operation_crc = compute_operation_crc(cells);
  return append_record(rec) ? rec.op_id : 0;
}

uint64_t CURecoveryManager::log_row_commit(uint64_t op_id, uint32_t imcu_id, uint32_t redo_count,
                                           uint32_t operation_crc) {
  WalRecord rec;
  rec.op_type = WalOpType::ROW_COMMIT;
  rec.op_id = op_id;
  rec.imcu_id = imcu_id;
  rec.redo_count = redo_count;
  rec.operation_crc = operation_crc;
  if (!append_record(rec)) return 0;  // append outcome is fail-stop; recovery_required may be set
  if (!sync()) {
    // COMMIT_OUTCOME_UNKNOWN: the commit record may or may not have reached
    // stable storage.  Do NOT report a clean failure — enter recovery-required.
    m_recovery_required.store(true, std::memory_order_release);
    return 0;
  }
  return rec.lsn;
}

fs::path CURecoveryManager::snap_path(uint64_t generation, uint32_t imcu_id) const {
  std::ostringstream ss;
  ss << "checkpoint-" << generation << "/imcu_" << imcu_id << ".snap";
  return m_partition_dir / "snapshots" / ss.str();
}

fs::path CURecoveryManager::manifest_path(uint64_t generation) const {
  std::ostringstream ss;
  ss << "checkpoint-" << generation << ".manifest";
  return m_partition_dir / "checkpoints" / ss.str();
}

uint64_t CURecoveryManager::latest_generation() const {
  const auto gens = list_manifest_generations();
  return gens.empty() ? 0 : gens.back();
}

std::vector<uint64_t> CURecoveryManager::list_manifest_generations() const {
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

void CURecoveryManager::remove_generation(uint64_t generation) {
  const std::string gen_dir = "checkpoint-" + std::to_string(generation);
  Recovery::DurableFileSystem::remove_directory(m_partition_dir / "snapshots" / gen_dir);
  std::error_code ec;
  fs::remove(m_partition_dir / "checkpoints" / (gen_dir + ".manifest"), ec);
  if (!ec) Recovery::DurableFileSystem::sync_directory(m_partition_dir / "checkpoints");
}

bool CURecoveryManager::persist_manifest(const RecoveryManifest &manifest) {
  std::string out;
  out.reserve(64 + manifest.imcus.size() * 96);
  append_pod(out, MANIFEST_MAGIC);
  append_pod(out, MANIFEST_FORMAT_VER);
  append_pod(out, manifest.table_id);
  append_pod(out, manifest.generation);
  append_pod(out, manifest.schema_fingerprint);
  append_pod(out, manifest.wal_base_lsn);
  append_pod(out, static_cast<uint32_t>(manifest.imcus.size()));
  for (const auto &e : manifest.imcus) {
    append_pod(out, e.imcu_id);
    append_pod(out, static_cast<uint8_t>(e.state));
    append_pod(out, e.snapshot_next_lsn);
    append_pod(out, e.snapshot_size);
    append_pod(out, e.snapshot_crc);
    append_str(out, e.snapshot_file);
  }
  const uint32_t crc = Utils::crc32c_compute(out.data(), out.size(), 0);
  append_pod(out, crc);
  return Recovery::DurableFileSystem::persist_file(manifest_path(manifest.generation), out);
}

Result<RecoveryManifest> CURecoveryManager::load_manifest(uint64_t generation) const {
  RecoveryManifest m;
  std::ifstream in(manifest_path(generation), std::ios::binary);
  if (!in.is_open()) return {ErrorCode::NOT_FOUND, m};

  in.seekg(0, std::ios::end);
  const std::streamoff file_size = in.tellg();
  if (file_size <= 0) return {ErrorCode::CORRUPTION, m};
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
  if (!r.read_pod(m.wal_base_lsn)) return {ErrorCode::CORRUPTION, m};

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
    if (!r.read_pod(e.snapshot_next_lsn)) return {ErrorCode::CORRUPTION, m};
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
bool CURecoveryManager::write_snap_header(std::ostream &out, uint32_t imcu_id, uint32_t col_count,
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

bool CURecoveryManager::read_snap_header(std::istream &in, uint32_t &imcu_id, uint32_t &col_count,
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

bool CURecoveryManager::write_imcu_metadata(std::ostream &out, const Imcu *imcu) const {
  write_pod(out, static_cast<uint64_t>(imcu->get_row_count()));  // current_rows
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

bool CURecoveryManager::read_imcu_metadata(std::istream &in, Imcu *imcu) const {
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

  return in.good();
}

bool CURecoveryManager::serialize_imcu(Imcu *imcu, uint64_t snapshot_next_lsn, std::string &out) const {
  const uint32_t imcu_id = imcu->get_imcu_id();
  const uint32_t col_count = static_cast<uint32_t>(imcu->get_column_count());

  std::ostringstream snap(std::ios::binary);
  if (!write_snap_header(snap, imcu_id, col_count, snapshot_next_lsn)) return false;
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

bool CURecoveryManager::checkpoint(Imcu *trigger, uint64_t snapshot_next_lsn) {
  if (!trigger || !trigger->owner()) return false;
  if (m_recovery_required.load(std::memory_order_acquire)) return false;

  std::lock_guard checkpoint_guard(m_checkpoint_mutex);
  auto *owner = trigger->owner();

  std::shared_lock table_list_lock(owner->m_table_mutex);
  auto imcus = owner->m_imcus;
  if (imcus.empty()) return false;

  std::sort(imcus.begin(), imcus.end(),
            [](const auto &a, const auto &b) { return a->get_imcu_id() < b->get_imcu_id(); });
  std::vector<std::unique_lock<std::shared_mutex>> freeze_locks;
  freeze_locks.reserve(imcus.size());
  for (const auto &im : imcus)
    if (im) freeze_locks.emplace_back(im->mutation_mutex());

  const uint64_t boundary = m_applied_lsn.load(std::memory_order_acquire) + 1;
  if (snapshot_next_lsn != 0 && snapshot_next_lsn != boundary) {
    DBUG_PRINT("cu_recovery", ("checkpoint: requested boundary %llu != safe boundary %llu",
                               (unsigned long long)snapshot_next_lsn, (unsigned long long)boundary));
    return false;
  }

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

  RecoveryManifest manifest;
  manifest.table_id = owner->meta().table_id;
  manifest.generation = gen;
  manifest.schema_fingerprint = compute_schema_fingerprint(owner->meta());

  for (const auto &im : imcus) {
    if (!im) continue;
    std::string snap_data;
    if (!serialize_imcu(im.get(), boundary, snap_data)) {
      Recovery::DurableFileSystem::remove_directory(tmp_dir);
      return false;
    }

    const uint32_t imcu_id = im->get_imcu_id();
    const fs::path snap_file = tmp_dir / ("imcu_" + std::to_string(imcu_id) + ".snap");
    if (!Recovery::DurableFileSystem::write_file(snap_file, snap_data)) {
      Recovery::DurableFileSystem::remove_directory(tmp_dir);
      return false;
    }

    ManifestImcuEntry e;
    e.imcu_id = imcu_id;
    e.state = ManifestImcuState::CHECKPOINTED;
    e.snapshot_next_lsn = boundary;
    e.snapshot_size = snap_data.size();
    e.snapshot_crc = Utils::crc32c_compute(snap_data.data(), snap_data.size(), 0);
    e.snapshot_file = "checkpoint-" + std::to_string(gen) + "/imcu_" + std::to_string(imcu_id) + ".snap";
    manifest.imcus.push_back(std::move(e));
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

  // Every IMCU was checkpointed at the same boundary, so it is a safe WAL GC
  // watermark.
  manifest.wal_base_lsn = boundary;

  if (!persist_manifest(manifest)) {
    DBUG_PRINT("cu_recovery", ("checkpoint: manifest persist failed"));
    return false;
  }

  gc_old_generations();
  DBUG_PRINT("cu_recovery", ("checkpoint generation %llu committed (boundary=%llu)", (unsigned long long)gen,
                             (unsigned long long)boundary));
  return true;
}

void CURecoveryManager::gc_old_generations() {
  auto gens = list_manifest_generations();  // ascending
  if (gens.size() <= kMaxRetainedGenerations) return;
  const size_t drop = gens.size() - kMaxRetainedGenerations;
  for (size_t i = 0; i < drop; ++i) remove_generation(gens[i]);
}

struct MemStreamBuf : std::streambuf {
  MemStreamBuf(const char *data, size_t size) {
    char *p = const_cast<char *>(data);
    setg(p, p, p + size);
  }
};

Result<uint64_t> CURecoveryManager::load_snapshot(Imcu *imcu, uint64_t generation) {
  if (!imcu) return {ErrorCode::INTERNAL, 0};

  uint32_t imcu_id = imcu->get_imcu_id();
  fs::path path = snap_path(generation, imcu_id);

  std::ifstream snap(path, std::ios::binary);
  if (!snap.is_open()) {
    // No snapshot file — legitimate cold start (replay from WAL genesis).
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

Result<size_t> CURecoveryManager::recover(const std::vector<Imcu *> &imcus,
                                          const std::function<ErrorCode(const WalRecord &)> &apply_fn) {
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
            snap_lsn != e.snapshot_next_lsn) {
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

  std::unordered_set<uint32_t> known_imcus;
  for (Imcu *imcu : imcus)
    if (imcu) known_imcus.insert(imcu->get_imcu_id());
  if (has_manifest)
    for (const auto &e : manifest.imcus) known_imcus.insert(e.imcu_id);

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

  // The snapshot boundary is a *next LSN*.  Even when WAL GC leaves an
  // empty file, a restart must never reuse LSNs below this boundary.
  uint64_t recovered_snapshot_next_lsn = has_manifest ? manifest.wal_base_lsn : 0;
  for (const auto &[iid, next_lsn] : checkpoint_lsn)
    recovered_snapshot_next_lsn = std::max(recovered_snapshot_next_lsn, next_lsn);

  auto publish_recovery_watermarks = [&](uint64_t max_seen_lsn, uint64_t max_committed_lsn) {
    const uint64_t snapshot_applied = recovered_snapshot_next_lsn > 0 ? recovered_snapshot_next_lsn - 1 : 0;
    const uint64_t next_lsn =
        std::max({m_written_lsn.load(std::memory_order_relaxed), max_seen_lsn + 1, recovered_snapshot_next_lsn});
    m_written_lsn.store(next_lsn, std::memory_order_release);
    // Every complete record found in the WAL survived a restart and is durable,
    // including an uncommitted ROW_PREPARE.  Applied only advances through a
    // checkpoint or a committed/replayed operation.
    m_durable_lsn.store(std::max(snapshot_applied, max_seen_lsn), std::memory_order_release);
    m_applied_lsn.store(std::max(snapshot_applied, max_committed_lsn), std::memory_order_release);
  };

  // Phase 2: replay WAL records past each IMCU's checkpoint LSN
  std::ifstream wal_in(m_wal_path, std::ios::binary);
  if (!wal_in.is_open()) {
    publish_recovery_watermarks(0, 0);
    DBUG_PRINT("cu_recovery", ("no WAL at %s — recovery complete (snapshot-only)", m_wal_path.string().c_str()));
    return {ErrorCode::OK, 0};
  }

  size_t replayed = 0;
  uint64_t max_seen_lsn = 0;
  uint64_t max_committed_lsn = 0;  // highest commit lsn actually replayed
  std::unordered_map<uint64_t, WalRecord> pending;
  WalRecord rec;
  WalReadStatus status;

  while ((status = read_record(wal_in, rec)) == WalReadStatus::OK) {
    if (rec.lsn > max_seen_lsn) max_seen_lsn = rec.lsn;

    if (rec.op_type == WalOpType::ROW_PREPARE) {
      if (rec.mut_type != WAL_MUT_INSERT && rec.mut_type != WAL_MUT_UPDATE && rec.mut_type != WAL_MUT_DELETE) {
        DBUG_PRINT("cu_recovery", ("ROW_PREPARE LSN %llu: invalid mutation type %u", (unsigned long long)rec.lsn,
                                   static_cast<unsigned>(rec.mut_type)));
        return {ErrorCode::CORRUPTION, replayed};
      }
      if (rec.mut_type == WAL_MUT_DELETE && !rec.cells.empty()) {
        DBUG_PRINT("cu_recovery",
                   ("ROW_PREPARE LSN %llu: DELETE unexpectedly carries cell redo", (unsigned long long)rec.lsn));
        return {ErrorCode::CORRUPTION, replayed};
      }
      if (has_manifest && known_imcus.count(rec.imcu_id) == 0) {
        DBUG_PRINT("cu_recovery", ("ROW_PREPARE LSN %llu: unknown IMCU %u — recovery aborted",
                                   (unsigned long long)rec.lsn, rec.imcu_id));
        return {ErrorCode::CORRUPTION, replayed};
      }
      if (rec.op_id != rec.lsn) {
        DBUG_PRINT("cu_recovery", ("ROW_PREPARE op_id/lsn mismatch — recovery aborted"));
        return {ErrorCode::CORRUPTION, replayed};
      }
      const uint64_t op_id = rec.op_id;
      auto emplaced = pending.emplace(op_id, std::move(rec));
      if (!emplaced.second) {
        DBUG_PRINT("cu_recovery", ("duplicate ROW_PREPARE op_id=%llu — recovery aborted", (unsigned long long)op_id));
        return {ErrorCode::CORRUPTION, replayed};
      }
      continue;
    }

    if (rec.op_type == WalOpType::ROW_COMMIT) {
      if (rec.commit_lsn != rec.lsn) {
        DBUG_PRINT("cu_recovery", ("ROW_COMMIT commit_lsn/lsn mismatch — recovery aborted"));
        return {ErrorCode::CORRUPTION, replayed};
      }
      auto it = pending.find(rec.op_id);
      if (it == pending.end()) {
        DBUG_PRINT("cu_recovery",
                   ("ROW_COMMIT without ROW_PREPARE op_id=%llu — recovery aborted", (unsigned long long)rec.op_id));
        return {ErrorCode::CORRUPTION, replayed};
      }
      WalRecord prep = std::move(it->second);  // take ownership before erasing
      pending.erase(it);
      if (prep.imcu_id != rec.imcu_id) {
        DBUG_PRINT("cu_recovery",
                   ("ROW_COMMIT op_id=%llu imcu mismatch — recovery aborted", (unsigned long long)rec.op_id));
        return {ErrorCode::CORRUPTION, replayed};
      }

      // The COMMIT digest must describe exactly this prepare.
      if (static_cast<size_t>(rec.redo_count) != prep.cells.size() ||
          rec.operation_crc != compute_operation_crc(prep.cells)) {
        DBUG_PRINT("cu_recovery",
                   ("ROW_COMMIT op_id=%llu digest mismatch — recovery aborted", (unsigned long long)rec.op_id));
        return {ErrorCode::CORRUPTION, replayed};
      }

      auto cp = checkpoint_lsn.find(prep.imcu_id);
      if (cp != checkpoint_lsn.end() && rec.commit_lsn < cp->second) continue;

      if (prep.mut_type == WAL_MUT_DELETE) {
        prep.op_type = WalOpType::DELETE;
        prep.col_id = 0;
        prep.val_len = 0;
        prep.val_data.clear();
      }

      const ErrorCode apply_ec = apply_fn(prep);
      if (apply_ec != ErrorCode::OK) {
        DBUG_PRINT("cu_recovery", ("WAL replay failed at LSN %llu — recovery aborted", (unsigned long long)prep.lsn));
        return {apply_ec, replayed};
      }
      ++replayed;
      max_committed_lsn = std::max(max_committed_lsn, rec.commit_lsn);
      continue;
    }

    // Legacy single-cell record (INSERT / UPDATE / DELETE / NULL_*).
    auto it = checkpoint_lsn.find(rec.imcu_id);
    if (it == checkpoint_lsn.end()) {
      if (has_manifest && known_imcus.count(rec.imcu_id) == 0) {
        DBUG_PRINT("cu_recovery", ("WAL record LSN %llu: unknown IMCU %u — recovery aborted",
                                   (unsigned long long)rec.lsn, rec.imcu_id));
        return {ErrorCode::CORRUPTION, replayed};
      }
      DBUG_PRINT("cu_recovery", ("WAL record LSN %llu: IMCU %u not in recovery set — skipped",
                                 (unsigned long long)rec.lsn, rec.imcu_id));
      continue;
    }

    if (rec.lsn < it->second) continue;

    const ErrorCode apply_ec = apply_fn(rec);
    if (apply_ec != ErrorCode::OK) {
      DBUG_PRINT("cu_recovery", ("WAL replay failed at LSN %llu — recovery aborted", (unsigned long long)rec.lsn));
      return {apply_ec, replayed};
    }
    ++replayed;
    max_committed_lsn = std::max(max_committed_lsn, rec.lsn);
  }

  if (status == WalReadStatus::BAD_MAGIC || status == WalReadStatus::CRC_MISMATCH ||
      status == WalReadStatus::IO_ERROR) {
    DBUG_PRINT("cu_recovery", ("WAL corruption detected at %s — recovery failed", m_wal_path.string().c_str()));
    return {ErrorCode::CORRUPTION, replayed};
  }

  publish_recovery_watermarks(max_seen_lsn, max_committed_lsn);

  DBUG_PRINT("cu_recovery", ("recovery complete: %zu WAL records replayed, next_lsn=%llu", replayed,
                             (unsigned long long)m_written_lsn.load()));
  return {ErrorCode::OK, replayed};
}

bool CURecoveryManager::truncate_wal(uint64_t up_to_lsn) {
  std::lock_guard checkpoint_guard(m_checkpoint_mutex);

  // Recovery intentionally retains multiple checkpoint generations and may
  // fall back to an older one.  WAL GC therefore cannot advance beyond the
  // oldest retained valid generation's replay boundary.
  uint64_t safe_frontier = std::numeric_limits<uint64_t>::max();
  bool have_valid_manifest = false;
  for (uint64_t gen : list_manifest_generations()) {
    auto mres = load_manifest(gen);
    if (!mres.ok() || mres.value.generation != gen) continue;
    if (mres.value.wal_base_lsn == 0) continue;
    safe_frontier = std::min(safe_frontier, mres.value.wal_base_lsn);
    have_valid_manifest = true;
  }

  if (!have_valid_manifest) {
    // Without a durable checkpoint there is no proven WAL prefix that is safe
    // to discard.  LSNs are 1-based, so frontier 1 means "keep everything".
    up_to_lsn = std::min<uint64_t>(up_to_lsn, 1);
  } else {
    up_to_lsn = std::min(up_to_lsn, safe_frontier);
  }

  std::lock_guard lock(m_wal_mutex);
  std::vector<WalRecord> keep;
  {
    std::ifstream in(m_wal_path, std::ios::binary);
    if (!in.is_open()) return true;
    WalRecord rec;
    WalReadStatus status;
    while ((status = read_record(in, rec)) == WalReadStatus::OK) {
      if (rec.lsn >= up_to_lsn) keep.push_back(std::move(rec));
    }
    if (status == WalReadStatus::BAD_MAGIC || status == WalReadStatus::CRC_MISMATCH ||
        status == WalReadStatus::IO_ERROR) {
      return false;
    }
  }
  close_locked();

  std::ostringstream out(std::ios::binary);
  uint64_t last_kept_lsn = 0;
  for (const auto &r : keep) {
    auto buf = encode_record(r);
    out.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
    last_kept_lsn = std::max(last_kept_lsn, r.lsn);
  }
  if (!out.good()) {
    m_recovery_required.store(true, std::memory_order_release);
    return false;
  }

  if (!Recovery::DurableFileSystem::persist_file(m_wal_path, out.str())) {
    m_recovery_required.store(true, std::memory_order_release);
    return false;
  }

  const bool reopened = m_wal_file.open(m_wal_path, /*append=*/true);
  if (!reopened) {
    m_recovery_required.store(true, std::memory_order_release);
    return false;
  }
  m_last_appended_lsn = last_kept_lsn;

  DBUG_PRINT("cu_recovery",
             ("WAL truncated: kept %zu records (lsn >= %llu)", keep.size(), (unsigned long long)up_to_lsn));
  return true;
}
}  // namespace Imcs
}  // namespace ShannonBase