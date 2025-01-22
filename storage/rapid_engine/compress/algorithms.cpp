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

std::once_flag CompressFactory::m_alg_once;
CompressFactory *CompressFactory::m_factory_instance = nullptr;

zstd_compress::zstd_compress() { m_result.reserve(Compress_algorithm::MAX_BUFF_LEN); }

std::string &zstd_compress::compressString(std::string &orginal) {
  size_t inputSize = orginal.size();
  size_t compressedBufferSize = ZSTD_compressBound(inputSize);
  if (compressedBufferSize <= 0) return m_result.assign("error");

  memset(m_buffer, 0, Compress_algorithm::MAX_BUFF_LEN);
  size_t compressedSize = ZSTD_compress(m_buffer, compressedBufferSize, orginal.c_str(), inputSize, 1);
  if (ZSTD_isError(compressedSize)) return m_result.assign("error");
  m_result.assign(m_buffer, compressedSize);
  return m_result;
}

// TODO: to reduce the copy operations on string.
std::string &zstd_compress::decompressString(std::string &compressed_str) {
  size_t compressedSize = compressed_str.size();
  size_t decompressedBufferSize = ZSTD_getFrameContentSize(compressed_str.c_str(), compressedSize);

  memset(m_buffer, 0, Compress_algorithm::MAX_BUFF_LEN);
  size_t decompressedSize = ZSTD_decompress(m_buffer, decompressedBufferSize, compressed_str.c_str(), compressedSize);

  if (ZSTD_isError(decompressedSize)) return m_result.assign("error");
  m_result.assign(m_buffer, decompressedSize);
  return m_result;
}

zlib_compress::zlib_compress() { m_result.reserve(Compress_algorithm::MAX_BUFF_LEN); }

std::string &zlib_compress::compressString(std::string &orginal) {
  if (!orginal.size() || !orginal.c_str()) m_result.assign("error");

  int compressionLevel = Z_BEST_COMPRESSION;
  z_stream zStream;
  zStream.zalloc = Z_NULL;
  zStream.zfree = Z_NULL;
  zStream.opaque = Z_NULL;
  zStream.avail_in = orginal.size();
  zStream.next_in = (Bytef *)(orginal.c_str());

  if (deflateInit(&zStream, compressionLevel) != Z_OK) {
    return m_result.assign("error");
  }

  memset(m_buffer, 0, Compress_algorithm::MAX_BUFF_LEN);
  do {
    zStream.avail_out = Compress_algorithm::MAX_BUFF_LEN;
    zStream.next_out = (Bytef *)m_buffer;
    if (deflate(&zStream, Z_FINISH) == Z_STREAM_ERROR) {
      deflateEnd(&zStream);
      return m_result.assign("error");
    }

    m_result.append(m_buffer, Compress_algorithm::MAX_BUFF_LEN - zStream.avail_out);
  } while (zStream.avail_out == 0);

  deflateEnd(&zStream);
  return m_result;
}

std::string &zlib_compress::decompressString(std::string &compressed_str) {
  z_stream zStream;
  zStream.zalloc = Z_NULL;
  zStream.zfree = Z_NULL;
  zStream.opaque = Z_NULL;
  zStream.avail_in = compressed_str.size();
  zStream.next_in = (Bytef *)(compressed_str.c_str());

  if (inflateInit(&zStream) != Z_OK) return m_result.assign("error");

  memset(m_buffer, 0, Compress_algorithm::MAX_BUFF_LEN);
  do {
    zStream.avail_out = Compress_algorithm::MAX_BUFF_LEN;
    zStream.next_out = (Bytef *)m_buffer;
    if (inflate(&zStream, Z_NO_FLUSH) == Z_STREAM_ERROR) {
      inflateEnd(&zStream);
      return m_result.assign("error");
    }
    m_result.append(m_buffer, Compress_algorithm::MAX_BUFF_LEN - zStream.avail_out);
  } while (zStream.avail_out == 0);

  inflateEnd(&zStream);
  return m_result;
}

lz4_compress::lz4_compress() { m_result.reserve(Compress_algorithm::MAX_BUFF_LEN); }

std::string &lz4_compress::compressString(std::string &original) {
  int maxCompressedSize = LZ4_compressBound(original.size());
  if (maxCompressedSize == 0) return m_result.assign("error");

  std::unique_ptr<char[]> comp_data = std::make_unique<char[]>(maxCompressedSize);
  memset(comp_data.get(), 0, maxCompressedSize);
  int compressedSize = LZ4_compress_default(original.c_str(), comp_data.get(), original.size(), maxCompressedSize);
  if (!compressedSize) return m_result.assign("error");
  m_result.assign(comp_data.get(), compressedSize);
  return m_result;
}

std::string &lz4_compress::decompressString(std::string &compressed_str) {
  auto original_size = LZ4_compressBound(compressed_str.size());

  std::unique_ptr<char[]> comp_data = std::make_unique<char[]>(original_size);
  memset(comp_data.get(), 0, original_size);
  int decompressedSize =
      LZ4_decompress_safe(compressed_str.data(), comp_data.get(), compressed_str.size(), original_size);
  if (decompressedSize <= 0) return m_result.assign("error");

  m_result.assign(comp_data.get(), decompressedSize);
  return m_result;
}

void CompressFactory::make_elements() {
  // important: the inserted index should same as its algo type value.
  m_factory.emplace_back(std::make_unique<zstd_compress>());
  m_factory.emplace_back(std::make_unique<zstd_compress>());
  m_factory.emplace_back(std::make_unique<zlib_compress>());
  m_factory.emplace_back(std::make_unique<zstd_compress>());
  m_factory.emplace_back(std::make_unique<lz4_compress>());
}

std::unique_ptr<Compress_algorithm> CompressFactory::get_instance(compress_algos algo) {
  std::call_once(m_alg_once, [&] { m_factory_instance = new CompressFactory(); });

  if (algo == compress_algos::DEFAULT || algo == compress_algos::NONE)
    return std::move(std::make_unique<zstd_compress>());
  else if (algo == compress_algos::LZ4)
    return std::move(std::make_unique<lz4_compress>());
  else if (algo == compress_algos::ZLIB)
    return std::move(std::make_unique<zlib_compress>());
  else if (algo == compress_algos::ZSTD)
    return std::move(std::make_unique<zstd_compress>());
  else
    assert(false);
}

}  // namespace Compress
}  // namespace ShannonBase