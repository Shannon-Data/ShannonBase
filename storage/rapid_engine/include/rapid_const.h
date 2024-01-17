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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#ifndef __SHANNONBASE_CONST_H__
#define __SHANNONBASE_CONST_H__

#include "my_inttypes.h"
namespace ShannonBase{

//the version of shannonbase.
constexpr uint SHANNONBASE_VERSION = 0x1;
constexpr uint SHANNON_RAPID_VERSION = 0x0001;

//unit of shannonbase.
constexpr uint64 SHANNON_KB = 1024;
constexpr uint64 SHANNON_MB = SHANNON_KB * 1024;
constexpr uint64 SHANNON_GB = SHANNON_MB * 1024;

//some sizes used by imcs.
constexpr uint64 SHANNON_CHUNK_SIZE = 32 * SHANNON_MB;
constexpr uint64 SHANNON_DEFAULT_MEMRORY_SIZE = 8 * SHANNON_GB;
constexpr uint64 SHANNON_MAX_MEMRORY_SIZE = SHANNON_DEFAULT_MEMRORY_SIZE;
constexpr uint64 SHANNON_DEFAULT_POPULATION_BUFFER_SIZE = 8 * SHANNON_MB;
constexpr uint64 SHANNON_MAX_POPULATION_BUFFER_SIZE = 8 * SHANNON_MB;

//these mask used for get meta info of data. infos is var according the data length we write,
constexpr uint DATA_DELETE_FLAG_MASK = 0x80;
constexpr uint DATA_NULL_FLAG_MASK =0x40;

constexpr uint SHANNON_MAGIC_IMCS = 0x0001;
constexpr uint SHANNON_MAGIC_IMCU = 0x0002;
constexpr uint SHANNON_MAGIC_CU = 0x0003;
constexpr uint SHANNON_MAGIC_CHUNK = 0x0004;

//This is use for Rapid cluster in future. in next, we will build up a AP clust for ShannonBase.
enum class RPD_NODE_ROLE {
  //meta node and primary role, name node.
  NODE_PRIMARY_NODE = 0,
  //secondary node: data node
  NODE_SECONDARY_NODE
};

} //ns:shannonbase
#endif //__SHANNONBASE_CONST_H__
