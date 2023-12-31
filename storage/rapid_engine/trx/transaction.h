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

   The fundmental code for imcs. for transaction.
*/
#include "include/my_inttypes.h"
namespace ShannonBase {
namespace Transaction {

/**This class is used for an interface of real implementation of trans.
Here, is used as an interface of innobase transaction. In future we can
use any transaction impl to replace innobase's trx used here.
*/
class Transaction {
 public:
   enum class ISOLATION_LEVEL : uint8 {READ_UNCOMMITTED, READ_COMMITTED, READ_REPEATABLE, SERIALIZABLE};
   enum class STATUS : uint8 {NOT_START, ACTIVE, PREPARED, COMMITTED_IN_MEMORY};
   Transaction () = default;
   virtual ~ Transaction() = default;
   virtual int Begin(ISOLATION_LEVEL iso_level = ISOLATION_LEVEL::READ_REPEATABLE);
   virtual int Commit ();
   virtual int Rollback();
 private:
  ISOLATION_LEVEL m_iso_level {ISOLATION_LEVEL::READ_REPEATABLE};
};

} //ns:transaction   
} //ns:shannonbase