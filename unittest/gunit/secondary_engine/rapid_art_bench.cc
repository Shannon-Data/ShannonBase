/* Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   Standalone throughput / latency harness for the Rapid ART.

   It uses only the public ART API, so the same source builds against the
   baseline and a candidate implementation:

     g++ -std=gnu++20 -O2 -g -pthread -D_GNU_SOURCE \
         -I$ROOT/cmake_build -I$ROOT/cmake_build/include -I$ROOT -I$ROOT/include
   \
         $ROOT/storage/rapid_engine/imcs/index/art/art.cpp this_file.cc \
         -o /tmp/art_bench

   Usage: art_bench [rows] [readers] [seconds] [writers]
   BENCH_SHARED=1 makes readers overlap the writer key range.
   BENCH_PIN_WORKERS=1 pins each worker to a distinct CPU (main/load uses CPU 0).
   BENCH_SCANS=1 measures 32-key scans with per-row locking; BENCH_REVERSE=1
   reverses them. BENCH_DUPLICATES=N loads N row ids per key and churns an extra
   row id instead of removing whole keys, exercising non-unique leaf updates.
*/

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "storage/rapid_engine/imcs/index/art/art.h"
#include "storage/rapid_engine/imcs/index/iterator.h"

using ShannonBase::Imcs::Index::ART;
using Clock = std::chrono::steady_clock;
using RowId = uint64_t;

namespace {

void PinWorker(int cpu) {
  if (!std::getenv("BENCH_PIN_WORKERS")) return;
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpu, &mask);
  if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) != 0) std::abort();
}

