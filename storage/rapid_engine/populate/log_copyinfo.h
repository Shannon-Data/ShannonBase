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

   Copyright (c) 2023, 2024, 2025,  Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.
*/
#ifndef __SHANNONBASE_LOG_COPY_INFO_PARSER_H__
#define __SHANNONBASE_LOG_COPY_INFO_PARSER_H__

#include "storage/rapid_engine/include/rapid_context.h"  //Rapid_load_context
namespace ShannonBase {
namespace Populate {
/**
 * To parse the copy_info, it used to populate the changes from ionnodb
 * to rapid.
 */
class CopyInfoParser {
 public:
  CopyInfoParser() = default;
  ~CopyInfoParser() = default;

  // store the field infor in mysql format.
  uint parse_copy_info(Rapid_load_context *context, byte *ptr, byte *end_ptr);
};
}  // namespace Populate
}  // namespace ShannonBase
#endif  //__SHANNONBASE_LOG_COPY_INFO_PARSER_H__
