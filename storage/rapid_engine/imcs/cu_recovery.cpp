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

#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#ifndef _WIN32
#include <fcntl.h>   // O_WRONLY
#include <unistd.h>  // fdatasync, close
#endif

#include "storage/innobase/include/ut0dbg.h"  // DBUG_PRINT, UNIV_SQL_NULL
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/imcu.h"

namespace ShannonBase {
namespace Imcs {

namespace fs = std::filesystem;
//  CRC-32  (ISO 3309, polynomial 0xEDB88320)
/* static */
uint32_t CURecoveryManager::crc32(const void *data, size_t len, uint32_t seed) {
  static constexpr uint32_t kPoly = 0xEDB88320u;
  uint32_t crc = ~seed;
  const auto *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (kPoly & -(crc & 1u));
  }
  return ~crc;
}

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

  // Open WAL in append mode (creates if absent).
  m_wal_out.open(m_wal_path, std::ios::out | std::ios::binary | std::ios::app);
  if (!m_wal_out.is_open()) return false;
#ifndef _WIN32
  m_wal_fd = ::open(m_wal_path.c_str(), O_WRONLY);
#endif

  uint64_t max_lsn = 0;
  {
    std::ifstream wal_in(m_wal_path, std::ios::binary | std::ios::ate);
    auto file_size = wal_in.tellg();
    if (file_size >= static_cast<std::streamoff>(WAL_FOOTER_SIZE)) {
      wal_in.seekg(-static_cast<std::streamoff>(WAL_FOOTER_SIZE), std::ios::end);
      uint32_t foot_magic = 0, reserved = 0;
      wal_in.read(reinterpret_cast<char *>(&foot_magic), 4);
      wal_in.read(reinterpret_cast<char *>(&reserved), 4);
      if (foot_magic == WAL_FOOT_MAGIC) {
        wal_in.read(reinterpret_cast<char *>(&max_lsn), 8);
      } else {  // old format without footer: need to scan the whole file to find max LSN
        wal_in.seekg(0);
        WalRecord rec;
        while (read_record(wal_in, rec))
          if (rec.lsn > max_lsn) max_lsn = rec.lsn;
      }
    }
  }
  if (max_lsn >= m_lsn.load()) m_lsn.store(max_lsn + 1);

  DBUG_PRINT("cu_recovery",
             ("WAL opened at %s  next_lsn=%llu", m_wal_path.string().c_str(), (unsigned long long)m_lsn.load()));
  return true;
}

void CURecoveryManager::close_locked() {
  // caller has already acquired m_wal_mutex
  if (m_wal_out.is_open()) {
    m_wal_out.flush();
    m_wal_out.close();
  }
#ifndef _WIN32
  if (m_wal_fd >= 0) {
    ::close(m_wal_fd);
    m_wal_fd = -1;
  }
#endif
}
void CURecoveryManager::close() {
  std::lock_guard lock(m_wal_mutex);
  if (m_wal_out.is_open()) {
    m_wal_out.flush();
    m_wal_out.close();
#ifndef _WIN32
    if (m_wal_fd >= 0) {
      ::close(m_wal_fd);
      m_wal_fd = -1;
    }
#endif
  }
}

bool CURecoveryManager::sync() {
  std::lock_guard lock(m_wal_mutex);
  if (!m_wal_out.is_open()) return false;
  m_wal_out.flush();
#ifndef _WIN32
  if (m_wal_fd >= 0) ::fdatasync(m_wal_fd);
#endif
  return m_wal_out.good();
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
  const size_t data_bytes = (rec.val_len != UNIV_SQL_NULL && rec.val_len > 0) ? rec.val_len : 0;

  std::vector<uint8_t> buf;
  buf.reserve(57 + data_bytes);

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
  push_u32(rec.imcu_id);
  push_u32(rec.col_id);
  push_u64(rec.row_id);
  push_u64(rec.txn_id);
  push_u64(rec.scn);
  push_u64(static_cast<uint64_t>(rec.val_len));
  if (data_bytes > 0) push(rec.val_data.data(), data_bytes);

  uint32_t chk = crc32(buf.data(), buf.size());
  push_u32(chk);

  return buf;
}

