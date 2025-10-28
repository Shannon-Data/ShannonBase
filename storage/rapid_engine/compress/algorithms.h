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

namespace ShannonBase {
namespace Compress {

enum class Encoding_type { NONE, SORTED, VARLEN };
enum class compress_algos { DEFAULT = 0, NONE, ZLIB, ZSTD, LZ4 };
enum class Compression_level { DEFAULT = 0, NONE, ZLIB, ZSTD, LZ4 };

class Compress_algorithm {
 public:
  Compress_algorithm() = default;
  virtual ~Compress_algorithm() = default;
  virtual std::string &compressString(std::string &original) = 0;
  virtual std::string &decompressString(const std::string &compressed_str) = 0;
  static constexpr uint MAX_BUFF_LEN = 65535;
};

class zstd_compress : public Compress_algorithm {
 public:
  zstd_compress();
  virtual ~zstd_compress() = default;
  virtual std::string &compressString(std::string &original) final;
  virtual std::string &decompressString(const std::string &compressed_str) final;

 private:
  char m_buffer[Compress_algorithm::MAX_BUFF_LEN] = {0};
  std::string m_result;
};

class zlib_compress : public Compress_algorithm {
 public:
  zlib_compress();
  virtual ~zlib_compress() = default;
  virtual std::string &compressString(std::string &original) final;
  virtual std::string &decompressString(const std::string &compressed_str) final;

 private:
  char m_buffer[Compress_algorithm::MAX_BUFF_LEN] = {0};
  std::string m_result;
};

class lz4_compress : public Compress_algorithm {
 public:
  lz4_compress();
  virtual ~lz4_compress() = default;
  virtual std::string &compressString(std::string &original) final;
  virtual std::string &decompressString(const std::string &compressed_str) final;

 private:
  std::string m_result;
};

class CompressFactory {
 public:
  static std::unique_ptr<Compress_algorithm> get_instance(compress_algos algo) {
    std::call_once(m_alg_once, [&] { m_factory_instance = std::make_unique<CompressFactory>(); });
    return create_algorithm_instance(algo);
  }

  static std::unique_ptr<Compress_algorithm> get_instance(Encoding_type type) {
    std::call_once(m_alg_once, [&] { m_factory_instance = std::make_unique<CompressFactory>(); });
    return create_algorithm_instance(type);
  }

  using AlgorithmFactoryT = std::vector<std::unique_ptr<Compress_algorithm>>;

  CompressFactory() = default;
  virtual ~CompressFactory() = default;

 private:
  CompressFactory(CompressFactory &&) = delete;
  CompressFactory(CompressFactory &) = delete;
  CompressFactory &operator=(const CompressFactory &) = delete;
  CompressFactory &operator=(const CompressFactory &&) = delete;

  static std::unique_ptr<Compress_algorithm> create_algorithm_instance(compress_algos algo) {
    switch (algo) {
      case compress_algos::LZ4:
        return std::move(std::make_unique<lz4_compress>());
      case compress_algos::ZLIB:
        return std::move(std::make_unique<zlib_compress>());
      case compress_algos::ZSTD:
      case compress_algos::DEFAULT:
      case compress_algos::NONE:
      default:
        return std::move(std::make_unique<zstd_compress>());
    }
  }

  static std::unique_ptr<Compress_algorithm> create_algorithm_instance(Encoding_type type) {
    switch (type) {
      case Encoding_type::SORTED:
        return std::move(std::make_unique<zstd_compress>());
      case Encoding_type::VARLEN:
        return std::move(std::make_unique<lz4_compress>());
      case Encoding_type::NONE:
      default:
        return std::move(std::make_unique<zstd_compress>());
    }
  }

 private:
  static std::once_flag m_alg_once;
  static std::unique_ptr<CompressFactory> m_factory_instance;
};

}  // namespace Compress
}  // namespace ShannonBase

#endif  //__SHANNONBASE_COMPRESS_ALGORITHMS_H__