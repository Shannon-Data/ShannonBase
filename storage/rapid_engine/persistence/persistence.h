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
#ifndef __SHANNONBASE_PERSISTENCE_H__
#define __SHANNONBASE_PERSISTENCE_H__
#include <atomic>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "include/my_inttypes.h"
#include "storage/rapid_engine/include/rapid_const.h"

namespace ShannonBase {
namespace Imcs {
class Chunk;
class ChunkPersister {
 public:
  ChunkPersister(const std::string &wal_path, const std::string &checkpoint_path);
  ~ChunkPersister();

  void save_chunk(const Chunk *chunk, const std::string &path);
  void load_chunk(Chunk *chunk, const std::string &path);

  void begin_transaction();
  void commit_transaction();

  void rollback_transaction();
  void recover();

  void create_checkpoint();

 private:
  struct WALEntry {
    uint64_t transaction_id;
    std::string operation;  // "INSERT", "UPDATE", "DELETE"
    std::string chunk_key;
    row_id_t row_id;
    std::vector<uchar> data;
  };

  void write_wal(const WALEntry &entry);

  std::vector<WALEntry> read_wal();

  void load_from_checkpoint();

  std::string wal_path_;
  std::string checkpoint_path_;
  std::atomic<uint64_t> transaction_id_;
  std::ofstream wal_file_;
  bool in_transaction_;
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_PERSISTENCE_H__
