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
#include <lz4.h>
#include <zlib.h>
#include <zstd.h>
#include <cstdint>
#include <cstring>
#include <limits>

#include "storage/innobase/include/ut0dbg.h"
#include "storage/rapid_engine/compress/algorithms.h"
#include "storage/rapid_engine/include/rapid_arch_inf.h"  // SHANNON_THREAD_LOCAL

namespace ShannonBase {
namespace Compress {
namespace {

/*
  LZ4 and raw zlib streams do not record how long the original data was, and
  the caller of the one-argument decompress() does not always know either --
  Dictionary::get() is handed a payload and nothing else. Guessing the size (it
  used to assume 4x the compressed length) fails outright on anything that
  compresses better than the guess, which for LZ4 means decompress() returns an
  empty string that Dictionary::get() cannot tell apart from a stored empty
  value, and that CU::deserialize() reports as snapshot corruption.

  So both formats carry the original length in a 4-byte little-endian prefix.
  Zstd needs none: its frame header already has one.
*/
constexpr size_t kSizePrefixBytes = 4;

void put_size_prefix(std::string &out, size_t size) {
  out.resize(kSizePrefixBytes);
  for (size_t i = 0; i < kSizePrefixBytes; ++i) out[i] = static_cast<char>((size >> (8 * i)) & 0xFF);
}

/*
  Largest output a single byte of compressed payload can legitimately produce.
  Deflate's theoretical ceiling is 1032:1; an LZ4 block's is 255:1, because
  extending a match past 19 bytes costs one 0xFF byte per further 255.

  Without a ceiling the prefix is an unbounded allocation request written by
  the payload itself: a truncated stripe, a payload from a build that predates
  the prefix, or plain corruption would have its first four bytes read as a
  length and resize() would try for up to 4 GiB before the decompressor got a
  chance to reject it.
*/
constexpr uint64_t kLz4MaxExpansion = 256;
constexpr uint64_t kZlibMaxExpansion = 1032;

/**
  Read the original-length prefix and check it against what @a data could
  plausibly expand to.

  @param max_expansion  Format's maximum output-to-input ratio.
  @return false when @a data is too short to carry a prefix, or the length it
          carries is not reachable from this much payload.
*/
bool read_size_prefix(std::string_view data, uint64_t max_expansion, uint32_t &size, std::string_view &payload) {
  if (data.size() < kSizePrefixBytes) return false;
  size = 0;
  for (size_t i = 0; i < kSizePrefixBytes; ++i)
    size |= static_cast<uint32_t>(static_cast<unsigned char>(data[i])) << (8 * i);
  payload = data.substr(kSizePrefixBytes);
  if (static_cast<uint64_t>(size) > payload.size() * max_expansion) return false;
  return true;
}

/** Inputs are CU stripes and dictionary values; the prefix caps them at 4 GiB. */
bool size_fits_prefix(size_t size) { return size <= std::numeric_limits<uint32_t>::max(); }

/**
  Inflate @a payload into a buffer already sized to the original length.

  @return bytes written, or 0 on any error. A single inflate() call suffices
          precisely because the destination is the exact original size.
*/
size_t inflate_into(std::string_view payload, char *buf, size_t buf_len) {
  z_stream zs{};
  if (inflateInit(&zs) != Z_OK) return 0;

  zs.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(payload.data()));
  zs.avail_in = static_cast<uInt>(payload.size());
  zs.next_out = reinterpret_cast<Bytef *>(buf);
  zs.avail_out = static_cast<uInt>(buf_len);

