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
#include <cstring>
#include "extra/lz4/lz4-1.10.0/lib/lz4.h"
#include "extra/zlib/zlib-1.3.1/zlib.h"
#include "extra/zstd/zstd-1.5.5/lib/zstd.h"

#include "storage/innobase/include/ut0dbg.h"
#include "storage/rapid_engine/compress/algorithms.h"

namespace ShannonBase {
namespace Compress {
class ZstdCompressor final : public CompressAlgorithm {
 public:
  std::string compress(std::string_view data) const override {
    if (data.empty()) return {};
    const size_t max = ZSTD_compressBound(data.size());
    std::string out(max, '\0');
    const size_t sz = ZSTD_compress(out.data(), max, data.data(), data.size(), 3);
    if (ZSTD_isError(sz)) return {};
    out.resize(sz);
    return out;
  }

  std::string decompress(std::string_view data) const override {
    if (data.empty()) return {};
    const unsigned long long dsize = ZSTD_getFrameContentSize(data.data(), data.size());
    if (dsize == ZSTD_CONTENTSIZE_UNKNOWN || dsize == ZSTD_CONTENTSIZE_ERROR) return {};

    std::string out(dsize, '\0');
    const size_t sz = ZSTD_decompress(out.data(), dsize, data.data(), data.size());
    if (ZSTD_isError(sz)) return {};
    out.resize(sz);
    return out;
  }
};

class Lz4Compressor final : public CompressAlgorithm {
 public:
  std::string compress(std::string_view data) const override {
    if (data.empty()) return {};
    const int max = LZ4_compressBound(static_cast<int>(data.size()));
    std::string out(max, '\0');
    const int sz = LZ4_compress_default(data.data(), out.data(), static_cast<int>(data.size()), max);
    if (sz <= 0) return {};
    out.resize(sz);
    return out;
  }

  std::string decompress(std::string_view data) const override {
    if (data.empty()) return {};
    std::string out(data.size() * 4, '\0');  // 保守估计
    const int sz =
        LZ4_decompress_safe(data.data(), out.data(), static_cast<int>(data.size()), static_cast<int>(out.capacity()));
    if (sz < 0) return {};
    out.resize(sz);
    return out;
  }
};

class ZlibCompressor final : public CompressAlgorithm {
 public:
  std::string compress(std::string_view data) const override {
    if (data.empty()) return {};
    z_stream zs{};
    if (deflateInit(&zs, Z_BEST_COMPRESSION) != Z_OK) return {};

    zs.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());

    std::string out(data.size() + 256, '\0');
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = static_cast<uInt>(out.capacity());

    if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
      deflateEnd(&zs);
      return {};
    }
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
  }

  std::string decompress(std::string_view data) const override {
    if (data.empty()) return {};
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return {};

    zs.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());

    std::string out(data.size() * 4, '\0');
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = static_cast<uInt>(out.capacity());

    const int ret = inflate(&zs, Z_NO_FLUSH);
    if (ret != Z_STREAM_END) {
      inflateEnd(&zs);
      return {};
    }
    out.resize(zs.total_out);
    inflateEnd(&zs);
    return out;
  }
};

static thread_local auto tl_zstd = std::make_unique<ZstdCompressor>();
static thread_local auto tl_lz4 = std::make_unique<Lz4Compressor>();
static thread_local auto tl_zlib = std::make_unique<ZlibCompressor>();

CompressAlgorithm *get_compressor(Encoding_type type) {
  switch (type) {
    case Encoding_type::VARLEN:
      return tl_lz4.get();
    case Encoding_type::SORTED:
    case Encoding_type::NONE:
    default:
      return tl_zstd.get();
  }
}

CompressAlgorithm *get_compressor(compress_algos algo) {
  switch (algo) {
    case compress_algos::LZ4:
      return tl_lz4.get();
    case compress_algos::ZLIB:
      return tl_zlib.get();
    case compress_algos::ZSTD:
    case compress_algos::DEFAULT:
    default:
      return tl_zstd.get();
  }
}
}  // namespace Compress
}  // namespace ShannonBase