uint64_t Mix(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

std::string KeyOf(uint64_t v) {
  std::string k(8, '\0');
  for (int i = 7; i >= 0; --i) {
    k[i] = static_cast<char>(v & 0xff);
    v >>= 8;
  }
  return k;
}

double Seconds(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

/**
 * Latency samples are capped per worker. Every operation up to the cap is
 * recorded; beyond it the count still advances. Merge retains every worker's
 * recorded prefix so aggregate percentiles do not depend on worker order.
 */
struct alignas(64) Collector {
  static constexpr size_t kCap = 4u << 20;
  std::vector<uint32_t> ns;
  uint64_t ops{0};
  uint64_t hits{0};
  bool sorted{false};

  void Add(uint32_t v, bool found = true) {
    ++ops;
    if (found) ++hits;
    sorted = false;
    if (ns.size() < kCap) ns.push_back(v);
  }

  void Merge(const Collector &other) {
    ops += other.ops;
    hits += other.hits;
    ns.insert(ns.end(), other.ns.begin(), other.ns.end());
    sorted = false;
  }

  double Pct(double p) {
    if (ns.empty()) return 0;
    // A full collector is still unsorted: reaching the sample cap must not
    // turn the requested percentile into an arbitrary insertion-order sample.
    if (!sorted) {
      std::sort(ns.begin(), ns.end());
      sorted = true;
    }
    const size_t idx = static_cast<size_t>(p * (ns.size() - 1));
    return ns[idx] / 1000.0;  // microseconds
  }
};

template <typename Fn>
uint32_t Timed(Fn &&fn) {
  const auto a = Clock::now();
  fn();
  const auto b = Clock::now();
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

size_t RssKb() {
  std::ifstream status("/proc/self/status");
  std::string name;
  while (status >> name) {
    if (name == "VmRSS:") {
      size_t kb = 0;
      status >> kb;
      return kb;
    }
    status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  PinWorker(0);
  const size_t rows = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2000000;
  const int readers = argc > 2 ? std::atoi(argv[2]) : 4;
  const double seconds = argc > 3 ? std::atof(argv[3]) : 2.0;
  const int writers = argc > 4 ? std::max(1, std::atoi(argv[4])) : 1;

  const char *duplicates_env = std::getenv("BENCH_DUPLICATES");
  const uint32_t duplicates = duplicates_env ? std::max(1, std::atoi(duplicates_env)) : 1;
  if (rows < 2 || readers < 1 || seconds <= 0 || writers > static_cast<int>(rows / 2)) return 2;
  std::atomic<size_t> errors{0};

  const bool scans = std::getenv("BENCH_SCANS") != nullptr;
  const bool reverse = std::getenv("BENCH_REVERSE") != nullptr;
  const bool recreate_cursor = std::getenv("BENCH_RECREATE_CURSOR") != nullptr;
  const char *scan_keys_env = std::getenv("BENCH_SCAN_KEYS");
  const size_t scan_keys = scan_keys_env ? std::max(1, std::atoi(scan_keys_env)) : 32;
  if (scans && rows < scan_keys * 4) return 2;
  std::vector<std::string> keys(rows);
  for (size_t i = 0; i < rows; ++i) keys[i] = KeyOf(scans ? i : Mix(i) | 1);

  ART tree;
  tree.ART_tree_init();

  // ---- load
  // -------------------------------------------------------------------------------
  {
    const auto t0 = Clock::now();
    for (size_t i = 0; i < rows; ++i) {
      for (uint32_t duplicate = 0; duplicate < duplicates; ++duplicate) {
        RowId v = i * duplicates + duplicate + 1;
        tree.ART_insert(reinterpret_cast<const unsigned char *>(keys[i].data()), static_cast<int>(keys[i].size()), &v,
                        sizeof(v));
      }
    }
    const double secs = Seconds(t0, Clock::now());
    std::printf(
        "LOAD      rows=%zu duplicates=%u  %.0f rows/s %.0f values/s "
        "(%.2fs) rss_kb=%zu\n",
        rows, duplicates, rows / secs, rows * duplicates / secs, secs, RssKb());
  }

  const size_t stable_begin = rows / 2;
  auto scan = [&](ShannonBase::Imcs::Index::Art_Iterator *cursor, size_t first) {
    std::optional<ShannonBase::Imcs::Index::Art_Iterator> temporary;
    if (!cursor) {
      temporary.emplace(&tree);
      cursor = &*temporary;
    }
    const auto *lo = reinterpret_cast<const unsigned char *>(keys[first].data());
    const auto *hi = reinterpret_cast<const unsigned char *>(keys[first + scan_keys - 1].data());
    if (reverse)
      cursor->init_reverse_scan(lo, 8, true, hi, 8, true);
    else
      cursor->init_scan(lo, 8, true, hi, 8, true);
    const unsigned char *key;
    uint32_t len;
    RowId value;
    size_t count = 0;
    while (reverse ? cursor->prev(&key, &len, &value) : cursor->next(&key, &len, &value)) {
      const size_t offset = reverse ? scan_keys * duplicates - 1 - count : count;
      // Reverse iteration reverses the values inside each duplicate leaf too.
      if (count >= scan_keys * duplicates || value != first * duplicates + offset + 1) {
        ++errors;
        return false;
      }
      ++count;
    }
    if (count != scan_keys * duplicates) ++errors;
    return count == scan_keys * duplicates;
  };
  std::printf("MODE      scans=%d reverse=%d keys_per_scan=%zu\n", scans, reverse, scan_keys);

  // ---- read only
  // --------------------------------------------------------------------------
  {
    std::atomic<bool> stop{false}, start{false};
    std::atomic<int> ready{0};
    std::vector<Collector> per_thread(readers);
    std::vector<std::thread> threads;
    for (int t = 0; t < readers; ++t) {
      threads.emplace_back([&, t] {
        PinWorker(t);
        std::mt19937_64 rng(1000 + t);
        RowId out = 0;
        std::unique_ptr<ShannonBase::Imcs::Index::Art_Iterator> cursor;
        if (scans && !recreate_cursor) cursor = std::make_unique<ShannonBase::Imcs::Index::Art_Iterator>(&tree);
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        while (!stop.load(std::memory_order_relaxed)) {
          const size_t i = rng() % rows;
          bool found = false;
          const uint32_t ns = Timed([&] {
            if (scans) {
              found = scan(cursor.get(), stable_begin + rng() % (rows - stable_begin - scan_keys));
              return;
            }
            found = tree.ART_search_copy(reinterpret_cast<const unsigned char *>(keys[i].data()),
                                         static_cast<int>(keys[i].size()), &out, sizeof(out));
            if (!found || out != (i + 1) * duplicates) ++errors;
          });
          per_thread[t].Add(ns);
        }
      });
    }
    while (ready.load(std::memory_order_acquire) != readers) std::this_thread::yield();
    const auto started = Clock::now();
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop.store(true);
    for (auto &th : threads) th.join();

    const double elapsed = Seconds(started, Clock::now());
    Collector all;
    for (auto &c : per_thread) all.Merge(c);
    std::printf("READ      readers=%d  %.0f ops/s  p50=%.3fus p99=%.3fus p999=%.3fus\n", readers, all.ops / elapsed,
                all.Pct(0.50), all.Pct(0.99), all.Pct(0.999));
  }

  // ---- one writer churning a range; BENCH_SHARED=1 makes readers hit it too
  // -----------------
  {
    const bool shared_range = std::getenv("BENCH_SHARED") != nullptr;
    const size_t read_lo = shared_range ? 0 : stable_begin;
    const size_t read_span = shared_range ? stable_begin : (rows - stable_begin);
    std::atomic<bool> stop{false}, start{false};
    std::atomic<int> ready{0};
    std::vector<Collector> read_lat(readers);
    std::vector<Collector> write_lat(writers);
    std::vector<std::thread> threads;

    for (int t = 0; t < readers; ++t) {
      threads.emplace_back([&, t] {
        PinWorker(t);
        std::mt19937_64 rng(5000 + t);
        RowId out = 0;
        std::unique_ptr<ShannonBase::Imcs::Index::Art_Iterator> cursor;
        if (scans && !recreate_cursor) cursor = std::make_unique<ShannonBase::Imcs::Index::Art_Iterator>(&tree);
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        while (!stop.load(std::memory_order_relaxed)) {
          const size_t i = read_lo + (rng() % read_span);
          bool found = false;
          const uint32_t ns = Timed([&] {
            if (scans) {
              found = scan(cursor.get(), stable_begin + rng() % (rows - stable_begin - scan_keys));
              return;
            }
            found = tree.ART_search_copy(reinterpret_cast<const unsigned char *>(keys[i].data()),
                                         static_cast<int>(keys[i].size()), &out, sizeof(out));
            if (!shared_range && (!found || out != (i + 1) * duplicates)) ++errors;
            if (found && (out == 0 || out > rows * duplicates + writers)) ++errors;
          });
          read_lat[t].Add(ns, found);
        }
      });
    }

    // Writers churn disjoint sub-ranges so their updates never fight over the
    // same key; what is measured is the index, not duplicate-key semantics.
    std::vector<std::thread> writer_threads;
    for (int w = 0; w < writers; ++w) {
      writer_threads.emplace_back([&, w] {
        PinWorker(readers + w);
        std::mt19937_64 rng(4242 + w);
        const size_t lo = (stable_begin * static_cast<size_t>(w)) / static_cast<size_t>(writers);
        const size_t hi = (stable_begin * static_cast<size_t>(w + 1)) / static_cast<size_t>(writers);
        const size_t span = std::max<size_t>(1, hi - lo);
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        while (!stop.load(std::memory_order_relaxed)) {
          const size_t i = lo + (rng() % span);
          const auto *k = reinterpret_cast<const unsigned char *>(keys[i].data());
          RowId v = duplicates > 1 ? rows * duplicates + w + 1 : i + 1;
          const uint32_t ns = Timed([&] {
            if (rng() & 1) {
              tree.ART_insert(k, static_cast<int>(keys[i].size()), &v, sizeof(v));
            } else if (duplicates > 1) {
              tree.ART_delete_value(k, static_cast<int>(keys[i].size()), &v, sizeof(v));
            } else {
              tree.ART_delete(k, static_cast<int>(keys[i].size()));
            }
          });
          write_lat[w].Add(ns);
        }
      });
    }

    while (ready.load(std::memory_order_acquire) != readers + writers) std::this_thread::yield();
    const auto started = Clock::now();
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop.store(true);
    for (auto &th : threads) th.join();
    for (auto &th : writer_threads) th.join();

    const double elapsed = Seconds(started, Clock::now());
    Collector rl;
    for (auto &c : read_lat) rl.Merge(c);
    Collector wl;
    for (auto &c : write_lat) wl.Merge(c);
    std::printf(
        "MIXED     readers=%d writers=%d  read=%.0f ops/s p99=%.3fus "
        "p999=%.3fus max=%.3fus | write=%.0f ops/s "
        "p99=%.3fus p999=%.3fus "
        "hits=%llu misses=%llu\n",
        readers, writers, rl.ops / elapsed, rl.Pct(0.99), rl.Pct(0.999), rl.Pct(1.0), wl.ops / elapsed, wl.Pct(0.99),
        wl.Pct(0.999), static_cast<unsigned long long>(rl.hits), static_cast<unsigned long long>(rl.ops - rl.hits));
  }

  std::printf("CHECK     errors=%zu rss_final_kb=%zu\n", errors.load(), RssKb());
  tree.ART_tree_destroy();
  return errors.load() == 0 ? 0 : 1;
}
