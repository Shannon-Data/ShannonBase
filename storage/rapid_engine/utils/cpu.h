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
#ifndef __SHANNONBASE_CPU_UTIL_H__
#define __SHANNONBASE_CPU_UTIL_H__
#include <algorithm>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <pdh.h>
#include <windows.h>
#pragma comment(lib, "pdh.lib")
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#else
#include <fstream>
#include <sstream>
#endif

/*****************************************************************************
 *   TO gets a num by CPU usage. for example:
 *   uint32_t batch_size = adjuster.getMaxBatchSize();
 *   double cpu_usage = adjuster.getCurrentCpuUsage();
 *   printf("Batch size: %u, CPU usage: %.2f%%\n",
 *              batch_size, cpu_usage * 100);
 *****************************************************************************/
namespace ShannonBase {
namespace Utils {
class SimpleRatioAdjuster {
 private:
  const uint32_t m_hardware_threads;
  const uint32_t m_min_batch_sz;
  const uint32_t m_max_batch_sz;

  // EMA smooth param
  mutable double m_cpu_usage_ema;
  const double m_ema_alpha;
  mutable bool m_first_call;

#ifdef _WIN32
  PDH_HQUERY m_cpu_query;
  PDH_HCOUNTER m_cpu_counter;
#elif defined(__APPLE__)
  host_t host_port_;
#else
  struct CpuTimes {
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
    uint64_t getTotal() const { return user + nice + system + idle + iowait + irq + softirq + steal; }
    uint64_t getIdle() const { return idle; }
  };
  mutable CpuTimes m_prev_cpu_times;
  mutable bool cpu_times_initialized_;
#endif

 public:
  SimpleRatioAdjuster(double ema_alpha = 0.3)
      : m_hardware_threads(std::thread::hardware_concurrency()),
        m_min_batch_sz(1),
        m_max_batch_sz(m_hardware_threads),
        m_cpu_usage_ema(0.0),
        m_ema_alpha(ema_alpha),
        m_first_call(true)
#ifdef __linux__
        ,
        cpu_times_initialized_(false)
#endif
  {
    initializePlatformSpecific();
  }

  ~SimpleRatioAdjuster() { cleanupPlatformSpecific(); }

  uint32_t getMaxBatchSize() const {
    double current_cpu = getCurrentCpuUsage();

    // EMA
    if (m_first_call) {
      m_cpu_usage_ema = current_cpu;
      m_first_call = false;
    } else {
      m_cpu_usage_ema = m_ema_alpha * current_cpu + (1.0 - m_ema_alpha) * m_cpu_usage_ema;
    }

    // base on th cup usage, the more higher cpu usage, the small the number get.
    double cpu_factor = std::max(0.1, 1.0 - m_cpu_usage_ema);
    uint32_t batch_size = static_cast<uint32_t>(m_max_batch_sz * cpu_factor);

    return std::clamp(batch_size, m_min_batch_sz, m_max_batch_sz);
  }

  double getCurrentCpuUsage() const {
#ifdef _WIN32
    return getWindowsCpuUsage();
#elif defined(__APPLE__)
    return getMacOSCpuUsage();
#else
    return getLinuxCpuUsage();
#endif
  }

 private:
  void initializePlatformSpecific() {
#ifdef _WIN32
    PdhOpenQuery(NULL, NULL, &m_cpu_query);
    PdhAddCounter(m_cpu_query, L"\\Processor(_Total)\\% Processor Time", NULL, &m_cpu_counter);
    PdhCollectQueryData(m_cpu_query);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the first sampling.
#elif defined(__APPLE__)
    host_port_ = mach_host_self();
#endif
  }

  void cleanupPlatformSpecific() {
#ifdef _WIN32
    PdhCloseQuery(m_cpu_query);
#endif
  }

#ifdef _WIN32
  double getWindowsCpuUsage() const {
    PDH_FMT_COUNTERVALUE counterVal;
    PdhCollectQueryData(m_cpu_query);
    PdhGetFormattedCounterValue(m_cpu_counter, PDH_FMT_DOUBLE, NULL, &counterVal);
    return std::clamp(counterVal.doubleValue / 100.0, 0.0, 1.0);
  }
#endif

#ifdef __APPLE__
  double getMacOSCpuUsage() const {
    processor_info_array_t cpu_info;
    mach_msg_type_number_t num_cpu_info;
    natural_t num_cpus = 0;

    kern_return_t kr = host_processor_info(host_port_, PROCESSOR_CPU_LOAD_INFO, &num_cpus, &cpu_info, &num_cpu_info);

    if (kr != KERN_SUCCESS) {
      return 0.0;
    }

    processor_cpu_load_info_t cpu_load = (processor_cpu_load_info_t)cpu_info;

    uint64_t total_ticks = 0;
    uint64_t idle_ticks = 0;

    for (natural_t i = 0; i < num_cpus; i++) {
      for (int j = 0; j < CPU_STATE_MAX; j++) {
        total_ticks += cpu_load[i].cpu_ticks[j];
      }
      idle_ticks += cpu_load[i].cpu_ticks[CPU_STATE_IDLE];
    }

    vm_deallocate(mach_task_self(), (vm_address_t)cpu_info, sizeof(processor_cpu_load_info_t) * num_cpus);

    if (total_ticks == 0) return 0.0;
    return std::clamp(1.0 - (static_cast<double>(idle_ticks) / total_ticks), 0.0, 1.0);
  }
#endif

#ifdef __linux__
  bool readCpuTimes(CpuTimes &times) const {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return false;

    std::string line;
    if (!std::getline(file, line)) return false;

    std::istringstream iss(line);
    std::string cpu_label;
    iss >> cpu_label >> times.user >> times.nice >> times.system >> times.idle >> times.iowait >> times.irq >>
        times.softirq >> times.steal;

    return true;
  }

  double getLinuxCpuUsage() const {
    CpuTimes current_times;
    if (!readCpuTimes(current_times)) {
      return 0.0;
    }

    if (!cpu_times_initialized_) {
      m_prev_cpu_times = current_times;
      cpu_times_initialized_ = true;
      return 0.0;
    }

    uint64_t total_diff = current_times.getTotal() - m_prev_cpu_times.getTotal();
    uint64_t idle_diff = current_times.getIdle() - m_prev_cpu_times.getIdle();

    m_prev_cpu_times = current_times;

    if (total_diff == 0) return 0.0;
    return std::clamp(1.0 - (static_cast<double>(idle_diff) / total_diff), 0.0, 1.0);
  }
#endif
};
}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_CONCURRENT_UTIL_H__