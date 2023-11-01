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

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/

#ifndef __SHANNONBASE_IMCS_H__
#define __SHANNONBASE_IMCS_H__

#include <atomic>
#include <mutex>
#include <map>

#include "my_alloc.h"
#include "my_inttypes.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/include/rapid_object.h"

namespace ShannonBase{
namespace Imcs{

//this is used for allocate memory buffer at start of imcs to keep all the loaded data.
constexpr unsigned long MAX_MEMRORY_SIZE = 100;    
constexpr unsigned long DEFAULT_MEMRORY_SIZE = MAX_MEMRORY_SIZE;

//the memory size of allocation for imcs to store the loaded data.
extern unsigned long rapid_memory_size;

class Imcu;
class Imcs :public MemoryObject {
public:
 static Imcs* Get_instance(){
     std::call_once(one, [&] { m_instance = new Imcs();
                             });
    return m_instance;
 }
 uint Write(ShannonBaseContext* context, TransactionID trxid, Field* fields);
 uint Read (ShannonBaseContext* context, Field* field);

private:
 //make ctor and dctor private.
 Imcs();
 virtual ~Imcs();

 Imcs(Imcs&& ) = delete;
 Imcs(Imcs&) = delete;
 Imcs& operator = (const Imcs&) = delete;

 Imcu* New_imcu(const TABLE& table_arg);

 static Imcs* m_instance;
 static std::once_flag one;

 //used to keep all allocated imcus. key string: db_name + table_name.
 std::mutex m_imcu_mtx;
 std::map<std::string, Imcu*> m_imcus;
 //the last id of an imcu
 std::atomic<uint> m_cu_id {0};
};

} //ns: imcs
} //ns:shannonbase
#endif //__SHANNONBASE_IMCS_H__