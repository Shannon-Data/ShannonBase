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

#include <chrono>
#include <fstream>
#include <limits>
#include <random>

#include "my_dbug.h"
#include "mysql_version.h"
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
  std::lock_guard lock(m_mutex);
  m_good = false;
  m_checkpointed = UINT64_MAX;
  m_disabled = false;
  m_file.close();
  m_transactions.clear();
  m_pending.clear();
  std::ifstream in(m_path, std::ios::binary);
  std::string bytes(kHeaderSize, '\0');
  if (!in.read(bytes.data(), bytes.size()) || get(bytes.data()) != kMagic || get(bytes.data() + 8) != compatibility() ||
      get(bytes.data() + 40, 4) != crc(bytes.substr(0, 40)))
    return false;
  m_epoch = get(bytes.data() + 16);
  m_base = get(bytes.data() + 24);
  m_last = m_base;
  if (m_epoch == 0 || !scan([&](const Record &r) { return account(r); })) return false;
  m_good = m_file.open(m_path, true);
  return m_good;
}

bool WAL::reset() {
  std::lock_guard lock(m_mutex);
  m_good = false;
  m_file.close();
  m_transactions.clear();
  m_pending.clear();
  std::random_device random;
  m_epoch =
      (uint64_t(random()) << 32) ^ random() ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  if (m_epoch == 0) m_epoch = 1;
  m_base = m_last = 0;
  m_checkpointed = UINT64_MAX;
  m_disabled = false;
  m_good = DurableFileSystem::create_directories(m_directory) && DurableFileSystem::persist_file(m_path, header(0)) &&
           m_file.open(m_path, true);
  return m_good;
}

bool WAL::scan(const std::function<bool(const Record &)> &visit) const {
  std::ifstream in(m_path, std::ios::binary);
  if (!in) return false;
  in.seekg(kHeaderSize);
  std::error_code size_error;
  const uint64_t file_size = std::filesystem::file_size(m_path, size_error);
  if (size_error || file_size < kHeaderSize) return false;
  uint64_t previous = m_base;
  for (;;) {
    char frame[25];  // kind, sequence, transaction, payload length
    in.read(frame, sizeof(frame));
    if (in.gcount() == 0 && in.eof()) return true;
    if (in.gcount() != sizeof(frame)) return false;  // even a torn tail is NOT a complete source history
    Record r{static_cast<Kind>(frame[0]), get(frame + 1), get(frame + 9), {}};
    const uint64_t length = get(frame + 17);
    const auto offset = in.tellg();
    if (offset < 0 || static_cast<uint64_t>(offset) > file_size || file_size - static_cast<uint64_t>(offset) < 4 ||
        length > file_size - static_cast<uint64_t>(offset) - 4 || r.sequence != previous + 1 || length > kMaxPayload)
      return false;
    r.payload.resize(static_cast<size_t>(length));
    if (length && !in.read(r.payload.data(), length)) return false;
    char checksum[4];
    if (!in.read(checksum, sizeof(checksum))) return false;
    uint32_t digest = Utils::crc32c_compute(frame, sizeof(frame), 0);
    digest = Utils::crc32c_compute(r.payload.data(), r.payload.size(), digest);
    if (get(checksum, 4) != digest || !visit(r)) return false;
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
    auto &tx = m_transactions[r.transaction];
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

bool WAL::append(Kind kind, uint64_t transaction, const std::string &payload, uint64_t *sequence) {
  if (!m_good || m_disabled || payload.size() > kMaxPayload || m_last == std::numeric_limits<uint64_t>::max())
    return false;
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
    if (!m_file.write(bytes.data(), bytes.size()) || !m_file.flush_data() || !account(r)) {
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
  if (!append(Kind::CHANGE, transaction, payload, &sequence)) return 0;
  return sequence;
}

bool WAL::terminal(Kind kind, uint64_t transaction) {
  std::lock_guard lock(m_mutex);
  if (m_disabled) return true;
  auto it = m_transactions.find(transaction);
  if (it == m_transactions.end()) return true;  // transaction did not capture this table
  if (it->second.outcome == kind) return true;
  if (it->second.outcome != Kind::CHANGE) return false;
  std::string payload;
  put(payload, it->second.count);
  put(payload, it->second.digest, 4);
  return append(kind, transaction, payload, nullptr);
}
bool WAL::committed(uint64_t transaction) { return terminal(Kind::COMMIT, transaction); }
bool WAL::aborted(uint64_t transaction) { return terminal(Kind::ABORT, transaction); }
bool WAL::invalidate() {
  std::lock_guard lock(m_mutex);
  if (m_disabled || append(Kind::INVALID, 0, {}, nullptr)) return true;
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

std::filesystem::path WAL::checkpoint_path(uint64_t generation) const {
  return m_directory / "checkpoints" / ("capture-" + std::to_string(generation) + ".meta");
}
bool WAL::checkpoint(uint64_t generation) {
  std::lock_guard lock(m_mutex);
  if (!quiescent()) return false;
  std::string bytes;
  put(bytes, kMagic);
  put(bytes, m_epoch);
  put(bytes, generation);
  put(bytes, m_last);
  put(bytes, crc(bytes), 4);
  if (!DurableFileSystem::create_directories(m_directory / "checkpoints") ||
      !DurableFileSystem::persist_file(checkpoint_path(generation), bytes))
    return false;
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
  if (!ok) return false;
  m_file.close();
  if (!DurableFileSystem::rename(temp, m_path)) {
    m_good = false;
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