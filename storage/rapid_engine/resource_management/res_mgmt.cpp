/* Copyright (c) 2026, Shannon Data AI and/or its affiliates.
   SPDX-License-Identifier: GPL-2.0-only */
#include "storage/rapid_engine/resource_management/res_mgmt.h"

#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <vector>
#include "mysql/psi/mysql_thread.h"
#include "sql/log.h"
#include "sql/resourcegroups/resource_group.h"
#include "sql/resourcegroups/resource_group_mgr.h"
#include "sql/sql_class.h"
#ifdef __linux__
#include <dirent.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace ShannonBase::ResMgmt {
std::atomic<uint64_t> cpu_restore_failures{0};
char *tp_resource_group = nullptr;
char *ap_resource_group = nullptr;
char *sync_resource_group = nullptr;
char *maint_resource_group = nullptr;
char *maint_user_resource_group = nullptr;
WorkGovernor &work_governor() {
  static WorkGovernor governor;
  return governor;
}
namespace {
std::mutex cpu_mutex;
std::unordered_map<unsigned long long, size_t> rapid_threads;
PSI_thread_key resource_thread_key;
std::once_flag thread_key_once;
using Group = resourcegroups::Resource_group;
using Manager = resourcegroups::Resource_group_mgr;
const char *group_name(WorkClass kind) {
  return kind == WorkClass::AP     ? ap_resource_group
         : kind == WorkClass::SYNC ? sync_resource_group
                                   : maint_resource_group;
}
#ifdef __linux__
// Guarded by cpu_mutex; explicit plan validation refreshes this topology.
std::unordered_map<int, std::pair<int, int>> core_topology;
bool cpu_mask(const Group *group, cpu_set_t &mask) {
  CPU_ZERO(&mask);
  if (group->controller()->vcpu_vector().empty()) return false;
  for (const auto &range : group->controller()->vcpu_vector()) {
    if (range.m_end >= CPU_SETSIZE) return false;
    for (auto cpu = range.m_start; cpu <= range.m_end; ++cpu) CPU_SET(cpu, &mask);
  }
  return CPU_COUNT(&mask) != 0;
}
// Native groups express logical CPU IDs. Reject shared SMT cores as well.
bool physical_cores(const cpu_set_t &mask, std::set<std::pair<int, int>> &cores) {
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (!CPU_ISSET(cpu, &mask)) continue;
    auto found = core_topology.find(cpu);
    if (found == core_topology.end()) {
      const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
      int package, core;
      if (!(std::ifstream(path + "physical_package_id") >> package) || !(std::ifstream(path + "core_id") >> core))
        return false;
      found = core_topology.emplace(cpu, std::make_pair(package, core)).first;
    }
    cores.insert(found->second);
  }
  return true;
}
#endif
class Pins {
 public:
  explicit Pins(THD *thd) : m_thd(thd) {}
  ~Pins() {
    if (m_tickets.empty()) return;
    auto *mgr = Manager::instance();
    for (auto *ticket : m_tickets) mgr->release_shared_mdl_for_resource_group(m_thd, ticket);
  }
  Group *get(const char *name) {
    if (!name || !*name) return nullptr;
    MDL_ticket *ticket = nullptr;
    auto *mgr = Manager::instance();
    // Never queue behind ALTER/DROP GROUP while holding a source table MDL.
    if (mgr->acquire_shared_mdl_for_resource_group(m_thd, name, MDL_EXPLICIT, &ticket, true)) return nullptr;
    m_tickets.push_back(ticket);
    return mgr->get_resource_group(name);
  }

