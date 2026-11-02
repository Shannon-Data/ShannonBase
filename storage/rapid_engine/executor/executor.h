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
#ifndef __SHANNONBASE_EXECUTOR_H__
#define __SHANNONBASE_EXECUTOR_H__

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <vector>

#include "storage/rapid_engine/resource_management/res_mgmt.h"

class THD;
namespace ShannonBase {
namespace ResMgmt {
class BoundedMemoryResource;
}
namespace Executor {
class ColumnChunk;

namespace Morsel {
// Upper bound on the workers one pipeline may use, taken from this process's
// usable CPUs (its affinity mask, not the machine's core count). Asked per
// pipeline rather than cached: the answer is a property of the environment the
// process happens to run in. The governor's admission cap and the session knob
// narrow it further; none of them is the source of truth on its own.
size_t MaxParallelWorkers();

struct Task {
  size_t partition{0};
  size_t begin{0};
  size_t count{0};
};

// Built by the coordinator, then immutable except for the claim cursor.
// A parallel is a physical interval, including rows rejected by visibility/filter.
//
// Rows are NOT pre-split into parallels. The queue stores one row range per
// partition and hands out fixed-size slices from a single atomic cursor, so a
// worker that finishes early takes the next slice instead of waiting for the
// slowest pre-split chunk. Static splitting made the tail of an uneven
// partition (visibility, predicates, dictionary misses) run on one thread while
// the rest idled; that whole tail is gone once the slice size, not the
// partition size, bounds the imbalance.
class ParallelQueue {
 public:
  // Claim granularity: small enough that one slow claim cannot strand the rest
  // of the workers, large enough that the claim itself stays noise next to a
  // slice's materialization work.
  static constexpr size_t kClaimRows = 4096;

  explicit ParallelQueue(std::pmr::memory_resource *memory) : m_ranges(memory) {}

  // Records [0, rows) of one partition. Partitions must be added once each.
  // Every partition starts on a kClaimRows boundary of the global cursor. The
  // cursor advances by a fixed kClaimRows per claim, so a partition whose size
  // is not a multiple of it would otherwise leave the next claim landing in the
  // middle of its neighbour and skip that neighbour's leading rows. The padding
  // is never handed out: Claim() skips claims that begin past a partition's end.
  void Add(size_t partition, size_t rows) {
    if (rows == 0) return;
    m_ranges.push_back({partition, m_total_rows, rows});
    m_total_rows += (rows + kClaimRows - 1) / kClaimRows * kClaimRows;
  }

  bool Claim(Task *task) {
    for (;;) {
      const size_t base = m_next.fetch_add(kClaimRows, std::memory_order_relaxed);
      if (base >= m_total_rows) return false;
      // A claim never straddles a partition: a slice that would cross the
      // boundary is cut at it, and the next claim continues in the neighbour.
      const size_t range = FindRange(base);
      if (range >= m_ranges.size()) return false;

      const auto &entry = m_ranges[range];
      const size_t begin = base - entry.start;
      if (begin >= entry.rows) continue;

      *task = {entry.partition, begin, std::min(kClaimRows, entry.rows - begin)};
      return true;
    }
  }

  // Upper bound on how many claims the queue can hand out, which is what a
  // caller needs to decide whether parallel execution is worth starting.
  size_t size() const { return (m_total_rows + kClaimRows - 1) / kClaimRows; }

 private:
  struct Range {
    size_t partition;
    size_t start;  // Global row offset of this partition.
    size_t rows;
  };

  size_t FindRange(size_t row) const {
    size_t low = 0, high = m_ranges.size();
    while (low + 1 < high) {
      const size_t mid = (low + high) / 2;
      if (m_ranges[mid].start <= row)
        low = mid;
      else
        high = mid;
    }
    return m_ranges.empty() ? m_ranges.size() : low;
  }

