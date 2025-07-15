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
#include "storage/rapid_engine/persistence/persistence.h"

#include <cstring>
#include <filesystem>
#include "storage/innobase/include/ut0dbg.h"  // ut_a

#include "storage/rapid_engine/imcs/chunk.h"
namespace ShannonBase {
namespace Imcs {
ChunkPersister::ChunkPersister(const std::string &wal_path, const std::string &checkpoint_path)
    : wal_path_(wal_path), checkpoint_path_(checkpoint_path), transaction_id_(0), in_transaction_(false) {
  std::filesystem::create_directories(std::filesystem::path(wal_path_).parent_path());
  std::filesystem::create_directories(std::filesystem::path(checkpoint_path_).parent_path());
  wal_file_.open(wal_path_, std::ios::app | std::ios::binary);
  ut_a(wal_file_.is_open());
}

ChunkPersister::~ChunkPersister() {
  if (wal_file_.is_open()) {
    wal_file_.close();
  }
}

void ChunkPersister::save_chunk(const Chunk *chunk, const std::string &path) {
  std::ofstream out_file(path, std::ios::binary);
  ut_a(out_file.is_open());

  auto header = const_cast<Chunk *>(chunk)->header();
  out_file.write(reinterpret_cast<const char *>(&header->m_type), sizeof(header->m_type));
  out_file.write(header->m_db.c_str(), header->m_db.length() + 1);
  out_file.write(header->m_table_name.c_str(), header->m_table_name.length() + 1);
  out_file.write(reinterpret_cast<const char *>(&header->m_pack_length), sizeof(header->m_pack_length));
  out_file.write(reinterpret_cast<const char *>(&header->m_normalized_pack_length),
                 sizeof(header->m_normalized_pack_length));
  out_file.write(reinterpret_cast<const char *>(&header->m_key_len), sizeof(header->m_key_len));
  out_file.write(reinterpret_cast<const char *>(&header->m_nullable), sizeof(header->m_nullable));
  out_file.write(reinterpret_cast<const char *>(&header->m_prows), sizeof(header->m_prows));

  // write the data
  size_t data_size = const_cast<Chunk *>(chunk)->current_id() * header->m_normalized_pack_length;
  out_file.write(reinterpret_cast<const char *>(chunk->base()), data_size);

  // write null mask（if exists）
  if (header->m_null_mask) {
    bool has_null_mask = true;
    out_file.write(reinterpret_cast<const char *>(&has_null_mask), sizeof(has_null_mask));
    out_file.write(reinterpret_cast<const char *>(header->m_null_mask->data), header->m_null_mask->size);
  } else {
    bool has_null_mask = false;
    out_file.write(reinterpret_cast<const char *>(&has_null_mask), sizeof(has_null_mask));
  }

  out_file.close();

  // if in transaction, then write wal.
  if (in_transaction_) {
    auto foot_print = const_cast<Chunk *>(chunk)->foot_print();
    WALEntry entry{transaction_id_, "SAVE", foot_print, 0, std::vector<uchar>()};
    write_wal(entry);
  }
}

void ChunkPersister::load_chunk(Chunk *chunk, const std::string &path) {
  std::ifstream in_file(path, std::ios::binary);
  ut_a(in_file.is_open());

  // read meta data.
  auto header = chunk->header();
  in_file.read(reinterpret_cast<char *>(&header->m_type), sizeof(header->m_type));
  std::getline(in_file, header->m_db, '\0');
  std::getline(in_file, header->m_table_name, '\0');
  in_file.read(reinterpret_cast<char *>(&header->m_pack_length), sizeof(header->m_pack_length));
  in_file.read(reinterpret_cast<char *>(&header->m_normalized_pack_length), sizeof(header->m_normalized_pack_length));
  in_file.read(reinterpret_cast<char *>(&header->m_key_len), sizeof(header->m_key_len));
  in_file.read(reinterpret_cast<char *>(&header->m_nullable), sizeof(header->m_nullable));
  in_file.read(reinterpret_cast<char *>(&header->m_prows), sizeof(header->m_prows));

  // read the data.
  size_t data_size = header->m_prows * header->m_normalized_pack_length;
  in_file.read(reinterpret_cast<char *>(chunk->base()), data_size);
  // chunk->seek(chunk->base() + data_size);

  // read null mask
  bool has_null_mask;
  in_file.read(reinterpret_cast<char *>(&has_null_mask), sizeof(has_null_mask));
  if (has_null_mask) {
    // chunk->ensure_null_mask_allocated();
    // in_file.read(reinterpret_cast<char *>(header->m_null_mask->data), header->m_null_mask->size);
  }

  in_file.close();
}

void ChunkPersister::begin_transaction() {
  transaction_id_.fetch_add(1);
  in_transaction_ = true;
  WALEntry entry{transaction_id_, "BEGIN", "", 0, std::vector<uchar>()};
  write_wal(entry);
}

void ChunkPersister::commit_transaction() {
  ut_a(in_transaction_);
  WALEntry entry{transaction_id_, "COMMIT", "", 0, std::vector<uchar>()};
  write_wal(entry);
  in_transaction_ = false;
}

void ChunkPersister::rollback_transaction() {
  ut_a(in_transaction_);
  WALEntry entry{transaction_id_, "ROLLBACK", "", 0, std::vector<uchar>()};
  write_wal(entry);
  in_transaction_ = false;
}

void ChunkPersister::recover() {
  auto wal_entries = read_wal();
  uint64_t last_checkpoint_txn = 0;

  // if there's checkpoint, then read from checkpoint.
  if (std::filesystem::exists(checkpoint_path_)) {
    load_from_checkpoint();
    std::ifstream checkpoint_file(checkpoint_path_, std::ios::binary);
    checkpoint_file.read(reinterpret_cast<char *>(&last_checkpoint_txn), sizeof(last_checkpoint_txn));
    checkpoint_file.close();
  }

  // replace WAL.
  for (const auto &entry : wal_entries) {
    if (entry.transaction_id <= last_checkpoint_txn) continue;
    // replace the oper. and rebuilt the Chunk.
  }
}

void ChunkPersister::create_checkpoint() {
  std::ofstream checkpoint_file(checkpoint_path_, std::ios::binary);
  ut_a(checkpoint_file.is_open());
  checkpoint_file.write(reinterpret_cast<const char *>(&transaction_id_), sizeof(transaction_id_));
  checkpoint_file.close();
}

void ChunkPersister::write_wal(const WALEntry &entry) {
  wal_file_.write(reinterpret_cast<const char *>(&entry.transaction_id), sizeof(entry.transaction_id));
  size_t op_size = entry.operation.size();
  wal_file_.write(reinterpret_cast<const char *>(&op_size), sizeof(op_size));
  wal_file_.write(entry.operation.c_str(), op_size);
  size_t key_size = entry.chunk_key.size();
  wal_file_.write(reinterpret_cast<const char *>(&key_size), sizeof(key_size));
  wal_file_.write(entry.chunk_key.c_str(), key_size);
  wal_file_.write(reinterpret_cast<const char *>(&entry.row_id), sizeof(entry.row_id));
  size_t data_size = entry.data.size();
  wal_file_.write(reinterpret_cast<const char *>(&data_size), sizeof(data_size));
  if (data_size > 0) {
    wal_file_.write(reinterpret_cast<const char *>(entry.data.data()), data_size);
  }
  wal_file_.flush();
}

std::vector<ChunkPersister::WALEntry> ChunkPersister::read_wal() {
  std::vector<ChunkPersister::WALEntry> entries;
  std::ifstream wal_in_file(wal_path_, std::ios::binary);
  if (!wal_in_file.is_open()) return entries;

  while (wal_in_file) {
    WALEntry entry;
    wal_in_file.read(reinterpret_cast<char *>(&entry.transaction_id), sizeof(entry.transaction_id));
    if (wal_in_file.eof()) break;

    size_t op_size;
    wal_in_file.read(reinterpret_cast<char *>(&op_size), sizeof(op_size));
    entry.operation.resize(op_size);
    wal_in_file.read(&entry.operation[0], op_size);

    size_t key_size;
    wal_in_file.read(reinterpret_cast<char *>(&key_size), sizeof(key_size));
    entry.chunk_key.resize(key_size);
    wal_in_file.read(&entry.chunk_key[0], key_size);

    wal_in_file.read(reinterpret_cast<char *>(&entry.row_id), sizeof(entry.row_id));

    size_t data_size;
    wal_in_file.read(reinterpret_cast<char *>(&data_size), sizeof(data_size));
    if (data_size > 0) {
      entry.data.resize(data_size);
      wal_in_file.read(reinterpret_cast<char *>(entry.data.data()), data_size);
    }
    entries.push_back(entry);
  }
  wal_in_file.close();
  return entries;
}

void ChunkPersister::load_from_checkpoint() {}
}  // namespace Imcs
}  // namespace ShannonBase
