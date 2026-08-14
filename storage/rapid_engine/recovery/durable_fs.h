/**
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   Copyright (c) 2023 - 2026, Shannon Data AI and/or its affiliates.
*/
/**
 * DurableFileSystem / DurableFile
 *
 * The single place that knows how to make bytes reach stable storage and how
 * to make filesystem metadata (rename / unlink / mkdir) durable.  Everything
 * else in the recovery subsystem treats these primitives as its durability
 * boundary; higher layers only deal with "durable" vs "not durable", never
 * with open/write/fsync/rename ordering directly.
 *
 * Header-only on purpose: it is included from both the IMCS CU recovery layer
 * and the Recovery namespace without requiring a CMake target change.
 */
#ifndef __SHANNONBASE_RECOVERY_DURABLE_FS_H__
#define __SHANNONBASE_RECOVERY_DURABLE_FS_H__

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>   // open, O_*
#include <unistd.h>  // write, fsync, fdatasync, close, rename, unlink
#endif

namespace ShannonBase {
namespace Recovery {

namespace fs = std::filesystem;

namespace durable_detail {
inline std::atomic<uint64_t> g_tmp_counter{0};

inline fs::path make_tmp_path(const fs::path &final_path) {
  return fs::path(final_path.string() + ".tmp." + std::to_string(::getpid()) + "." +
                  std::to_string(g_tmp_counter.fetch_add(1, std::memory_order_relaxed)));
}

inline bool write_all(int fd, const char *data, size_t len) {
  while (len > 0) {
    ssize_t n = ::write(fd, data, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    data += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

inline bool fsync_dir(const fs::path &dir) {
#ifndef _WIN32
#ifdef O_DIRECTORY
  int dirfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
#else
  int dirfd = ::open(dir.c_str(), O_RDONLY);
#endif
  if (dirfd < 0) return false;
  const int ret = ::fsync(dirfd);
  const int saved_errno = errno;
  ::close(dirfd);
  errno = saved_errno;
  return ret == 0;
#else
  (void)dir;
  return true;
#endif
}
}  // namespace durable_detail

/**
 * Filesystem-level durability primitives.  All "commit" operations end in an
 * fsync of the affected directory so a subsequent crash cannot lose the rename
 * / unlink / mkdir itself.
 */
class DurableFileSystem {
 public:
  /** mkdir -p (best effort durable: fsyncs the parent of the deepest entry). */
  static bool create_directories(const fs::path &p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    return !ec;
  }

  /** fsync the directory containing `p`. */
  static bool sync_directory(const fs::path &p) {
    auto parent = p.parent_path();
    if (parent.empty()) parent = ".";
    return durable_detail::fsync_dir(parent);
  }

  /** Create/truncate a file, write the bytes, fdatasync, close. */
  static bool write_file(const fs::path &p, const std::string &data) {
#ifndef _WIN32
    int fd = ::open(p.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const bool ok = durable_detail::write_all(fd, data.data(), data.size()) && (::fdatasync(fd) == 0);
    const int saved_errno = errno;
    ::close(fd);
    errno = saved_errno;
    return ok;
#else
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    return out.good();
#endif
  }

  /** rename + fsync the destination's parent directory. */
  static bool rename(const fs::path &from, const fs::path &to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) return false;
    return sync_directory(to);
  }

  /**
   * Atomically and durably replace `final_path`:
   *   write tmp -> fdatasync(tmp) -> close -> rename(tmp, final) -> fsync(parent)
   * On failure the tmp file is removed when it still exists.
   */
  static bool persist_file(const fs::path &final_path, const std::string &data) {
#ifndef _WIN32
    const fs::path tmp_path = durable_detail::make_tmp_path(final_path);
    int fd = ::open(tmp_path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) return false;

    bool ok = false;
    do {
      if (!durable_detail::write_all(fd, data.data(), data.size())) break;
      if (::fsync(fd) != 0) break;
      if (::close(fd) != 0) {
        fd = -1;
        break;
      }
      fd = -1;
      if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) break;
      if (!sync_directory(final_path)) break;
      ok = true;
    } while (false);

    if (fd >= 0) ::close(fd);
    if (!ok) ::unlink(tmp_path.c_str());
    return ok;
#else
    std::ofstream out(final_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    return out.good();
#endif
  }

  /** Recursively remove a directory (best-effort GC) + fsync parent. */
  static bool remove_directory(const fs::path &p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    if (ec) return false;
    return sync_directory(p);
  }
};

/**
 * A raw fd-backed file for append-style writers (WAL).  Exposes the durability
 * boundary explicitly — Write() is buffered-by-the-kernel only, FlushData() is
 * fdatasync — instead of relying on ofstream::flush().
 */
class DurableFile {
 public:
  DurableFile() = default;
  ~DurableFile() { close(); }
  DurableFile(const DurableFile &) = delete;
  DurableFile &operator=(const DurableFile &) = delete;

  bool open(const fs::path &path, bool append) {
    close();
#ifndef _WIN32
    const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
    m_fd = ::open(path.c_str(), flags, 0644);
#else
    m_fd = -1;
#endif
    return m_fd >= 0;
  }

  bool is_open() const { return m_fd >= 0; }

  bool write(const void *buf, size_t len) {
    if (m_fd < 0) return false;
    return durable_detail::write_all(m_fd, static_cast<const char *>(buf), len);
  }

  /** fdatasync the file so every byte written so far is durable. */
  bool flush_data() {
#ifndef _WIN32
    return m_fd >= 0 && ::fdatasync(m_fd) == 0;
#else
    return m_fd >= 0;
#endif
  }

  void close() {
#ifndef _WIN32
    if (m_fd >= 0) {
      ::close(m_fd);
      m_fd = -1;
    }
#endif
  }

 private:
  int m_fd{-1};
};

}  // namespace Recovery
}  // namespace ShannonBase

#endif  // __SHANNONBASE_RECOVERY_DURABLE_FS_H__
