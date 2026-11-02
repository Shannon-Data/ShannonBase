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

   The fundmental code for imcs optimizer.
*/
#include "storage/rapid_engine/executor/executor.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

#include "mysql/psi/mysql_thread.h"
#include "sql/debug_sync.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/sql_thd_internal_api.h"
#include "storage/innobase/include/os0thread-create.h"
#include "storage/rapid_engine/monitor/rapid_monitor.h"
#include "storage/rapid_engine/resource_management/res_mgmt.h"
#include "storage/rapid_engine/trx/transaction.h"

namespace ShannonBase {
namespace Executor {
namespace Morsel {
namespace {
// Rapid owns this instrument. A default-constructed mysql_pfs_key_t would
// incorrectly add it to InnoDB's startup key-registration count, before Rapid
// initializes. Wrap the registered PSI key only when creating a native thread.
PSI_thread_key parallel_thread_key;
std::once_flag thread_registration;
}  // namespace

bool ParallelSource::Execute(THD *worker, ResMgmt::BoundedMemoryResource *memory, const std::atomic<bool> &cancelled,
                             const Consumer &consume) {
  // Cancellation is not this source's failure: whoever set `cancelled` already
  // recorded the error that RunParallelWorkers reports, so report success here.
  if (cancelled.load(std::memory_order_acquire)) return false;
    // Implementations report an allocation refusal through their return value and
    // last_failure(); nothing unwinds out of an operator body.
#ifndef NDEBUG
  // Test-only hook; keep it off the production path. A refusal is reported the
  // same way a real one is: this source fails, records that the caller must
  // replay serially, and the caller retries on a single worker.
  if (m_test_budget_refusal) {
    m_serial_retry.store(true, std::memory_order_relaxed);
    return true;
  }
#endif
  // Operator bodies mostly use non-throwing allocation, but reserve()/push_back()
  // on a pmr vector and Reservation's constructor throw when the worker's budget
  // is refused. Left to unwind, that skipped the retry check below and turned a
  // budget refusal into a hard query error. Anything that is not a budget
  // refusal keeps propagating to RunParallelWorkers.
  bool error;
  try {
    error = ExecuteImpl(worker, memory, cancelled, consume);
  } catch (const std::bad_alloc &) {
    if (!memory->budget_refused()) throw;
    error = true;
  }
  if (cancelled.load(std::memory_order_acquire)) return false;
  // The flag is sticky: last_failure() would be erased by any allocation that
  // succeeded between the refusal and this return, losing the serial retry.
  if (error && memory->budget_refused()) m_serial_retry.store(true, std::memory_order_relaxed);
  return error;
}

void RegisterParallelThreads() {
#ifdef UNIV_PFS_THREAD
  std::call_once(thread_registration, [] {
    // PSI_FLAG_AUTO_SEQNUM formats the OS name as "<os_name>-%d", so the OS name
    // has to leave room for the suffix: it must be shorter than
    // PFS_MAX_OS_NAME_LENGTH - 3 (13). "rapid_worker" is 12 characters, which
    // fits; a longer name aborts the server in register_thread_class().
    PSI_thread_info info[] = {{&parallel_thread_key, "parallel_worker", "rapid_worker",
                               PSI_FLAG_THREAD_SYSTEM | PSI_FLAG_AUTO_SEQNUM, 0, PSI_DOCUMENT_ME}};
    mysql_thread_register("rapid", info, 1);
  });
#endif
}

size_t MaxParallelWorkers() { return ResMgmt::HardwareWorkers(); }

ParallelRunResult RunParallelWorkers(
    THD *coordinator, size_t workers,
    const std::function<const char *(size_t, THD *, const std::atomic<bool> &)> &work) {
  if (!coordinator || !workers) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid parallel executor requires a coordinator and 1+ workers");
    return ParallelRunResult::ERROR;
  }
  // Capacity belongs to the running process, so an oversized request is
  // clamped: refusing would turn a busy machine into a failed query.
  workers = std::min(workers, MaxParallelWorkers());
  auto admission = ResMgmt::work_governor().try_acquire_parallel_workers(workers);
  if (!admission) return ParallelRunResult::RETRY;
  return RunParallelWorkers(coordinator, admission, work);
}

ParallelRunResult RunParallelWorkers(
    THD *coordinator, const ResMgmt::WorkGovernor::ParallelLease &admission,
    const std::function<const char *(size_t, THD *, const std::atomic<bool> &)> &work) {
  const size_t reserved = admission.size();
  if (!coordinator || !reserved) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Rapid parallel executor requires a coordinator and 1+ reserved workers");
    return ParallelRunResult::ERROR;
  }
  const size_t workers = std::min(reserved, MaxParallelWorkers());
  RegisterParallelThreads();
  // Contract for `work`: it must not block on MDL or other locks that the
  // coordinator's statement can hold, because the coordinator waits on a
  // condition variable that the MDL deadlock detector cannot see.
  std::atomic<bool> cancelled{false};
  std::mutex mutex;
  // One condition variable per direction. Sharing one let a worker's
  // completion notify_one() wake a worker parked at the launch gate instead of
  // the coordinator, so completion only worked through the polling timeout.
  std::condition_variable launch_cv;  // coordinator -> workers
  std::condition_variable done_cv;    // workers -> coordinator
  size_t finished = 0;
  bool launched = false;
  // 0: a message produced by this harness. Otherwise the errno of the worker's
  // own diagnostics area, so deadlock/timeout/OOM codes survive.
  uint error_code = 0;
  std::array<char, 512> error{};
  const auto fail = [&](const char *message, uint code) {
    cancelled.store(true, std::memory_order_release);
    std::lock_guard lock(mutex);
    if (error[0]) return;
    std::snprintf(error.data(), error.size(), "%s", (message && *message) ? message : "Rapid parallel worker failed");
    error_code = code;
  };
  // No exception path: ask for the slot array without throwing, so a refusal is
  // one more value this function can report.
  std::unique_ptr<IB_thread[]> threads(new (std::nothrow) IB_thread[workers]);
  if (!threads) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid could not reserve its parallel worker slots");
    return ParallelRunResult::ERROR;
  }
  // Workers capture this frame by reference, so every started worker must be
  // joined before any return or unwind. `started` counts only threads that
  // really launched; the wait and the join below use it, never `workers`.
  size_t started = 0;
  for (size_t index = 0; index < workers; ++index) {
    try {
      // The thread body is the last boundary: an exception must not escape it.
      threads[index] = os_thread_create(mysql_pfs_key_t(parallel_thread_key), 0, [&, index] {
        ResMgmt::WorkerRegistration registration;
        THD *worker = nullptr;
        try {
          worker = create_internal_thd();
          if (!worker) {
            fail("Rapid parallel worker could not create its internal THD", 0);
          } else {
            worker->system_thread = SYSTEM_THREAD_BACKGROUND;
            worker->store_globals();
            thd_proc_info(worker, "Rapid parallel pipeline: waiting for launch");
            {
              std::unique_lock lock(mutex);
              launch_cv.wait(lock, [&] { return launched; });
            }
            if (!cancelled.load(std::memory_order_acquire)) {
              thd_proc_info(worker, "Rapid parallel pipeline: executing");
              const char *message = work(index, worker, cancelled);
              // A diagnostics-area error carries the real errno; prefer it over
              // the generic message.
              if (worker->is_error())
                fail(worker->get_stmt_da()->message_text(), worker->get_stmt_da()->mysql_errno());
              else if (message)
                fail(message, 0);
            }
          }
        } catch (const std::bad_alloc &) {
          fail("Rapid parallel worker exhausted its memory share", 0);
        } catch (...) {
          fail("Rapid parallel worker failed", 0);
        }
        // Teardown can throw too; it must not escape the thread body, and the
        // completion count below must always be reached.
        try {
          if (worker) {
            Transaction::free_trx_from_thd(worker);
            close_thread_tables(worker);
            worker->mdl_context.release_transactional_locks();
            destroy_internal_thd(worker);
          }
        } catch (...) {
          fail("Rapid parallel worker failed during teardown", 0);
        }
        {
          std::lock_guard lock(mutex);
          ++finished;
        }
        done_cv.notify_one();
      });
      threads[index].start();
    } catch (...) {
      // Could not launch worker `index`: stop launching, let the ones already
      // running observe `cancelled`, and report after they are joined.
      fail("Rapid could not start a parallel worker thread", 0);
      break;
    }
    ++started;
    RapidMonitor::rapid_counters.query_parallel_workers_total.fetch_add(1, std::memory_order_relaxed);
  }
  DEBUG_SYNC(coordinator, "rapid_parallel_workers_started");
  {
    std::lock_guard lock(mutex);
    if (coordinator->killed) cancelled.store(true, std::memory_order_release);
    launched = true;
  }
  launch_cv.notify_all();
  {
    std::unique_lock lock(mutex);
    while (finished < started) {
      if (coordinator->killed) cancelled.store(true, std::memory_order_release);
      // Completion notifies immediately; the timeout also observes KILL QUERY,
      // which does not notify this condition variable.
      done_cv.wait_for(lock, std::chrono::milliseconds(10), [&] { return finished == started; });
    }
  }
  for (size_t index = 0; index < started; ++index) threads[index].join();
  if (coordinator->killed) {
    coordinator->send_kill_message();
    return ParallelRunResult::ERROR;
  }
  if (error[0]) {
    if (error_code == 0)
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), error.data());
    else
      my_message(error_code, error.data(), MYF(0));  // keep the worker's original errno
    return ParallelRunResult::ERROR;
  }
  return ParallelRunResult::SUCCESS;
}
}  // namespace Morsel
}  // namespace Executor
}  // namespace ShannonBase
