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
#include <iostream>
#include <sstream>

#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/compress/algorithms.h"

namespace ShannonBase{
namespace Compress {

uint32 Dictionary::Store(String& str, Encoding_type type) {
  //returns dictionary id. //encoding alg pls ref to: heatwave document.
  std::string orgin_str(str.c_ptr());
  compress_algos alg {compress_algos::NONE};
  switch (m_encoding_type) {
    case Encoding_type::SORTED: alg = compress_algos::ZSTD; break;
    case Encoding_type::VARLEN:
    case Encoding_type::NONE:
      alg = compress_algos::LZ4;
    break;
    default: break;
  }

  std::string compressed_str = CompressFactory::GetInstance(alg)->compressString(orgin_str);

  {
    std::unique_lock lk(m_content_mtx);
    if (m_content.find(compressed_str) == m_content.end()){
      m_content.insert({compressed_str, m_content.size()});
    } else
      return m_content[compressed_str];
  }
  return m_content.size() -1;
}
uint32 Dictionary::Get(uint64 strid, String& val, CHARSET_INFO& charset) {
  compress_algos alg {compress_algos::NONE};
  switch (m_encoding_type) {
    case Encoding_type::SORTED: alg = compress_algos::ZSTD; break;
    case  Encoding_type::VARLEN:
    case Encoding_type::NONE:
      alg = compress_algos::LZ4;
    break;
    default: break;
  }

  std::shared_lock lk(m_content_mtx);
  for (auto it = m_content.begin(); it != m_content.end(); it++) {
    if (it->second == strid) {
      std::string compressed_str = it->first;
      std::string decom_str = CompressFactory::GetInstance(alg)->decompressString(compressed_str);
      String strs (decom_str.c_str(), decom_str.length(), &charset);
      copy_if_not_alloced(&val, &strs, strs.length());
      break;
    }
  }
  return 0;
}
} //ns:Compress
} //ns::shannonbase