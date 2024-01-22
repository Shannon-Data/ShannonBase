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
#include "extra/zstd/zstd-1.5.5/lib/zstd.h"
#include "extra/zlib/zlib-1.2.13/zlib.h"
#include "extra/lz4/lz4-1.9.4/lib/lz4.h"

#include "storage/rapid_engine/compress/algorithms.h"

namespace ShannonBase {
namespace Compress {

std::once_flag CompressFactory::m_alg_once;
Compress_algorithm* CompressFactory::m_factory = nullptr;
std::string zstd_compress::compressString(std::string& orginal) {
   size_t inputSize = orginal.size();
   size_t compressedBufferSize = ZSTD_compressBound(inputSize);
   std::unique_ptr<char[]> compressedBuffer;
   compressedBuffer.reset (new char[compressedBufferSize]);
   size_t compressedSize = ZSTD_compress(compressedBuffer.get(), compressedBufferSize, orginal.c_str(), inputSize, 1);
   if (ZSTD_isError(compressedSize)) {
       //std::cerr << "Compression error: " << ZSTD_getErrorName(compressedSize) << std::endl;
       return "";
   }
   std::string compressedString(compressedBuffer.get(), compressedSize);
   return compressedString;
}
std::string zstd_compress::decompressString(std::string& compressed_str) {
   size_t compressedSize = compressed_str.size();
   size_t decompressedBufferSize = ZSTD_getFrameContentSize(compressed_str.c_str(), compressedSize);
   std::unique_ptr<char[]> decompressedBuffer;
   decompressedBuffer.reset(new char[decompressedBufferSize]);
   size_t decompressedSize = ZSTD_decompress(decompressedBuffer.get(), decompressedBufferSize, compressed_str.c_str(), compressedSize);

   if (ZSTD_isError(decompressedSize)) {
       //std::cerr << "Decompression error: " << ZSTD_getErrorName(decompressedSize) << std::endl;
       return "";
   }
   std::string decompressedString(decompressedBuffer.get(), decompressedSize);
   return decompressedString;
}
std::string zlib_compress::compressString(std::string& orginal) {
   int compressionLevel = Z_BEST_COMPRESSION;

   z_stream zStream;
   zStream.zalloc = Z_NULL;
   zStream.zfree = Z_NULL;
   zStream.opaque = Z_NULL;
   zStream.avail_in = orginal.size();
   zStream.next_in = (Bytef *)(orginal.c_str());

   if (deflateInit(&zStream, compressionLevel) != Z_OK) {
      return "";
   }

   const int bufferSize = 65535;
   std::unique_ptr<char[]> buffer(new char[bufferSize]);
   std::string compressedString;
   do {
       zStream.avail_out = bufferSize;
       zStream.next_out = (Bytef *)buffer.get();

       if (deflate(&zStream, Z_FINISH) == Z_STREAM_ERROR) {
           deflateEnd(&zStream);
           return "";
       }

       compressedString.append(buffer.get(), bufferSize - zStream.avail_out);
    } while (zStream.avail_out == 0);

    deflateEnd(&zStream);
    return compressedString;
}
std::string zlib_compress::decompressString(std::string& compressed_str) {
   z_stream zStream;
   zStream.zalloc = Z_NULL;
   zStream.zfree = Z_NULL;
   zStream.opaque = Z_NULL;
   zStream.avail_in = compressed_str.size();
   zStream.next_in = (Bytef *)(compressed_str.c_str());

   if (inflateInit(&zStream) != Z_OK) {
      return "";
   }

   const int bufferSize = 65535;
   std::unique_ptr<char[]> buffer(new char[bufferSize]);
   std::string decompressedString;
   do {
       zStream.avail_out = bufferSize;
       zStream.next_out = (Bytef *)buffer.get();
       if (inflate(&zStream, Z_NO_FLUSH) == Z_STREAM_ERROR) {
           inflateEnd(&zStream);
           return "";
       }
       decompressedString.append(buffer.get(), bufferSize - zStream.avail_out);
   } while (zStream.avail_out == 0);

   inflateEnd(&zStream);
   return decompressedString;
}
std::string lz4_compress::compressString(std::string& orginal) {
    size_t maxCompressedSize = LZ4_compressBound(orginal.size());
    std::string compressedData(maxCompressedSize, '\0');
    int compressedSize = LZ4_compress_default(orginal.c_str(), &compressedData[0], orginal.size(), maxCompressedSize);
    if (compressedSize <= 0) {
        return "";
    }
    compressedData.resize(compressedSize);
    return compressedData;
}
std::string lz4_compress::decompressString(std::string& compressed_str) {
   uint original_size = 65535;
    std::string decompressedData(original_size, '\0');
    int decompressedSize = LZ4_decompress_safe(compressed_str.c_str(), &decompressedData[0], compressed_str.size(), original_size);
    if (decompressedSize <= 0) {
        return "";
    }
    decompressedData.resize(decompressedSize);
    return decompressedData;
}

std::unique_ptr<Compress_algorithm> CompressFactory::get_instance(compress_algos algo) {
  switch (algo) {
   case compress_algos::ZLIB:
     return std::make_unique<zlib_compress>();
   break;
   case compress_algos::ZSTD:
     return std::make_unique<zstd_compress>();
   break;
   case compress_algos::LZ4:
     return std::make_unique<lz4_compress>();
   break;
   default: break;
  }
  return std::make_unique<default_compress>();
}

} //ns:compress
} //ns:shnnonbase

