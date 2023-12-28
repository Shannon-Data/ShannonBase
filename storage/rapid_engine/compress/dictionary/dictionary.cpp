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

namespace ShannonBase{
namespace Compress {

uint32 Dictionary::Store(String& str) {
  //returns dictionary id.
  std::ostringstream oss;
  if (str.c_ptr()) {
    oss << str.c_ptr();
  }
  {
    std::unique_lock lk(m_content_mtx);
    std::string strv = oss.str();
    if (m_content.find(oss.str()) == m_content.end()){
      m_content.insert(std::make_pair<std::string, uint32>(oss.str(), m_content.size()));
    } else
      return m_content[oss.str()];
  }
  return m_content.size() -1;
}
uint32 Dictionary::Get(uint64 strid, String& val, CHARSET_INFO& charset) {
  std::shared_lock lk(m_content_mtx);
  for (auto it = m_content.begin(); it != m_content.end(); it++) {
    if (it->second == strid) {
      std::string strstr = it->first;
      String strs (strstr.c_str(), strstr.length(), &charset);
      copy_if_not_alloced(&val, &strs, strs.length());
      break;
    }
  }
  return 0;
}
} //ns:Compress
} //ns::shannonbase