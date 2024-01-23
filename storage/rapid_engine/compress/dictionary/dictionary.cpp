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

#include "include/ut0dbg.h"

#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/compress/algorithms.h"

namespace ShannonBase{
namespace Compress {

uint32 Dictionary::store(String& str, Encoding_type type) {
  DBUG_TRACE;
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

  std::string compressed_str(CompressFactory::get_instance(alg)->compressString(orgin_str));
  {
    std::unique_lock lk(m_content_mtx);
    if (m_content.find(compressed_str) == m_content.end()){ //insert new one.
      m_content_id.fetch_add(1,std::memory_order::memory_order_acq_rel);
      uint64 id = m_content_id.load(std::memory_order::memory_order_acq_rel);
      m_content.emplace(compressed_str, id);
      m_id2content.emplace(id, compressed_str);
      ut_a((m_content.size() == m_id2content.size()));
      ut_a(m_content_id == m_content.size());
      return m_content_id;
    } else
      return m_content[compressed_str];
  }

  return 0;
}
uint32 Dictionary::get(uint64 strid, String& val, CHARSET_INFO& charset) {
  compress_algos alg {compress_algos::NONE};
  switch (m_encoding_type) {
    case Encoding_type::SORTED: alg = compress_algos::ZSTD; break;
    case  Encoding_type::VARLEN:
    case Encoding_type::NONE:
      alg = compress_algos::LZ4;
    break;
    default: break;
  }

  {
    std::shared_lock lk(m_content_mtx);
    if (m_id2content.find(strid) != m_id2content.end()) {
      std::string compressed_str (m_id2content[strid]);
      std::string decom_str(CompressFactory::get_instance(alg)->decompressString(compressed_str));
      String strs (decom_str.c_str(), decom_str.length(), &charset);
      copy_if_not_alloced(&val, &strs, strs.length());
    }
  }
  return 0;
}
} //ns:Compress
} //ns::shannonbase