  std::pmr::vector<Range> m_ranges;
  size_t m_total_rows{0};
  std::atomic<size_t> m_next{0};
};

// Immutable SQL metadata is compiled by the foreground thread. Execute owns
// all mutable chunks/scratch through the supplied worker-local memory resource.
// Callbacks are synchronous; no column, selection or ordinal escapes a call.
// A source borrows its cursor's pins, predicates and transaction ReadView. Workers
// may only READ that snapshot. Shared SQL buffers cannot change while workers
// run. The coordinator must not advance a transaction/cursor or release pins
// until every worker has joined and the source is destroyed.
// Execute(false) means success or cancellation;
// Execute(true) means error, or speculative failure when needs_serial_retry().
// Ordinals encode serial input order, including build-row fanout at joins.
class ParallelSource {
 public:
  // Transport capacity shared by scan/filter/join sources and their budget estimates.
  static constexpr size_t kBatchRows = 1024;
  using Consumer = std::function<bool(const std::pmr::vector<ColumnChunk> &, size_t, const std::pmr::vector<uint32_t> &,
                                      const std::pmr::vector<uint64_t> &)>;
  virtual ~ParallelSource() = default;
  virtual size_t tasks() const = 0;
  virtual uint64_t max_ordinal() const = 0;
  virtual size_t extra_scratch_bytes() const { return 0; }
  virtual bool needs_serial_retry() const { return m_serial_retry.load(std::memory_order_relaxed); }
  // Called once by the coordinator after all workers join and the consumer
  // accepts the complete result. Speculative retries discard pending stats.
  virtual void CommitStats() {}
  bool Execute(THD *worker, ResMgmt::BoundedMemoryResource *memory, const std::atomic<bool> &cancelled,
               const Consumer &consume);

 protected:
  virtual bool ExecuteImpl(THD *worker, ResMgmt::BoundedMemoryResource *memory, const std::atomic<bool> &cancelled,
                           const Consumer &consume) = 0;
  bool m_test_budget_refusal{false};

 private:
  std::atomic<bool> m_serial_retry{false};
};

/**
 * A source whose rows are an immutable, memory-resident block can be read more
 * than once, by any thread, in any order. Operators that rescan their input
 * once per outer row (nested loop join) need that: ParallelSource::Execute() is
 * one-shot by contract, so a plain source cannot be re-entered.
 *
 * Replay() appends block rows [from, from + count) into caller-owned chunks
 * that the caller bound to the same fields the source was created with.
 * Implementations are read-only and thread-safe.
 */
class ReplayableParallelSource {
 public:
  virtual ~ReplayableParallelSource() = default;
  virtual size_t replay_row_count() const = 0;
  virtual bool Replay(size_t from, size_t count, std::pmr::vector<ColumnChunk> &chunks) = 0;
};

// A worker costs a thread, an admission slot and its own scratch, and that cost
// does not shrink with the input: the same fixed work is paid on a 100k-row
// build as on a 10M-row one. Entering the parallel phase is only worth it when
// every worker gets enough input to amortise it.
inline constexpr size_t kMinWindowsPerWorker = 16;

// True when `workers` threads have enough rows to be worth starting. Both
// callers pass a source-derived row bound, so this is a cost check, not a
// heuristic on data size: it says "the fixed coordination cost is amortised".
inline bool EnoughWorkForWorkers(size_t workers, double rows) {
  if (workers < 2 || rows <= 0) return false;
  const double windows = rows / static_cast<double>(ParallelSource::kBatchRows);
  return windows >= static_cast<double>(kMinWindowsPerWorker * workers);
}

unsigned ParallelWorkers(THD *thd);
void RegisterParallelThreads();
// Uses the same os_thread_create / IB_thread / internal THD lifecycle as
// change propagation. Joins every started worker before returning, on all exits.
// Only the coordinator raises the statement error.
enum class ParallelRunResult { SUCCESS, RETRY, ERROR };
ParallelRunResult RunParallelWorkers(THD *coordinator, size_t workers,
                                     const std::function<const char *(size_t, THD *, const std::atomic<bool> &)> &work);
// Reuse admission across sequential phases. The caller retains the lease until
// all phases finish; this overload never reacquires slots or returns RETRY.
ParallelRunResult RunParallelWorkers(THD *coordinator, const ResMgmt::WorkGovernor::ParallelLease &admission,
                                     const std::function<const char *(size_t, THD *, const std::atomic<bool> &)> &work);
}  // namespace Morsel
}  // namespace Executor
}  // namespace ShannonBase
#endif  //__SHANNONBASE_EXECUTOR_H__
