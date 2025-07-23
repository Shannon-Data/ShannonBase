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
#ifndef __SHANNONBASE_CONCURRENT_UTIL_H__
#define __SHANNONBASE_CONCURRENT_UTIL_H__

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <optional>

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace ShannonBase {
namespace Utils {
using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::use_awaitable;

// co-routine utils class.
template <typename T>
class shared_promise {
 public:
  void set_value(const T &v) {
    std::lock_guard<std::mutex> lck(m_mutex);
    if (!m_ready) {
      m_value = v;
      m_ready = true;
      m_cond.notify_all();
    }
  }

  boost::asio::awaitable<T> get_awaitable(boost::asio::any_io_executor executor) {
    while (true) {
      {
        std::lock_guard<std::mutex> lck(m_mutex);
        if (m_ready) co_return *m_value;
      }

      co_await boost::asio::post(executor, boost::asio::use_awaitable);
    }
  }

 private:
  std::mutex m_mutex;
  std::condition_variable m_cond;
  std::optional<T> m_value;
  bool m_ready = false;
};

template <typename T>
T cowait_sync_with(boost::asio::thread_pool &pool, boost::asio::awaitable<T> aw) {
  auto prom = std::make_shared<std::promise<T>>();
  auto fut = prom->get_future();

  boost::asio::co_spawn(
      pool,
      [aw = std::move(aw), prom]() mutable -> boost::asio::awaitable<void> {
        try {
          T result = co_await std::move(aw);
          prom->set_value(std::move(result));
        } catch (...) {
          prom->set_exception(std::current_exception());
        }
        co_return;
      },
      boost::asio::detached);

  return fut.get();  // when the result is ready.
}

}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_CONCURRENT_UTIL_H__