bool CURecoveryManager::read_record(std::istream &in, WalRecord &rec) const {
  uint32_t running_crc = 0;

  auto rd_crc = [&](void *p, size_t n) -> bool {
    if (!in.read(reinterpret_cast<char *>(p), static_cast<std::streamsize>(n))) return false;
    running_crc = crc32(p, n, running_crc);
    return true;
  };

  uint32_t magic = 0;
  if (!rd_crc(&magic, 4) || magic != WAL_MAGIC) return false;

  uint64_t lsn = 0;
  if (!rd_crc(&lsn, 8)) return false;
  uint8_t op = 0;
  if (!rd_crc(&op, 1)) return false;
  uint32_t iid = 0;
  if (!rd_crc(&iid, 4)) return false;
  uint32_t cid = 0;
  if (!rd_crc(&cid, 4)) return false;
  uint64_t rid = 0;
  if (!rd_crc(&rid, 8)) return false;
  uint64_t tid = 0;
  if (!rd_crc(&tid, 8)) return false;
  uint64_t scn = 0;
  if (!rd_crc(&scn, 8)) return false;
  uint64_t vl = 0;
  if (!rd_crc(&vl, 8)) return false;

  std::vector<uint8_t> val_data;
  const size_t data_bytes = (vl != UNIV_SQL_NULL && vl > 0) ? static_cast<size_t>(vl) : 0;
  if (data_bytes > 0) {
    val_data.resize(data_bytes);
    if (!rd_crc(val_data.data(), data_bytes)) return false;
  }

  uint32_t stored_crc = 0;
  if (!in.read(reinterpret_cast<char *>(&stored_crc), 4)) return false;

  if (running_crc != stored_crc) {
    DBUG_PRINT("cu_recovery", ("WAL CRC mismatch at LSN %llu", (unsigned long long)lsn));
    return false;
  }

  rec.lsn = lsn;
  rec.op_type = static_cast<WalOpType>(op);
  rec.imcu_id = iid;
  rec.col_id = cid;
  rec.row_id = rid;
  rec.txn_id = tid;
  rec.scn = scn;
  rec.val_len = static_cast<size_t>(vl);
  rec.val_data = std::move(val_data);
  return true;
}

void CURecoveryManager::update_wal_footer(uint64_t current_max_lsn) {
  uint32_t magic = WAL_FOOT_MAGIC;
  uint32_t reserved = 0;
  m_wal_out.write(reinterpret_cast<const char *>(&magic), 4);
  m_wal_out.write(reinterpret_cast<const char *>(&reserved), 4);
  m_wal_out.write(reinterpret_cast<const char *>(&current_max_lsn), 8);
}

bool CURecoveryManager::append_record(const WalRecord &rec) {
  std::lock_guard lock(m_wal_mutex);
  if (!m_wal_out.is_open()) return false;

  auto buf = encode_record(rec);
  m_wal_out.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
  return m_wal_out.good();
}

bool CURecoveryManager::log_write(uint32_t imcu_id, uint32_t col_id, uint64_t row_id, uint64_t txn_id, uint64_t scn,
                                  const uint8_t *val_data, size_t val_len) {
  WalRecord rec;
  rec.lsn = m_lsn.fetch_add(1, std::memory_order_relaxed);
  rec.op_type = (val_len == UNIV_SQL_NULL) ? WalOpType::NULL_INSERT : WalOpType::INSERT;
  rec.imcu_id = imcu_id;
  rec.col_id = col_id;
  rec.row_id = row_id;
  rec.txn_id = txn_id;
  rec.scn = scn;
  rec.val_len = val_len;
  if (val_len != UNIV_SQL_NULL && val_len > 0 && val_data) rec.val_data.assign(val_data, val_data + val_len);
  auto result = append_record(rec);
  if (result) update_wal_footer(rec.lsn);
  return result;
}

bool CURecoveryManager::log_update(uint32_t imcu_id, uint32_t col_id, uint64_t row_id, uint64_t txn_id, uint64_t scn,
                                   const uint8_t *new_val, size_t val_len) {
  WalRecord rec;
  rec.lsn = m_lsn.fetch_add(1, std::memory_order_relaxed);
  rec.op_type = (val_len == UNIV_SQL_NULL) ? WalOpType::NULL_UPDATE : WalOpType::UPDATE;
  rec.imcu_id = imcu_id;
  rec.col_id = col_id;
  rec.row_id = row_id;
  rec.txn_id = txn_id;
  rec.scn = scn;
  rec.val_len = val_len;
  if (val_len != UNIV_SQL_NULL && val_len > 0 && new_val) rec.val_data.assign(new_val, new_val + val_len);
  auto result = append_record(rec);
  if (result) update_wal_footer(rec.lsn);
  return result;
}

bool CURecoveryManager::log_delete(uint32_t imcu_id, uint32_t col_id, uint64_t row_id, uint64_t txn_id, uint64_t scn) {
  WalRecord rec;
  rec.lsn = m_lsn.fetch_add(1, std::memory_order_relaxed);
  rec.op_type = WalOpType::DELETE;
  rec.imcu_id = imcu_id;
  rec.col_id = col_id;
  rec.row_id = row_id;
  rec.txn_id = txn_id;
  rec.scn = scn;
  rec.val_len = 0;
  auto result = append_record(rec);
  if (result) update_wal_footer(rec.lsn);
  return result;
}

