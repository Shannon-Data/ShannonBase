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
#include "storage/rapid_engine/utils/binlog_position.h"
#include <openssl/evp.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include "sql/binlog.h"
#include "sql/log_event.h"
#include "sql/mysqld.h"
#include "storage/innobase/include/srv0srv.h"

namespace ShannonBase {
namespace Utils {
namespace {
std::string basename(const std::string &name) { return std::filesystem::path(name).filename().string(); }

// Hash the immutable prefix, including FDE and all bytes up to the exact cut.
// Reused filenames after RESET and truncated/replaced files cannot certify a
// checkpoint merely by carrying the same server UUID and filename.
bool prefix_digest(std::istream &in, uint64_t offset, std::string &digest) {
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

bool prefix_digest(const char *path, uint64_t offset, std::string &digest) {
  std::ifstream in(path, std::ios::binary);
  return prefix_digest(in, offset, digest);
}

}  // namespace

BinlogPosition BinlogPositionReader::current_position() {
  // Never force additional source flushes merely to make a Rapid checkpoint.
  if (!mysql_bin_log.is_open() || sync_binlog_period != 1 || srv_flush_log_at_trx_commit != 1) return {};
  LOG_INFO current;
  if (mysql_bin_log.get_current_log(&current) || current.encrypted_header_size) return {};
  return {basename(current.log_file_name), static_cast<uint64_t>(current.pos), {}};
}

bool BinlogPositionReader::certify(BinlogPosition &position) {
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

bool BinlogPositionReader::certify_stream(std::istream &stream, BinlogPosition &position) {
  return position.valid() && prefix_digest(stream, position.offset, position.prefix_digest);
}

bool BinlogPositionReader::certify_file(const char *path, BinlogPosition &position) {
  return position.valid() && prefix_digest(path, position.offset, position.prefix_digest);
}
}  // namespace Utils
}  // namespace ShannonBase