  const int ret = inflate(&zs, Z_FINISH);
  const size_t written = zs.total_out;
  inflateEnd(&zs);
  return (ret == Z_STREAM_END) ? written : 0;
}

}  // namespace
std::string ZstdCompressor::compress(std::string_view data) const {
  if (data.empty()) return {};
  const size_t max = ZSTD_compressBound(data.size());
  std::string out(max, '\0');
  const size_t sz = ZSTD_compressCCtx(m_cctx, out.data(), max, data.data(), data.size(), 3);
  if (ZSTD_isError(sz)) return {};
  out.resize(sz);
  return out;
}

std::string ZstdCompressor::decompress(std::string_view data) const {
  if (data.empty()) return {};
  const unsigned long long dsize = ZSTD_getFrameContentSize(data.data(), data.size());
  if (dsize == ZSTD_CONTENTSIZE_UNKNOWN || dsize == ZSTD_CONTENTSIZE_ERROR) return {};
  // The length comes out of the payload, so a corrupt frame header would
  // otherwise size the allocation.
  if (!size_fits_prefix(static_cast<size_t>(dsize))) return {};

  std::string out;
  try {
    out.resize(static_cast<size_t>(dsize));
  } catch (const std::bad_alloc &) {
    return {};
  }
  const size_t sz = ZSTD_decompressDCtx(m_dctx, out.data(), out.size(), data.data(), data.size());
  if (ZSTD_isError(sz)) return {};
  out.resize(sz);
  return out;
}

size_t ZstdCompressor::decompress(std::string_view data, char *buf, size_t buf_len) const {
  if (data.empty()) return 0;
  const size_t sz = ZSTD_decompressDCtx(m_dctx, buf, buf_len, data.data(), data.size());
  return ZSTD_isError(sz) ? 0 : sz;
}

std::string Lz4Compressor::compress(std::string_view data) const {
  if (data.empty() || !size_fits_prefix(data.size())) return {};
  // LZ4's int-typed API cannot describe an input this large.
  if (data.size() > static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) return {};

  const int max = LZ4_compressBound(static_cast<int>(data.size()));
  std::string out;
  put_size_prefix(out, data.size());
  out.resize(kSizePrefixBytes + static_cast<size_t>(max), '\0');

  const int sz = LZ4_compress_default(data.data(), out.data() + kSizePrefixBytes, static_cast<int>(data.size()), max);
  if (sz <= 0) return {};
  out.resize(kSizePrefixBytes + static_cast<size_t>(sz));
  return out;
}

std::string Lz4Compressor::decompress(std::string_view data) const {
  uint32_t original_size = 0;
  std::string_view payload;
  if (!read_size_prefix(data, kLz4MaxExpansion, original_size, payload)) return {};
  if (original_size == 0) return {};

  std::string out;
  try {
    out.resize(original_size);
  } catch (const std::bad_alloc &) {
    return {};
  }
  const int sz =
      LZ4_decompress_safe(payload.data(), out.data(), static_cast<int>(payload.size()), static_cast<int>(out.size()));
  if (sz < 0 || static_cast<size_t>(sz) != out.size()) return {};
  return out;
}

size_t Lz4Compressor::decompress(std::string_view data, char *buf, size_t buf_len) const {
  uint32_t original_size = 0;
  std::string_view payload;
  if (!read_size_prefix(data, kLz4MaxExpansion, original_size, payload)) return 0;
  if (original_size == 0 || original_size > buf_len) return 0;

  const int sz =
      LZ4_decompress_safe(payload.data(), buf, static_cast<int>(payload.size()), static_cast<int>(original_size));
  return (sz < 0 || static_cast<size_t>(sz) != original_size) ? 0 : static_cast<size_t>(sz);
}

std::string ZlibCompressor::compress(std::string_view data) const {
  if (data.empty() || !size_fits_prefix(data.size())) return {};
  z_stream zs{};
  if (deflateInit(&zs, Z_BEST_COMPRESSION) != Z_OK) return {};

  zs.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data.data()));
  zs.avail_in = static_cast<uInt>(data.size());

  // deflateBound() is the only safe destination size: data.size() + 256 is too
  // small for incompressible input, and deflate() then stops at Z_OK rather
  // than Z_STREAM_END and the whole call fails.
  const uLong bound = deflateBound(&zs, static_cast<uLong>(data.size()));
  std::string out;
  put_size_prefix(out, data.size());
  out.resize(kSizePrefixBytes + bound, '\0');

  // avail_out must describe the region the string actually guarantees, which
  // is size(), not capacity().
  zs.next_out = reinterpret_cast<Bytef *>(out.data() + kSizePrefixBytes);
  zs.avail_out = static_cast<uInt>(bound);

  if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
    deflateEnd(&zs);
    return {};
  }
  out.resize(kSizePrefixBytes + zs.total_out);
  deflateEnd(&zs);
  return out;
}

std::string ZlibCompressor::decompress(std::string_view data) const {
  uint32_t original_size = 0;
  std::string_view payload;
  if (!read_size_prefix(data, kZlibMaxExpansion, original_size, payload)) return {};
  if (original_size == 0) return {};

  std::string out;
  try {
    out.resize(original_size);
  } catch (const std::bad_alloc &) {
    return {};
  }
  if (inflate_into(payload, out.data(), out.size()) != out.size()) return {};
  return out;
}

size_t ZlibCompressor::decompress(std::string_view data, char *buf, size_t buf_len) const {
  uint32_t original_size = 0;
  std::string_view payload;
  if (!read_size_prefix(data, kZlibMaxExpansion, original_size, payload)) return 0;
  if (original_size == 0 || original_size > buf_len) return 0;

  return inflate_into(payload, buf, original_size) == original_size ? original_size : 0;
}

static SHANNON_THREAD_LOCAL auto tl_zstd = std::make_unique<ZstdCompressor>();
static SHANNON_THREAD_LOCAL auto tl_lz4 = std::make_unique<Lz4Compressor>();
static SHANNON_THREAD_LOCAL auto tl_zlib = std::make_unique<ZlibCompressor>();

CompressAlgorithm *get_compressor(ENCODING_TYPE type) {
  switch (type) {
    case ENCODING_TYPE::VARLEN:
      return tl_lz4.get();
    case ENCODING_TYPE::SORTED:
    case ENCODING_TYPE::NONE:
    default:
      return tl_zstd.get();
  }
}

CompressAlgorithm *get_compressor(COMPRESS_ALGO algo) {
  switch (algo) {
    case COMPRESS_ALGO::LZ4:
      return tl_lz4.get();
    case COMPRESS_ALGO::ZLIB:
      return tl_zlib.get();
    case COMPRESS_ALGO::ZSTD:
    case COMPRESS_ALGO::DEFAULT:
    default:
      return tl_zstd.get();
  }
}
}  // namespace Compress
}  // namespace ShannonBase