fs::path CURecoveryManager::snap_path(uint32_t imcu_id) const {
  std::ostringstream ss;
  ss << "imcu_" << imcu_id << ".snap";
  return m_partition_dir / ss.str();
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

bool CURecoveryManager::checkpoint(Imcu *imcu, uint64_t snapshot_lsn) {
  if (!imcu) return false;

  if (snapshot_lsn == 0) snapshot_lsn = m_lsn.load(std::memory_order_acquire);

  uint32_t imcu_id = imcu->get_imcu_id();
  uint32_t col_count = static_cast<uint32_t>(imcu->get_column_count());

  // Write to a temp file first; rename to final path on success (atomic-ish).
  fs::path tmp_path = snap_path(imcu_id).string() + ".tmp";
  fs::path final_path = snap_path(imcu_id);

  {
    std::ofstream snap(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!snap.is_open()) {
      DBUG_PRINT("cu_recovery", ("checkpoint: cannot open %s", tmp_path.string().c_str()));
      return false;
    }

    if (!write_snap_header(snap, imcu_id, col_count, snapshot_lsn)) {
      DBUG_PRINT("cu_recovery", ("checkpoint: header write failed"));
      return false;
    }

    size_t row_count = imcu->get_row_count();
    for (uint32_t c = 0; c < col_count; ++c) {
      CU *cu = imcu->get_cu(c);
      if (!cu) {
        uint64_t sentinel = 0;
        snap.write(reinterpret_cast<const char *>(&sentinel), sizeof(sentinel));
        continue;
      }

      auto size_pos = snap.tellp();
      uint64_t cu_size_placeholder = 0;
      snap.write(reinterpret_cast<const char *>(&cu_size_placeholder), sizeof(cu_size_placeholder));

      auto data_start = snap.tellp();
      bool ok = cu->serialize(snap, row_count);
      if (!ok) {
        DBUG_PRINT("cu_recovery", ("checkpoint: CU %u serialize failed", c));
        return false;
      }
      auto data_end = snap.tellp();

      // Go back and write the actual CU size (data_end - data_start).
      uint64_t cu_size = static_cast<uint64_t>(data_end - data_start);
      snap.seekp(size_pos);
      snap.write(reinterpret_cast<const char *>(&cu_size), sizeof(cu_size));
      snap.seekp(data_end);
    }

    snap.flush();
    if (!snap.good()) {
      DBUG_PRINT("cu_recovery", ("checkpoint: stream error before rename"));
      return false;
    }
  }  // snap closed here

  // Atomically replace the previous snapshot.
  std::error_code ec;
  fs::rename(tmp_path, final_path, ec);
  if (ec) {
    DBUG_PRINT("cu_recovery", ("checkpoint rename failed: %s", ec.message().c_str()));
    fs::remove(tmp_path, ec);
    return false;
  }

  DBUG_PRINT("cu_recovery", ("checkpoint IMCU %u  lsn=%llu  cols=%u  path=%s", imcu_id,
                             (unsigned long long)snapshot_lsn, col_count, final_path.string().c_str()));
  return true;
}

struct MemStreamBuf : std::streambuf {
  MemStreamBuf(const char *data, size_t size) {
    char *p = const_cast<char *>(data);
    setg(p, p, p + size);
  }
};

uint64_t CURecoveryManager::load_snapshot(Imcu *imcu) {
  if (!imcu) return 0;

  uint32_t imcu_id = imcu->get_imcu_id();
  fs::path path = snap_path(imcu_id);

  std::ifstream snap(path, std::ios::binary);
  if (!snap.is_open()) {
    DBUG_PRINT("cu_recovery", ("no snapshot for IMCU %u at %s", imcu_id, path.string().c_str()));
    return 0;
  }

  uint32_t snap_imcu_id = 0, col_count = 0;
  uint64_t snap_lsn = 0;
  if (!read_snap_header(snap, snap_imcu_id, col_count, snap_lsn)) {
    DBUG_PRINT("cu_recovery", ("snapshot header corrupt: %s", path.string().c_str()));
    return 0;
  }
  if (snap_imcu_id != imcu_id) {
    DBUG_PRINT("cu_recovery", ("snapshot IMCU id mismatch: file=%u expected=%u", snap_imcu_id, imcu_id));
    return 0;
  }

  uint32_t actual_cols = static_cast<uint32_t>(imcu->get_column_count());
  uint32_t restore_cols = std::min(col_count, actual_cols);

  for (uint32_t c = 0; c < col_count; ++c) {
    uint64_t cu_size = 0;
    if (!snap.read(reinterpret_cast<char *>(&cu_size), sizeof(cu_size))) return 0;  // truncated

    if (cu_size == 0) continue;  // placeholder

    if (c >= restore_cols) {
      // Extra columns in snapshot that no longer exist — skip.
      snap.seekg(static_cast<std::streamoff>(cu_size), std::ios::cur);
      continue;
    }

    CU *cu = imcu->get_cu(c);
    if (!cu) {
      snap.seekg(static_cast<std::streamoff>(cu_size), std::ios::cur);
      continue;
    }

    // Read exactly cu_size bytes into a buffer, then wrap in istringstream.
    std::vector<char> cu_buf(cu_size);
    if (!snap.read(cu_buf.data(), static_cast<std::streamsize>(cu_size))) return 0;

    MemStreamBuf msb(cu_buf.data(), cu_size);
    std::istream cu_in(&msb);
    if (!cu->deserialize(cu_in)) {
      DBUG_PRINT("cu_recovery", ("CU %u deserialize failed", c));
      return 0;
    }
  }

  DBUG_PRINT("cu_recovery", ("loaded snapshot for IMCU %u  snap_lsn=%llu  cols=%u", imcu_id,
                             (unsigned long long)snap_lsn, restore_cols));
  return snap_lsn;
}

size_t CURecoveryManager::recover(const std::vector<Imcu *> &imcus,
                                  const std::function<void(const WalRecord &)> &apply_fn) {
  if (imcus.empty()) return 0;

  // Phase 1: load snapshots, record per-IMCU checkpoint LSN
  // imcu_id → checkpoint LSN (0 if no snapshot found)
  std::unordered_map<uint32_t, uint64_t> checkpoint_lsn;

  for (Imcu *imcu : imcus) {
    if (!imcu) continue;
    uint32_t iid = imcu->get_imcu_id();
    uint64_t lsn = load_snapshot(imcu);
    checkpoint_lsn[iid] = lsn;
    DBUG_PRINT("cu_recovery", ("IMCU %u checkpoint_lsn=%llu", iid, (unsigned long long)lsn));
  }

  // Phase 2: replay WAL records past each IMCU's checkpoint LSN
  std::ifstream wal_in(m_wal_path, std::ios::binary);
  if (!wal_in.is_open()) {
    DBUG_PRINT("cu_recovery", ("no WAL at %s — recovery complete (snapshot-only)", m_wal_path.string().c_str()));
    return 0;
  }

  size_t replayed = 0;
  uint64_t max_seen_lsn = 0;
  WalRecord rec;

  while (read_record(wal_in, rec)) {
    if (rec.lsn > max_seen_lsn) max_seen_lsn = rec.lsn;

    // Look up the checkpoint LSN for this record's IMCU.
    auto it = checkpoint_lsn.find(rec.imcu_id);
    if (it == checkpoint_lsn.end()) {
      // Unknown IMCU (possibly from a different table shard) — skip.
      DBUG_PRINT("cu_recovery",
                 ("WAL record LSN %llu: unknown IMCU %u — skipped", (unsigned long long)rec.lsn, rec.imcu_id));
      continue;
    }

    // This record is already covered by the checkpoint snapshot — skip.
    if (rec.lsn <= it->second) continue;

    // Apply the record via the caller-supplied function.
    apply_fn(rec);
    ++replayed;
  }

  // Advance our LSN counter past anything we saw in the WAL.
  uint64_t expected = m_lsn.load(std::memory_order_relaxed);
  while (max_seen_lsn + 1 > expected) {
    if (m_lsn.compare_exchange_weak(expected, max_seen_lsn + 1, std::memory_order_relaxed)) break;
  }

  DBUG_PRINT("cu_recovery", ("recovery complete: %zu WAL records replayed, next_lsn=%llu", replayed,
                             (unsigned long long)m_lsn.load()));
  return replayed;
}

bool CURecoveryManager::truncate_wal(uint64_t up_to_lsn) {
  std::lock_guard lock(m_wal_mutex);
  std::vector<WalRecord> keep;
  {
    std::ifstream in(m_wal_path, std::ios::binary);
    if (!in.is_open()) return true;
    WalRecord rec;
    while (read_record(in, rec)) {
      if (rec.lsn >= up_to_lsn) keep.push_back(std::move(rec));
    }
  }
  close_locked();

  {
    std::ofstream out(m_wal_path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    for (const auto &r : keep) {
      auto buf = encode_record(r);
      out.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
    }
  }
  m_wal_out.open(m_wal_path, std::ios::out | std::ios::binary | std::ios::app);
#ifndef _WIN32
  if (m_wal_out.is_open()) m_wal_fd = ::open(m_wal_path.c_str(), O_WRONLY);
#endif

  DBUG_PRINT("cu_recovery",
             ("WAL truncated: kept %zu records (lsn >= %llu)", keep.size(), (unsigned long long)up_to_lsn));
  return m_wal_out.is_open();
}
}  // namespace Imcs
}  // namespace ShannonBase