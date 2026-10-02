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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/recovery/wal.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <random>

#include "my_dbug.h"
#include "mysql_version.h"
#include "sql/log.h"  // sql_print_warning
#include "storage/rapid_engine/utils/crc.h"
namespace ShannonBase {
namespace Recovery {
namespace {
constexpr uint64_t kMagic = 0x314c415750414352ULL;  // RCAPWAL1
constexpr uint64_t kMaxPayload = 1ULL << 30;
constexpr size_t kHeaderSize = 44;
void put(std::string &out, uint64_t value, size_t width = 8) {
  for (size_t i = 0; i < width; ++i) out.push_back(static_cast<char>(value >> (i * 8)));
}
uint64_t get(const char *data, size_t width = 8) {
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) value |= uint64_t(static_cast<unsigned char>(data[i])) << (i * 8);
  return value;
}
uint32_t crc(const std::string &data) { return Utils::crc32c_compute(data.data(), data.size(), 0); }
// Row images are version/architecture-specific, even though the framing is LE.
uint64_t compatibility() {
  const uint16_t endian = 1;
  return (uint64_t(MYSQL_VERSION_ID) << 16) | (sizeof(void *) << 8) | *reinterpret_cast<const unsigned char *>(&endian);
}
// Every refusal below means "no fast recovery for this table"; say which one.
void warn(const std::filesystem::path &path, const char *what) {
  sql_print_warning("Rapid capture WAL %s: %s", path.string().c_str(), what);
}
}  // namespace

WAL::WAL(std::filesystem::path directory) : m_directory(std::move(directory)), m_path(m_directory / "rapid_wal.log") {}

std::string WAL::header(uint64_t base) const {
  std::string bytes;
  put(bytes, kMagic);
  put(bytes, compatibility());
  put(bytes, m_epoch);
  put(bytes, base);
  put(bytes, 0);  // reserved
  put(bytes, crc(bytes), 4);
  return bytes;
}

bool WAL::open() {
  try {
    return open_impl();
  } catch (const std::exception &e) {
    std::lock_guard lock(m_mutex);
    m_good = false;
    warn(m_path, e.what());
    return false;
  }
}

bool WAL::open_impl() {
  std::lock_guard lock(m_mutex);
  m_good = false;
  m_checkpointed = UINT64_MAX;
  m_certified_cut = 0;
  m_disabled = false;
  m_file.close();
  m_transactions.clear();
  m_pending.clear();
  std::ifstream in(m_path, std::ios::binary);
  std::string bytes(kHeaderSize, '\0');
  if (!in.read(bytes.data(), bytes.size())) {
    warn(m_path, "journal is missing or its header is truncated");
    return false;
  }
  if (get(bytes.data()) != kMagic) {
    warn(m_path, "bad magic; not a capture journal");
    return false;
  }
  if (get(bytes.data() + 8) != compatibility()) {
    warn(m_path, "written by a different server version or architecture; its history is discarded");
    return false;
  }
  if (get(bytes.data() + 40, 4) != crc(bytes.substr(0, 40))) {
    warn(m_path, "header checksum mismatch");
    return false;
  }
  m_epoch = get(bytes.data() + 16);
  m_base = get(bytes.data() + 24);
  m_last = m_base;
  if (m_epoch == 0) {
    warn(m_path, "zero epoch in header");
    return false;
  }
  if (!scan([&](const Record &r) { return account(r); })) {
    warn(m_path, "history is incomplete or corrupt; the table must be reloaded from the primary");
    return false;
  }
  m_good = m_file.open(m_path, true);
  if (!m_good) warn(m_path, "cannot open the journal for append");
  return m_good;
}

bool WAL::reset() {
  try {
    return reset_impl();
  } catch (const std::exception &e) {
    std::lock_guard lock(m_mutex);
    m_good = false;
    warn(m_path, e.what());
    return false;
  }
}

bool WAL::reset_impl() {
  std::lock_guard lock(m_mutex);
  m_good = false;
  m_file.close();
  m_transactions.clear();
  m_pending.clear();
  std::random_device rng;
  m_epoch = (uint64_t(rng()) << 32) ^ rng() ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  if (m_epoch == 0) m_epoch = 1;
  m_base = m_last = 0;
  m_checkpointed = UINT64_MAX;
  m_certified_cut = 0;
  m_disabled = false;
  bool ok = DurableFileSystem::create_directories(m_directory);
  if (ok) {
    // <table>/checkpoints/ is shared with TablePersistenceManager, which keeps
    // its generation manifests there. Delete only this journal's own
    // certificates (capture-<gen>.meta); a blanket remove_all() would destroy
    // another subsystem's durable state. The manifests' lifecycle is handled by
    // TablePersistenceManager::reset_epoch_impl().
    const std::filesystem::path cert_dir = m_directory / "checkpoints";
    std::error_code ec;
    const auto cert_type = std::filesystem::status(cert_dir, ec);
    // A missing checkpoints/ directory is normal (there is simply nothing to
    // clean), not a reset failure. Ignore ENOENT/ENOTDIR explicitly: not every
    // standard library clears it from the error_code here, and treating it as an
    // error makes every first reset() fail.
    if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) ec.clear();
    if (ec) {
      ok = false;
    } else if (cert_type.type() == std::filesystem::file_type::directory) {
      bool removed_any = false;
      for (std::filesystem::directory_iterator it(cert_dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const bool is_cert =
            name.size() > 13 && name.compare(0, 8, "capture-") == 0 && name.compare(name.size() - 5, 5, ".meta") == 0;
        if (!is_cert) continue;
        std::error_code rm;
        std::filesystem::remove(it->path(), rm);
        if (!rm) removed_any = true;
      }
      if (ec) {
        ok = false;
      } else if (removed_any) {
        ok = DurableFileSystem::sync_directory(cert_dir);
      }
    }
  }
  m_good = ok && DurableFileSystem::persist_file(m_path, header(0)) && m_file.open(m_path, true);
  return m_good;
}

bool WAL::scan(const std::function<bool(const Record &)> &visit) const {
  std::ifstream in(m_path, std::ios::binary);
  if (!in) {
    warn(m_path, "cannot open the journal for reading");
    return false;
  }
  in.seekg(kHeaderSize);
  std::error_code size_error;
  const uint64_t file_size = std::filesystem::file_size(m_path, size_error);
  if (size_error || file_size < kHeaderSize) {
    warn(m_path, "cannot determine the journal size");
    return false;
  }
  uint64_t previous = m_base;
  for (;;) {
    char frame[25];  // kind, sequence, transaction, payload length
    in.read(frame, sizeof(frame));
    if (in.gcount() == 0 && in.eof()) return true;
    if (in.gcount() != sizeof(frame)) {  // even a torn tail is NOT a complete source history
      warn(m_path, "torn or truncated frame header");
      return false;
    }
    Record r{static_cast<Kind>(frame[0]), get(frame + 1), get(frame + 9), {}};
    const uint64_t length = get(frame + 17);
    const auto offset = in.tellg();
    if (offset < 0 || static_cast<uint64_t>(offset) > file_size || file_size - static_cast<uint64_t>(offset) < 4 ||
        length > file_size - static_cast<uint64_t>(offset) - 4 || r.sequence != previous + 1 || length > kMaxPayload) {
      warn(m_path, "frame length or sequence is invalid (gap, duplicate, oversized or truncated frame)");
      return false;
    }
    r.payload.resize(static_cast<size_t>(length));
    if (length && !in.read(r.payload.data(), length)) {
      warn(m_path, "truncated frame payload");
      return false;
    }
    char checksum[4];
    if (!in.read(checksum, sizeof(checksum))) {
      warn(m_path, "truncated frame checksum");
      return false;
    }
    uint32_t digest = Utils::crc32c_compute(frame, sizeof(frame), 0);
    digest = Utils::crc32c_compute(r.payload.data(), r.payload.size(), digest);
    if (get(checksum, 4) != digest) {
      warn(m_path, "frame checksum mismatch");
      return false;
    }
    if (!visit(r)) return false;
    previous = r.sequence;
  }
}

bool WAL::account(const Record &r) {
  if (m_disabled || r.sequence != m_last + 1) return false;
  if (r.kind == Kind::INVALID) {
    if (r.transaction != 0 || !r.payload.empty()) return false;
    m_disabled = true;
  } else {
    if (r.transaction == 0) return false;
    if (r.kind != Kind::CHANGE && r.kind != Kind::COMMIT && r.kind != Kind::ABORT) return false;
    auto found = m_transactions.find(r.transaction);
    if (found == m_transactions.end()) {
      // A terminal frame for a transaction with no changes is invalid; reject it
      // without leaving an entry behind.
      if (r.kind != Kind::CHANGE) return false;
      found = m_transactions.emplace(r.transaction, Transaction{}).first;
    }
    auto &tx = found->second;
    if (tx.outcome != Kind::CHANGE) return false;
    if (r.kind == Kind::CHANGE) {
      ++tx.count;
      tx.digest = Utils::crc32c_compute(r.payload.data(), r.payload.size(), tx.digest);
      m_pending.insert(r.sequence);
    } else if (r.kind == Kind::COMMIT || r.kind == Kind::ABORT) {
      if (tx.count == 0 || r.payload.size() != 12 || get(r.payload.data()) != tx.count ||
          get(r.payload.data() + 8, 4) != tx.digest)
        return false;
      tx.outcome = r.kind;
    } else {
      return false;
    }
    tx.last = r.sequence;
  }
  m_last = r.sequence;
  return true;
}

bool WAL::append(Kind kind, uint64_t transaction, const std::string &payload, uint64_t *sequence, bool sync) {
  if (!m_good || m_disabled) return false;
  if (payload.size() > kMaxPayload || m_last == std::numeric_limits<uint64_t>::max()) {
    // A change that cannot be journaled is a hole in the history: poison the
    // journal here instead of relying on every caller to invalidate it.
    m_good = false;
    return false;
  }
  try {
    DBUG_EXECUTE_IF("rapid_capture_wal_write_error", {
      m_good = false;
      return false;
    });
    Record r{kind, m_last + 1, transaction, payload};
    std::string bytes;
    put(bytes, static_cast<uint8_t>(kind), 1);
    put(bytes, r.sequence);
    put(bytes, transaction);
    put(bytes, payload.size());
    bytes.append(payload);
    put(bytes, crc(bytes), 4);
    if (!m_file.write(bytes.data(), bytes.size()) || (sync && !m_file.flush_data()) || !account(r)) {
      m_good = false;
      return false;
    }
    if (sequence) *sequence = r.sequence;
    return true;
  } catch (const std::exception &) {
    // In particular, allocation can fail after the frame was written but
    // before account() completed. Never append another frame at that sequence.
    m_good = false;
    return false;
  }
}

uint64_t WAL::capture(uint64_t transaction, const std::string &payload) {
  std::lock_guard lock(m_mutex);
  uint64_t sequence = 0;
  if (!append(Kind::CHANGE, transaction, payload, &sequence, /*sync=*/false)) return 0;
  return sequence;
}

bool WAL::terminal(Kind kind, uint64_t transaction) {
  std::lock_guard lock(m_mutex);
  if (m_disabled) return true;
  auto it = m_transactions.find(transaction);
  // A well-formed caller only asks about a table the transaction captured
  // (register_change() ran before capture()). An unknown transaction therefore
  // means the change was never journaled here -- a hole -- so refuse to certify
  // rather than report success. A disabled journal is handled above: there is
  // nothing to certify. WAL::append() also poisons the journal when a captured
  // change cannot be written.
  if (it == m_transactions.end()) return false;
  if (it->second.outcome == kind) return true;
  if (it->second.outcome != Kind::CHANGE) return false;
  std::string payload;
  put(payload, it->second.count);
  put(payload, it->second.digest, 4);
  return append(kind, transaction, payload, nullptr, /*sync=*/kind == Kind::COMMIT);
}
bool WAL::committed(uint64_t transaction) { return terminal(Kind::COMMIT, transaction); }
bool WAL::aborted(uint64_t transaction) { return terminal(Kind::ABORT, transaction); }
bool WAL::invalidate() {
  std::lock_guard lock(m_mutex);
  // Simulate failure of both the INVALID append and durable journal removal.
  DBUG_EXECUTE_IF("rapid_capture_wal_invalidation_error", { return false; });
  if (m_disabled || append(Kind::INVALID, 0, {}, nullptr, /*sync=*/true)) return true;
  // A failed append may leave a perfectly valid old prefix. Removing the
  // journal and syncing its directory revokes every checkpoint certificate,
  // even when ENOSPC prevents writing an INVALID record.
  m_good = false;
  m_file.close();
  std::error_code ec;
  std::filesystem::remove(m_path, ec);
  if (ec || !DurableFileSystem::sync_directory(m_path)) return false;
  m_disabled = true;
  return true;
}
void WAL::applied(uint64_t sequence) {
  std::lock_guard lock(m_mutex);
  m_pending.erase(sequence);
}
bool WAL::needs_checkpoint() {
  std::lock_guard lock(m_mutex);
  return m_good && !m_disabled && m_last != m_checkpointed;
}
bool WAL::quiescent() const {
  std::lock_guard lock(m_mutex);
  if (!m_good || m_disabled || !m_pending.empty()) return false;
  for (const auto &[id, tx] : m_transactions)
    if (tx.outcome == Kind::CHANGE) return false;
  return true;
}

bool WAL::has_unresolved_transaction() const {
  std::lock_guard lock(m_mutex);
  for (const auto &[id, tx] : m_transactions)
    if (tx.outcome == Kind::CHANGE) return true;
  return false;
}

uint64_t WAL::safe_cut() const {
  std::lock_guard lock(m_mutex);
  if (m_pending.empty()) return m_last;
  const uint64_t oldest = *std::min_element(m_pending.begin(), m_pending.end());
  return oldest > m_base ? oldest - 1 : m_base;
}

std::filesystem::path WAL::checkpoint_path(uint64_t generation) const {
  return m_directory / "checkpoints" / ("capture-" + std::to_string(generation) + ".meta");
}
bool WAL::checkpoint_exists(uint64_t generation) const {
  std::lock_guard lock(m_mutex);
  std::error_code ec;
  return std::filesystem::exists(checkpoint_path(generation), ec) || ec;
}

std::string WAL::checkpoint_blockers() const {
  std::lock_guard lock(m_mutex);
  if (!m_good) return "the journal is not usable";
  if (m_disabled) return "the journal has been revoked";
  size_t unresolved = 0;
  double oldest = 0;
  const auto now = std::chrono::steady_clock::now();
  for (const auto &[id, tx] : m_transactions) {
    if (tx.outcome != Kind::CHANGE) continue;
    ++unresolved;
    oldest = std::max(oldest, std::chrono::duration<double>(now - tx.first_seen).count());
  }
  if (unresolved == 0 && m_pending.empty()) return std::string();
  return std::to_string(unresolved) + " unresolved source transaction(s) (oldest " +
         std::to_string(static_cast<uint64_t>(oldest)) + " s), " + std::to_string(m_pending.size()) +
         " captured change(s) not yet applied";
}

uint64_t WAL::oldest_unresolved_seconds() const {
  std::lock_guard lock(m_mutex);
  double oldest = 0;
  const auto now = std::chrono::steady_clock::now();
  for (const auto &[id, tx] : m_transactions) {
    if (tx.outcome != Kind::CHANGE) continue;
    oldest = std::max(oldest, std::chrono::duration<double>(now - tx.first_seen).count());
  }
  return static_cast<uint64_t>(oldest);
}

bool WAL::checkpoint(uint64_t generation) {
  std::lock_guard lock(m_mutex);
  if (!quiescent()) return false;
  return checkpoint_locked(generation, m_last);
}

bool WAL::checkpoint(uint64_t generation, uint64_t cut) {
  std::lock_guard lock(m_mutex);
  return checkpoint_locked(generation, cut);
}

bool WAL::checkpoint_locked(uint64_t generation, uint64_t cut) {
  if (!m_good || m_disabled) return false;
  if (cut < m_base || cut > m_last) return false;
  {
    // A generation is certified once; a second certificate could name a newer cut
    // than the snapshot it sits next to.
    std::error_code ec;
    if (std::filesystem::exists(checkpoint_path(generation), ec) || ec) return false;
  }
  // CHANGE and ABORT frames are not individually synced, so make the whole prefix
  // durable before a certificate names a cut inside it.
  if (!m_file.flush_data()) {
    m_good = false;
    return false;
  }
  std::string bytes;
  put(bytes, kMagic);
  put(bytes, m_epoch);
  put(bytes, generation);
  put(bytes, cut);
  put(bytes, crc(bytes), 4);
  if (!DurableFileSystem::create_directories(m_directory / "checkpoints") ||
      !DurableFileSystem::persist_file(checkpoint_path(generation), bytes))
    return false;
  m_certified_cut = cut;
  return true;
}
bool WAL::checkpoint_cut(uint64_t generation, uint64_t *cut) const {
  std::lock_guard lock(m_mutex);
  if (!m_good || m_disabled) return false;
  std::ifstream in(checkpoint_path(generation), std::ios::binary);
  std::string bytes(36, '\0');
  if (!in.read(bytes.data(), bytes.size()) || in.peek() != std::char_traits<char>::eof() ||
      get(bytes.data()) != kMagic || get(bytes.data() + 8) != m_epoch || get(bytes.data() + 16) != generation ||
      get(bytes.data() + 32, 4) != crc(bytes.substr(0, 32)))
    return false;
  *cut = get(bytes.data() + 24);
  return *cut >= m_base && *cut <= m_last;
}

bool WAL::replay(uint64_t cut, const std::function<bool(uint64_t, uint64_t, const std::string &)> &apply) {
  try {
    return replay_impl(cut, apply);
  } catch (const std::exception &e) {
    // The apply callback or an allocation failed part-way: the caller must reload.
    warn(m_path, e.what());
    return false;
  }
}

bool WAL::replay_impl(uint64_t cut, const std::function<bool(uint64_t, uint64_t, const std::string &)> &apply) {
  std::lock_guard lock(m_mutex);
  if (!m_good || m_disabled || cut < m_base || cut > m_last) return false;
  // A crash between primary commit and its outcome marker is ambiguous. It
  // must reload, not silently discard a potentially committed transaction.
  for (const auto &[id, tx] : m_transactions)
    if (tx.outcome == Kind::CHANGE) return false;
  for (auto it = m_pending.begin(); it != m_pending.end();) {
    if (*it <= cut)
      it = m_pending.erase(it);
    else
      ++it;
  }
  return scan([&](const Record &r) {
    if (r.sequence <= cut || r.kind != Kind::CHANGE) return true;
    auto it = m_transactions.find(r.transaction);
    if (it == m_transactions.end()) return false;
    if (it->second.outcome == Kind::COMMIT && !apply(r.sequence, r.transaction, r.payload)) return false;
    m_pending.erase(r.sequence);
    return true;
  });
}

bool WAL::compact(uint64_t cut) {
  try {
    return compact_impl(cut);
  } catch (const std::exception &e) {
    warn(m_path, e.what());
    return false;
  }
}

bool WAL::compact_to_generation(uint64_t generation) {
  uint64_t cut = 0;
  if (!checkpoint_cut(generation, &cut)) return false;
  return compact(cut);
}

bool WAL::compact_impl(uint64_t cut) {
  std::lock_guard lock(m_mutex);
  if (!quiescent() || cut < m_base || cut > m_last) return false;
  if (cut == m_base) return true;
  const auto temp = m_directory / "capture_wal.compacting";
  DurableFile output;
  if (!output.open(temp, false)) return false;
  const std::string prefix = header(cut);
  bool ok = output.write(prefix.data(), prefix.size()) && scan([&](const Record &r) {
              if (r.sequence <= cut) return true;
              std::string bytes;
              put(bytes, static_cast<uint8_t>(r.kind), 1);
              put(bytes, r.sequence);
              put(bytes, r.transaction);
              put(bytes, r.payload.size());
              bytes.append(r.payload);
              put(bytes, crc(bytes), 4);
              return output.write(bytes.data(), bytes.size());
            });
  ok = ok && output.flush_data();
  output.close();
  if (!ok) {
    std::error_code rm;
    std::filesystem::remove(temp, rm);  // do not leave a half-written copy behind
    return false;
  }
  m_file.close();
  if (!DurableFileSystem::rename(temp, m_path)) {
    // The rename never took effect, so the previous journal is still intact.
    // Reopen it instead of leaving the WAL permanently unusable (m_good=false
    // with a closed file), which would force a primary reload for a table whose
    // on-disk history is fine.
    std::error_code rm;
    std::filesystem::remove(temp, rm);
    m_good = m_file.open(m_path, true);
    return false;
  }
  m_base = cut;
  for (auto it = m_transactions.begin(); it != m_transactions.end();) {
    if (it->second.last <= cut)
      it = m_transactions.erase(it);
    else
      ++it;
  }
  m_good = m_file.open(m_path, true);
  return m_good;
}
}  // namespace Recovery
}  // namespace ShannonBase