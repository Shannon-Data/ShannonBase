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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/

#include "storage/rapid_engine/imcs/imcs.h"

namespace ShannonBase{
namespace Imcs{

unsigned long rapid_memory_size {0};

Imcs* Imcs::m_instance {nullptr};
std::once_flag Imcs::one;

uint Imcs::Initialization(MEM_ROOT* mem_root)
{
  if (mem_root) return true;

  return false;
}
uint Imcs::Deinitialization()
{
  return false;
}

Imcu* Alloc_imcu(MEM_ROOT* mem_root, const char* db_name, const char* table_name, const char* column_name)
{
    if (!mem_root) return nullptr;

    Imcu* rapid_imcu {nullptr};

    std::string cu_key;
    cu_key += db_name;
    cu_key += table_name;
    cu_key += column_name;

    return rapid_imcu;
}

Imcu* Imcs::Get_imcu(const char* db_name, const char* table_name, const char* column_name)
{
    std::string cu_key;
    cu_key += db_name;
    cu_key += table_name;
    cu_key += column_name;

    Imcu* rapid_cu {nullptr};
    return rapid_cu;
}

} //ns:imcs 
} //ns:shannonbase