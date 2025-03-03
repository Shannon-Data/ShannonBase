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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. for readview.
*/
#ifndef __SHANNONBASE_READVIEW_H__
#define __SHANNONBASE_READVIEW_H__

#include <chrono>
#include <tuple>
#include <unordered_map>

#include "storage/rapid_engine/trx/transaction.h"

namespace ShannonBase {
class Rapid_load_context;
namespace ReadView {

// in chunk, the latest veresion data always is in. the old version of data moves to
// SMU. So if a trx can see the latest version data, it should travers the version
// link to check whether there's some visible data or not. if yes, return the old ver
// data or otherwise, go to check the next item.
struct SHANNON_ALIGNAS smu_item_t {
  // trxid of old version value.
  Transaction::ID trxid;

  // timestamp of the modification.
  std::chrono::time_point<std::chrono::high_resolution_clock> tm_stamp;

  // the old version of data. all var data types were encoded.
  std::unique_ptr<uchar[]> data;

  smu_item_t(size_t size) : data(new uchar[size]) { tm_stamp = std::chrono::high_resolution_clock::now(); }
  smu_item_t() = delete;
  // Disable copying
  smu_item_t(const smu_item_t &) = delete;
  smu_item_t &operator=(const smu_item_t &) = delete;

  // Define a move constructor
  smu_item_t(smu_item_t &&other) noexcept : trxid(other.trxid), tm_stamp(other.tm_stamp), data(std::move(other.data)) {}

  // Define a move assignment operator
  smu_item_t &operator=(smu_item_t &&other) noexcept {
    if (this != &other) {
      trxid = other.trxid;
      tm_stamp = other.tm_stamp;
      data = std::move(other.data);
    }
    return *this;
  }
};

struct smu_item_vec_t {
  std::mutex vec_mutex;
  std::vector<smu_item_t> items;
  smu_item_vec_t() = default;

  smu_item_vec_t(const smu_item_vec_t &) = delete;
  smu_item_vec_t &operator=(const smu_item_vec_t &) = delete;

  smu_item_vec_t(smu_item_vec_t &&other) noexcept {
    std::lock_guard<std::mutex> lock(other.vec_mutex);
    items = std::move(other.items);
  }

  smu_item_vec_t &operator=(smu_item_vec_t &&other) noexcept {
    if (this != &other) {
      std::lock_guard<std::mutex> lock(other.vec_mutex);
      items = std::move(other.items);
    }
    return *this;
  }

  void add(smu_item_t &&item) {
    std::lock_guard<std::mutex> lock(vec_mutex);
    items.emplace_back(std::move(item));
  }
  // gets the first met visibility data in this version link.
  uchar *get_data(Rapid_load_context *context);
};

}  // namespace ReadView
}  // namespace ShannonBase
#endif  //__SHANNONBASE_READVIEW_H__