 private:
  THD *m_thd;
  std::vector<MDL_ticket *> m_tickets;
};
const char *check_plan(Pins &pins, std::unique_lock<std::mutex> &cpu_lock, unsigned retries = 20) {
#ifndef __linux__
  return "Rapid STRICT_CPU requires Linux CPU affinity and topology support";
#else
  const char *names[] = {tp_resource_group, ap_resource_group, sync_resource_group, maint_resource_group};
  std::set<std::pair<int, int>> occupied;
  cpu_set_t tp_mask;
  std::unordered_map<unsigned long long, cpu_set_t> classified;
  for (size_t i = 0; i < 4; ++i) {
    auto *group = pins.get(names[i]);
    if (!group || !group->enabled()) return "Rapid STRICT_CPU requires enabled TP/AP/SYNC/MAINT native resource groups";
    const auto expected =
        i < 2 ? resourcegroups::Type::USER_RESOURCE_GROUP : resourcegroups::Type::SYSTEM_RESOURCE_GROUP;
    if (group->type() != expected) return "Rapid resource group type mismatch: TP/AP must be USER, SYNC/MAINT SYSTEM";
    cpu_set_t mask;
    if (!cpu_mask(group, mask)) return "Rapid STRICT_CPU requires explicit nonempty CPU ranges";
    std::set<std::pair<int, int>> cores;
    if (!physical_cores(mask, cores)) return "Rapid cannot verify CPU topology";
    for (auto core : cores)
      if (!occupied.insert(core).second) return "Rapid STRICT_CPU groups share a physical CPU core";
    if (!i)
      tp_mask = mask;
    else
      group->apply_control_func([&](ulonglong id) {
        PSI_thread_attrs attrs{};
        if (!Manager::instance()->get_thread_attributes(&attrs, id) && attrs.m_thread_os_id)
          classified[attrs.m_thread_os_id] = mask;
      });
  }
  auto *maint_user = pins.get(maint_user_resource_group);
  auto *maint_system = pins.get(maint_resource_group);
  cpu_set_t user_mask, system_mask;
  if (!maint_user || !maint_user->enabled() || maint_user->type() != resourcegroups::Type::USER_RESOURCE_GROUP ||
      !cpu_mask(maint_user, user_mask) || !cpu_mask(maint_system, system_mask) || !CPU_EQUAL(&user_mask, &system_mask))
    return "Rapid STRICT_CPU foreground MAINT USER group must share the SYSTEM MAINT CPU range";
  maint_user->apply_control_func([&](ulonglong id) {
    PSI_thread_attrs attrs{};
    if (!Manager::instance()->get_thread_attributes(&attrs, id) && attrs.m_thread_os_id)
      classified[attrs.m_thread_os_id] = user_mask;
  });
  // Include uninstrumented source/background threads; PFS alone misses them.
  // Deployment must first bind all ordinary server threads to the TP partition.
  DIR *tasks = opendir("/proc/self/task");
  if (!tasks) return "Rapid cannot audit source thread CPU coverage";
  const char *error = nullptr;
  while (auto *entry = readdir(tasks)) {
    char *end = nullptr;
    const auto tid = strtoull(entry->d_name, &end, 10);
    if (!tid || *end || rapid_threads.count(tid)) continue;
    cpu_set_t mask;
    if (sched_getaffinity(tid, sizeof(mask), &mask)) {
      if (errno == ESRCH) continue;
      error = "Rapid cannot read source thread CPU affinity";
      break;
    }
    const auto covered = [&](const cpu_set_t &allowed) {
      cpu_set_t intersection;
      CPU_AND(&intersection, &mask, &allowed);
      return CPU_EQUAL(&intersection, &mask);
    };
    const auto known = classified.find(tid);
    if (!covered(tp_mask) && (known == classified.end() || !covered(known->second))) {
      error = "Rapid STRICT_CPU source threads are not confined to the TP CPU partition";
      break;
    }
  }
  closedir(tasks);
  if (error && retries) {
    // A worker born from an admitted SYNC/MAINT thread inherits that CPU
    // partition before WorkerRegistration can take cpu_mutex. Let registration
    // finish, then repeat the complete audit; never accept an unknown thread.
    cpu_lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    cpu_lock.lock();
    return check_plan(pins, cpu_lock, retries - 1);
  }
  return error;
#endif
}
}  // namespace
WorkerRegistration::WorkerRegistration() {
#ifdef __linux__
  m_tid = syscall(SYS_gettid);
  std::lock_guard lock(cpu_mutex);
  ++rapid_threads[m_tid];
#endif
}
WorkerRegistration::~WorkerRegistration() {
#ifdef __linux__
  std::lock_guard lock(cpu_mutex);
  if (m_tid && --rapid_threads[m_tid] == 0) rapid_threads.erase(m_tid);
#endif
}
const char *validate_cpu_plan(THD *thd) {
  auto *mgr = Manager::instance();
  if (!mgr || !mgr->resource_group_support()) return "Rapid native CPU resource groups are unavailable";
  std::unique_lock lock(cpu_mutex);
#ifdef __linux__
  core_topology.clear();
#endif
  Pins pins(thd);
  return check_plan(pins, lock);
}
struct WorkScope::State {
  THD *thd;
  WorkGovernor::Lease lease;
  Pins pins;
  Group *original{nullptr}, *original_context{nullptr}, *destination{nullptr};
  PSI_thread *instrument{nullptr};
  bool switched{false};
#ifdef __linux__
  cpu_set_t original_mask;
  bool have_mask{false};
  unsigned long long tid{0};
#endif
  explicit State(THD *value) : thd(value), pins(value) {}
  ~State() {
    std::lock_guard lock(cpu_mutex);
    if (switched) {
      auto *mgr = Manager::instance();
      mysql_mutex_lock(&thd->LOCK_thd_data);
      // SET RESOURCE GROUP from another connection may have superseded us.
      if (thd->resource_group_ctx()->m_cur_resource_group == destination) {
        bool failed = original != destination && !mgr->move_resource_group(destination, original);
        if (!failed) thd->resource_group_ctx()->m_cur_resource_group = original_context;
#ifdef __linux__
        if (have_mask && sched_setaffinity(0, sizeof(original_mask), &original_mask)) failed = true;
#endif
        if (failed) {
          ++cpu_restore_failures;
          sql_print_error("Rapid failed to restore original thread resource group/CPU affinity; terminating session");
          thd->killed = THD::KILL_CONNECTION;
        }
      }
      mysql_mutex_unlock(&thd->LOCK_thd_data);
    }
#ifdef __linux__
    if (tid && --rapid_threads[tid] == 0) rapid_threads.erase(tid);
#endif
#ifdef HAVE_PSI_THREAD_INTERFACE
    if (instrument) {
      PSI_THREAD_CALL(delete_current_thread)();
    }
#endif
  }
};
WorkScope::WorkScope() = default;
WorkScope::~WorkScope() = default;
void WorkScope::reset() { m_state.reset(); }
const char *WorkScope::enter(THD *thd, WorkClass kind, bool wait, const std::function<bool()> &cancelled) {
  reset();
  if (!thd) return "Rapid resource operation has no SQL thread context";
  auto state = std::make_unique<State>(thd);
  state->lease = wait ? work_governor().acquire(kind, cancelled) : work_governor().try_acquire(kind);
  if (!state->lease) {
    if (work_governor().stopping()) return "Rapid resource admission stopped during shutdown";
    if (wait && cancelled()) return "Rapid background resource admission cancelled";
    switch (kind) {
      case WorkClass::AP:
        return "Rapid AP active-query budget exhausted (rapid_resource_ap_limit)";
      case WorkClass::SYNC:
        return "Rapid SYNC active-operation budget exhausted (rapid_resource_sync_limit)";
      case WorkClass::MAINT:
        return "Rapid MAINT active-operation budget exhausted (rapid_resource_maint_limit)";
      default:
        return "Rapid resource admission failed";
    }
  }
  const auto mode = work_governor().mode();
  const char *name = group_name(kind);
  if (mode == IsolationMode::OFF || mode == IsolationMode::OBSERVE ||
      ((!name || !*name) && !(kind == WorkClass::MAINT && maint_user_resource_group && *maint_user_resource_group))) {
    if (mode == IsolationMode::STRICT_CPU) return "Rapid STRICT_CPU resource group is not configured";
    m_state = std::move(state);
    return nullptr;
  }
  auto *mgr = Manager::instance();
  if (!mgr || !mgr->resource_group_support()) return "Rapid native CPU resource groups are unavailable";
  std::unique_lock lock(cpu_mutex);
  // Register raw std::threads as SYSTEM threads, without changing user THDs.
#ifdef HAVE_PSI_THREAD_INTERFACE
  if (kind != WorkClass::AP && !PSI_THREAD_CALL(get_thread)()) {
    std::call_once(thread_key_once, [] {
      PSI_thread_info info[] = {{&resource_thread_key, "resource_worker", "rapid_res",
                                 PSI_FLAG_THREAD_SYSTEM | PSI_FLAG_AUTO_SEQNUM, 0, PSI_DOCUMENT_ME}};
      mysql_thread_register("rapid", info, 1);
    });
    state->instrument = PSI_THREAD_CALL(new_thread)(resource_thread_key, 0, nullptr, 0);
    if (state->instrument) {
      PSI_THREAD_CALL(set_thread_os_id)(state->instrument);
      PSI_THREAD_CALL(set_thread)(state->instrument);
    }
  }
  PSI_thread_attrs attrs{};
  const auto pfs_id = PSI_THREAD_CALL(get_current_thread_internal_id)();
  if (mgr->get_thread_attributes(&attrs, pfs_id)) return "Rapid cannot identify native resource group thread type";
  const auto expected =
      attrs.m_system_thread ? resourcegroups::Type::SYSTEM_RESOURCE_GROUP : resourcegroups::Type::USER_RESOURCE_GROUP;
  if (kind != WorkClass::MAINT && attrs.m_system_thread != (kind != WorkClass::AP))
    return "Rapid native resource group thread type mismatch";
  if (kind == WorkClass::MAINT && !attrs.m_system_thread) name = maint_user_resource_group;
  if (!name || !*name) {
    if (mode == IsolationMode::STRICT_CPU) return "Rapid STRICT_CPU resource group is not configured";
    m_state = std::move(state);
    return nullptr;
  }
  auto *destination = state->pins.get(name);
  if (!destination || !destination->enabled() || destination->type() != expected)
    return "Rapid configured resource group is missing, disabled or has the wrong type";
#ifdef __linux__
  state->tid = syscall(SYS_gettid);
  ++rapid_threads[state->tid];
#endif
  if (mode == IsolationMode::STRICT_CPU) {
    if (const char *error = check_plan(state->pins, lock)) return error;
  }
  if (attrs.m_groupname_length > sizeof(attrs.m_groupname)) return "Rapid native resource group name is invalid";
  const std::string original_name = attrs.m_groupname_length
                                        ? std::string(attrs.m_groupname, attrs.m_groupname_length)
                                        : std::string(attrs.m_system_thread ? mgr->sys_default_resource_group_name()
                                                                            : mgr->usr_default_resource_group_name());
  auto *original = state->pins.get(original_name.c_str());
  if (!original) return "Rapid cannot pin original native resource group";
  state->original = original;
  state->destination = destination;
  mysql_mutex_lock(&thd->LOCK_thd_data);
  PSI_thread_attrs latest{};
  if (mgr->get_thread_attributes(&latest, pfs_id) || latest.m_groupname_length > sizeof(latest.m_groupname) ||
      std::string(latest.m_groupname, latest.m_groupname_length) != original_name) {
    mysql_mutex_unlock(&thd->LOCK_thd_data);
    return "Rapid original resource group changed during admission";
  }
#ifdef __linux__
  state->have_mask = sched_getaffinity(0, sizeof(state->original_mask), &state->original_mask) == 0;
  if (!state->have_mask) {
    mysql_mutex_unlock(&thd->LOCK_thd_data);
    return "Rapid cannot capture original CPU affinity";
  }
#endif
  state->original_context = thd->resource_group_ctx()->m_cur_resource_group;
  // apply_control may set affinity before a priority change fails. Mark the
  // attempted binding for rollback even when move_resource_group returns false.
  thd->resource_group_ctx()->m_cur_resource_group = destination;
  state->switched = true;
  if (original != destination && !mgr->move_resource_group(original, destination)) {
    mysql_mutex_unlock(&thd->LOCK_thd_data);
    return "Rapid failed to apply native CPU resource group";
  }
#ifdef __linux__
  if (mode == IsolationMode::STRICT_CPU) {
    cpu_set_t expected_mask, actual_mask;
    if (!cpu_mask(destination, expected_mask) || sched_getaffinity(0, sizeof(actual_mask), &actual_mask) ||
        !CPU_EQUAL(&expected_mask, &actual_mask)) {
      mysql_mutex_unlock(&thd->LOCK_thd_data);
      return "Rapid native CPU group does not match effective affinity (CPU/cpuset restriction)";
    }
  }
#endif
  mysql_mutex_unlock(&thd->LOCK_thd_data);
#else
  return "Rapid native resource groups require thread instrumentation";
#endif
  m_state = std::move(state);
  return nullptr;
}
}  // namespace ShannonBase::ResMgmt
