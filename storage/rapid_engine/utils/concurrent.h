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

class latch {
 public:
  enum class result_t {
    SUCCESS = 0,
    INVALID_ARGUMENT,  // update <= 0
    EXCEEDS_COUNT,     // update > m_count
    ALREADY_ZERO       // m_count alread 0
  };

  explicit latch(std::ptrdiff_t count) : m_count(count) { assert(count >= 0); }

  latch(const latch &) = delete;
  latch &operator=(const latch &) = delete;

  result_t count_down(std::ptrdiff_t update = 1) {
    if (update <= 0) return result_t::INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_count == 0) return result_t::ALREADY_ZERO;

    if (update > m_count) return result_t::EXCEEDS_COUNT;

    m_count -= update;
    if (m_count == 0) m_cv.notify_all();
    return result_t::SUCCESS;
  }

  result_t arrive_and_wait(std::ptrdiff_t update = 1) {
    if (update <= 0) return result_t::INVALID_ARGUMENT;

    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_count == 0) return result_t::ALREADY_ZERO;
    if (update > m_count) return result_t::EXCEEDS_COUNT;

    m_count -= update;
    (m_count == 0) ? m_cv.notify_all() : m_cv.wait(lock, [this] { return m_count == 0; });
    return result_t::SUCCESS;
  }

  void wait() const {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this] { return m_count == 0; });
  }

  template <typename Rep, typename Period>
  bool wait_for(const std::chrono::duration<Rep, Period> &timeout) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_cv.wait_for(lock, timeout, [this] { return m_count == 0; });
  }

  template <typename Clock, typename Duration>
  bool wait_until(const std::chrono::time_point<Clock, Duration> &time) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_cv.wait_until(lock, time, [this] { return m_count == 0; });
  }

  bool try_wait() const noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_count == 0;
  }

  std::ptrdiff_t current_count() const noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_count;
  }

  static constexpr std::ptrdiff_t max() noexcept { return std::numeric_limits<std::ptrdiff_t>::max(); }

  static const char *result_to_string(result_t r) noexcept {
    switch (r) {
      case result_t::SUCCESS:
        return "success";
      case result_t::INVALID_ARGUMENT:
        return "invalid_argument";
      case result_t::EXCEEDS_COUNT:
        return "exceeds_count";
      case result_t::ALREADY_ZERO:
        return "already_zero";
      default:
        return "unknown";
    }
  }

 private:
  mutable std::mutex m_mutex;
  mutable std::condition_variable m_cv;
  std::ptrdiff_t m_count;
};
}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_CONCURRENT_UTIL_H__