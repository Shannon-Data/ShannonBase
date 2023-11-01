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

#define KB 1024
#define MB (KB * KB)

using  TransactionID = uint64_t;

//The max num of rows in a chunk.
constexpr uint MAX_NUM_CHUNK_ROWS = 65535;
//The max num of cus in an imcu.
constexpr uint MAX_NUM_CUS = 150;
//The max num of chunks in a CU
constexpr uint MAX_CHUNK_NUMS = 128;
//The max num of IMCU dims.
constexpr uint MAX_NUM_IMCU_DIMS = 1024;
//The max row width
constexpr uint MAX_ROW_LEN = 512;
//This is use for Rapid cluster in future.
enum class RPD_NODE_ROLE {
  //meta node and primary role, name node.
  NODE_PRIMARY_NODE = 0,
  //secondary node: data node
  NODE_SECONDARY_NODE
};

//these mask used for get meta info of data.
//infos is var according the data length we write,
constexpr uint DATA_BYTES_FLAG_MASK =0x0011;
constexpr uint DATA_DELETE_FLAG_MASK = 0x0;
constexpr uint DATA_NULL_FLAG_MASK =0X0;
constexpr uint DATA_LEN_FLAG_MASK =0x0;
constexpr uint SHANNON_RAPID_VERSION = 0x1;
constexpr uint SHANNONBASE_VERSION = 0x1;
constexpr uint8 SHANNON_MAGIC_IMCS = 0x0001;
constexpr uint8 SHANNON_MAGIC_IMCU = 0x0002;
constexpr uint8 SHANNON_MAGIC_CU = 0x0003;
constexpr uint8 SHANNON_MAGIC_CHUNK = 0x0004;

} //ns:shannonbase
#endif //__SHANNONBASE_CONST_H__
