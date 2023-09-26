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

#include <mutex>
#include <map>

#include "my_alloc.h"
#include "my_inttypes.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/cu.h"

namespace ShannonBase{
namespace Imcs{

//this is used for allocate memory buffer at start of imcs to keep all the loaded data.
constexpr unsigned long MAX_MEMRORY_SIZE = 100;    
constexpr unsigned long DEFAULT_MEMRORY_SIZE = MAX_MEMRORY_SIZE;

//the memory size of allocation for imcs to store the loaded data.
extern unsigned long rapid_memory_size;

class Memory_object{

};

class Imcu;

class Imcs :public Memory_object {
public:
 static Imcs* Get_instance(){
     std::call_once(one, [&] { m_instance = new Imcs();
                             });
    return m_instance;
 }

 //init and deinit.
 uint Initialization(MEM_ROOT* mem_root);
 uint Deinitialization();
 bool IsInitialized () { return m_initialized; }

 //Gets a new IMCU handler and insert into
 Imcu* Allocate_imcu(MEM_ROOT* mem_root, const char* db_name, const char* table_name);
 uint  Deallcate_imcu(const char* db_name, const char* table_name);
 Imcu* Get_imcu(const char* db_name, const char* table_name);
 inline uint Get_num_imcu() { return m_imcus.size(); }
private:
 //make ctor and dctor private.
 Imcs() {}
 virtual ~Imcs() {}

 Imcs(Imcs&& ) = delete;
 Imcs(Imcs&) = delete;
 Imcs& operator = (const Imcs&) = delete;

 bool m_initialized {false};
 static Imcs* m_instance;
 static std::once_flag one;

 //used to keep all allocated imcus
 std::map<std::string, Imcu*> m_imcus;
 std::mutex m_imcu_mtx;
};

} //ns: imcs
} //ns:shannonbase
#endif //__SHANNONBASE_IMCS_H__