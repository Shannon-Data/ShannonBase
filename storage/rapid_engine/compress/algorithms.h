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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_COMPRESS_ALGORITHMS_H__
#define __SHANNONBASE_COMPRESS_ALGORITHMS_H__
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <zlib.h>
#include <zstd.h>
namespace ShannonBase {
namespace Compress {

enum class ENCODING_TYPE { NONE, SORTED, VARLEN };
enum class COMPRESS_ALGO { DEFAULT = 0, NONE, ZLIB, ZSTD, LZ4 };
enum class COMPRESS_LEVEL { DEFAULT = 0, NONE, ZLIB, ZSTD, LZ4 };

class CompressAlgorithm {
 public:
  virtual ~CompressAlgorithm() = default;
  virtual std::string compress(std::string_view data) const = 0;
  virtual std::string decompress(std::string_view data) const = 0;
  virtual size_t decompress(std::string_view data, char *buf, size_t buf_len) const {
    auto s = decompress(data);
    if (s.empty()) return 0;
    size_t n = std::min(s.size(), buf_len);
    std::memcpy(buf, s.data(), n);
    return n;
  }
};

CompressAlgorithm *get_compressor(ENCODING_TYPE type);
CompressAlgorithm *get_compressor(COMPRESS_ALGO algo);

class ZstdCompressor final : public CompressAlgorithm {
 private:
  ZSTD_DCtx *m_dctx{nullptr};
  ZSTD_CCtx *m_cctx{nullptr};

 public:
  ZstdCompressor() {
    m_dctx = ZSTD_createDCtx();
    m_cctx = ZSTD_createCCtx();
    assert(m_dctx && m_cctx);
  }
  ~ZstdCompressor() {
    ZSTD_freeDCtx(m_dctx);
    ZSTD_freeCCtx(m_cctx);
  }
  std::string compress(std::string_view data) const override;
  std::string decompress(std::string_view data) const override;
  size_t decompress(std::string_view data, char *buf, size_t buf_len) const override;
};

class Lz4Compressor final : public CompressAlgorithm {
 public:
  std::string compress(std::string_view data) const override;
  std::string decompress(std::string_view data) const override;
  size_t decompress(std::string_view data, char *buf, size_t buf_len) const override;
};

class ZlibCompressor final : public CompressAlgorithm {
 public:
  std::string compress(std::string_view data) const override;
  std::string decompress(std::string_view data) const override;
  size_t decompress(std::string_view data, char *buf, size_t buf_len) const override;
};
}  // namespace Compress
}  // namespace ShannonBase

#endif  //__SHANNONBASE_COMPRESS_ALGORITHMS_H__