/* Copyright (c) 2026, Shannon Data AI and/or its affiliates.
   SPDX-License-Identifier: GPL-2.0-only */
#include "storage/rapid_engine/resource_management/res_mgmt.h"

#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <vector>
#include "mysql/components/services/pfs_notification.h"
#include "mysql/psi/mysql_thread.h"
#include "mysql/service_plugin_registry.h"
#include "scope_guard.h"
#include "sql/dd/types/resource_group.h"
#include "sql/log.h"
#include "sql/mysqld_thd_manager.h"
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
std::unordered_map<unsigned long long, size_t> native_scopes;
PSI_thread_key resource_thread_key;
std::once_flag thread_key_once;
using Group = resourcegroups::Resource_group;
using Manager = resourcegroups::Resource_group_mgr;
const char *auto_names[] = {"__rapid_auto_tp", "__rapid_auto_ap", "__rapid_auto_sync", "__rapid_auto_maint",
                            "__rapid_auto_maint_user"};
std::atomic<bool> auto_native{false};
std::atomic<bool> auto_restoring{false};
std::mutex auto_lifecycle_mutex;
std::string auto_reason{"startup initialization pending"};
AutoCpuPlan auto_plan;
std::array<size_t, 3> auto_limits{DEFAULT_AP_LIMIT, DEFAULT_SYNC_LIMIT, DEFAULT_MAINT_LIMIT};
std::unique_ptr<THD> auto_owner;
std::array<Group *, 5> auto_groups{};
std::array<MDL_ticket *, 5> auto_tickets{};
SERVICE_TYPE(registry) * auto_registry{nullptr};
my_h_service auto_service_handle{nullptr};
SERVICE_TYPE(pfs_notification_v3) * auto_notifications{nullptr};
int auto_notification_handle{0};
thread_local bool rapid_notification{false};
struct NotificationGuard {
  bool previous{rapid_notification};
  NotificationGuard() { rapid_notification = true; }
  ~NotificationGuard() { rapid_notification = previous; }
};
const char *maint_user_name() { return auto_native.load() ? auto_names[4] : maint_user_resource_group; }
const char *group_name(WorkClass kind) {
  if (auto_native.load()) return auto_names[static_cast<size_t>(kind) + 1];
  return kind == WorkClass::AP     ? ap_resource_group
         : kind == WorkClass::SYNC ? sync_resource_group
                                   : maint_resource_group;
}
#ifdef __linux__
class FindSourceThread final : public Find_THD_Impl {
 public:
  explicit FindSourceThread(unsigned long long tid) : m_tid(tid) {}
  bool operator()(THD *thd) override {
#ifdef HAVE_PSI_THREAD_INTERFACE
    auto *psi = thd->get_psi();
    if (!psi) return false;
    PSI_thread_attrs attrs{};
    return !Manager::instance()->get_thread_attributes(&attrs, PSI_THREAD_CALL(get_thread_internal_id)(psi)) &&
           attrs.m_thread_os_id == m_tid;
#else
    return false;
#endif
  }

 private:
  unsigned long long m_tid;
};
THD_ptr pin_source_thread(unsigned long long tid) {
  FindSourceThread find(tid);
  return Global_THD_manager::get_instance()->find_thd(&find);
}
struct SourceBinding {
  cpu_set_t original, applied;
  uint64_t start_time;
};
std::unordered_map<unsigned long long, SourceBinding> auto_bindings;
cpu_set_t auto_available;
uint64_t thread_start(unsigned long long tid) {
  std::ifstream input("/proc/self/task/" + std::to_string(tid) + "/stat");
  std::string line;
  if (!std::getline(input, line)) return 0;
  const auto end = line.rfind(')');
  if (end == std::string::npos) return 0;
  std::istringstream fields(line.substr(end + 2));
  std::string field;
  for (int index = 3; index <= 22; ++index) {
    if (!(fields >> field)) return 0;
    if (index == 22) return strtoull(field.c_str(), nullptr, 10);
  }
  return 0;
}
bool default_group(const PSI_thread_attrs &attrs) {
  auto *mgr = Manager::instance();
  if (attrs.m_groupname_length > sizeof(attrs.m_groupname)) return false;
  const std::string name(attrs.m_groupname, attrs.m_groupname_length);
  return name.empty() || name == mgr->usr_default_resource_group_name() ||
         name == mgr->sys_default_resource_group_name();
}
// Only undo a binding while its exact affinity and Linux thread identity match.
// SET RESOURCE GROUP or an external affinity change supersedes AUTO ownership.
bool explicitly_assigned(unsigned long long tid);
void restore_binding(unsigned long long tid) {
  auto found = auto_bindings.find(tid);
  if (found == auto_bindings.end()) return;
  auto source = pin_source_thread(tid);
  if (explicitly_assigned(tid)) {
    auto_bindings.erase(found);
    return;
  }
  cpu_set_t current;
  if (thread_start(tid) == found->second.start_time && !sched_getaffinity(tid, sizeof(current), &current) &&
      CPU_EQUAL(&current, &found->second.applied)) {
    if (sched_setaffinity(tid, sizeof(current), &found->second.original)) {
      if (errno != ESRCH) ++cpu_restore_failures;
      return;
    }
  }
  auto_bindings.erase(found);
}
void degrade_auto(const char *reason) {
  if (auto_native.load() || !auto_bindings.empty()) auto_restoring.store(true);
  auto_native.store(false);
  auto_reason = reason;
  std::vector<unsigned long long> tids;
  for (const auto &entry : auto_bindings) tids.push_back(entry.first);
  for (auto tid : tids)
    if (!native_scopes.count(tid)) restore_binding(tid);
}
void auto_thread_callback(const PSI_thread_attrs *notification) {
  if (rapid_notification || (!auto_native.load() && !auto_restoring.load())) return;
  std::lock_guard lock(cpu_mutex);
  if (!auto_native.load() && !auto_restoring.load()) return;
  THD_ptr source(notification->m_thread_os_id == static_cast<unsigned long long>(syscall(SYS_gettid)) ? current_thd
                                                                                                      : nullptr);
  PSI_thread_attrs attrs{};
  if (Manager::instance()->get_thread_attributes(&attrs, notification->m_thread_internal_id) || !attrs.m_thread_os_id ||
      !default_group(attrs) || rapid_threads.count(attrs.m_thread_os_id))
    return;
  const auto tid = attrs.m_thread_os_id;
  cpu_set_t current, tp;
  CPU_ZERO(&tp);
  for (auto cpu : auto_plan.cpus[0]) CPU_SET(cpu, &tp);
  if (sched_getaffinity(tid, sizeof(current), &current)) {
    if (errno != ESRCH) {
      source = THD_ptr{};
      degrade_auto("cannot read new source thread affinity");
    }
    return;
  }
  if (!auto_native.load()) {
    // A child can inherit TP while its parent is being restored. Drain these
    // notifications before removing the groups or discarding the saved plan.
    if (CPU_EQUAL(&current, &tp) && sched_setaffinity(tid, sizeof(current), &auto_available) && errno != ESRCH)
      ++cpu_restore_failures;
    auto_bindings.erase(tid);
    return;
  }
  if (!CPU_EQUAL(&current, &auto_available) && !CPU_EQUAL(&current, &tp)) {
    source = THD_ptr{};
    degrade_auto("source thread has an explicit CPU restriction; software admission retained");
    return;
  }
  const auto start = thread_start(tid);
  if (!start) return;
  // Cached connection threads can outlive their PFS instrument. Keep ownership
  // until their OS thread exits, pruning dead identities periodically.
  static unsigned notifications = 0;
  if (++notifications % 64 == 0) {
    for (auto it = auto_bindings.begin(); it != auto_bindings.end();) {
      if (thread_start(it->first) != it->second.start_time)
        it = auto_bindings.erase(it);
      else
        ++it;
    }
  }
  if (!auto_bindings.count(tid) || auto_bindings[tid].start_time != start)
    auto_bindings[tid] = SourceBinding{auto_available, tp, start};
  if (sched_setaffinity(tid, sizeof(tp), &tp) && errno != ESRCH) {
    source = THD_ptr{};
    degrade_auto("cannot bind new source thread to TP CPUs");
  }
}
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
    MDL_key key;
    dd::Resource_group::create_mdl_key(name, &key);
    MDL_request request;
    MDL_REQUEST_INIT_BY_KEY(&request, &key, MDL_INTENTION_EXCLUSIVE, MDL_EXPLICIT);
    // An unavailable optional native binding must not set a SQL timeout error:
    // AUTO can continue with software admission after a failed nonblocking pin.
    if (m_thd->mdl_context.try_acquire_lock(&request) || !request.ticket) return nullptr;
    ticket = request.ticket;
    m_tickets.push_back(ticket);
    return mgr->get_resource_group(name);
  }

 private:
  THD *m_thd;
  std::vector<MDL_ticket *> m_tickets;
};
#ifdef __linux__
bool explicitly_assigned(unsigned long long tid) {
  if (!auto_owner) return false;
  auto *mgr = Manager::instance();
  Pins pins(auto_owner.get());
  for (const auto &name : mgr->resource_group_names()) {
    if (name == mgr->usr_default_resource_group_name() || name == mgr->sys_default_resource_group_name()) continue;
    auto *group = pins.get(name.c_str());
    if (!group) return true;
    bool assigned = false;
    group->apply_control_func([&](ulonglong id) {
      PSI_thread_attrs attrs{};
      if (!mgr->get_thread_attributes(&attrs, id) && attrs.m_thread_os_id == tid) assigned = true;
    });
    if (assigned) return true;
  }
  return false;
}
#endif
const char *check_plan(Pins &pins, std::unique_lock<std::mutex> &cpu_lock, unsigned retries = 20,
                       bool automatic = false) {
#ifndef __linux__
  return "Rapid STRICT_CPU requires Linux CPU affinity and topology support";
#else
  const char *names[] = {automatic ? auto_names[0] : tp_resource_group, automatic ? auto_names[1] : ap_resource_group,
                         automatic ? auto_names[2] : sync_resource_group,
                         automatic ? auto_names[3] : maint_resource_group};
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
  auto *maint_user = pins.get(automatic ? auto_names[4] : maint_user_resource_group);
  auto *maint_system = pins.get(names[3]);
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
    return check_plan(pins, cpu_lock, retries - 1, automatic);
  }
  return error;
#endif
}
}  // namespace
bool limits_execution(IsolationMode mode) {
  return mode == IsolationMode::SOFT || mode == IsolationMode::STRICT_CPU || mode == IsolationMode::AUTO;
}
void stop_auto_resources() {
  std::lock_guard lifecycle(auto_lifecycle_mutex);
  {
    std::lock_guard lock(cpu_mutex);
#ifdef __linux__
    degrade_auto("AUTO disabled");
#else
    auto_native.store(false);
#endif
  }
  // Notification callbacks take cpu_mutex; drain them without that lock held.
  if (auto_notifications && auto_notification_handle) {
    while (auto_notifications->unregister_notification(auto_notification_handle)) std::this_thread::yield();
    auto_notification_handle = 0;
  }
  std::lock_guard lock(cpu_mutex);
  auto *mgr = Manager::instance();
  if (mgr && mgr->resource_group_support()) {
    for (size_t i = 0; i < auto_groups.size(); ++i) {
      auto *group = auto_groups[i];
      if (group && mgr->get_resource_group(auto_names[i]) == group && !group->is_bound_to_threads())
        mgr->remove_resource_group(auto_names[i]);
      auto_groups[i] = nullptr;
    }
  }
  if (auto_owner) {
    for (auto &ticket : auto_tickets) {
      if (ticket) auto_owner->mdl_context.release_lock(ticket);
      ticket = nullptr;
    }
  }
  if (auto_registry) {
    if (auto_service_handle) auto_registry->release(auto_service_handle);
    mysql_plugin_registry_release(auto_registry);
  }
  auto_service_handle = nullptr;
  auto_registry = nullptr;
  auto_notifications = nullptr;
  auto_restoring.store(false);
  auto_plan = {};
}
static void provision_auto_resources();
// THD initialization/cleanup take LOCK_global_system_variables. The owner must
// live across runtime SET GLOBAL callbacks, which already hold that mutex.
void start_auto_resources() {
  std::lock_guard lifecycle(auto_lifecycle_mutex);
  std::lock_guard lock(cpu_mutex);
  if (!auto_owner && Manager::instance()->resource_group_support()) {
    auto_owner = std::make_unique<THD>();
    auto_owner->security_context()->skip_grants();
  }
}
void shutdown_auto_resources() {
  stop_auto_resources();
  std::lock_guard lifecycle(auto_lifecycle_mutex);
  std::lock_guard lock(cpu_mutex);
  if (auto_owner) {
    auto_owner->release_resources();
    auto_owner.reset();
  }
}
bool configure_auto_resources(IsolationMode mode, std::array<size_t, 3> limits) {
  if (!work_governor().configure(mode, limits, false)) {
    std::lock_guard lock(cpu_mutex);
    if (mode == IsolationMode::AUTO && work_governor().mode() == IsolationMode::AUTO &&
        auto_reason == "startup initialization pending")
      auto_reason = "work is active during startup; software admission retained";
    return false;
  }
  stop_auto_resources();
  {
    std::lock_guard lock(cpu_mutex);
    auto_limits = limits;
    auto_reason = "startup initialization pending";
  }
  if (mode == IsolationMode::AUTO)
    provision_auto_resources();
  else
    work_governor().configure(mode, limits);
  return true;
}
static void provision_auto_resources() {
  if (work_governor().mode() != IsolationMode::AUTO) return;
  std::lock_guard lifecycle(auto_lifecycle_mutex);
  std::lock_guard lock(cpu_mutex);
  if (auto_registry) return;
  auto finish = create_scope_guard([&] {
    // Admission stays paused until all group setup and source binding is done.
    if (!work_governor().configure(IsolationMode::AUTO, auto_limits)) {
#ifdef __linux__
      degrade_auto("work is active during AUTO setup; software admission retained");
#else
      auto_reason = "work is active during AUTO setup; software admission retained";
#endif
    }
  });
  auto_reason = "native CPU isolation unavailable; software admission retained";
#ifndef __linux__
  auto_reason = "Linux CPU affinity is unavailable; software admission retained";
#else
  auto *mgr = Manager::instance();
  if ((tp_resource_group && *tp_resource_group) || (ap_resource_group && *ap_resource_group) ||
      (sync_resource_group && *sync_resource_group) || (maint_resource_group && *maint_resource_group) ||
      (maint_user_resource_group && *maint_user_resource_group)) {
    auto_reason = "explicit resource group configuration present; software admission retained";
    return;
  }
  if (sched_getaffinity(getpid(), sizeof(auto_available), &auto_available)) {
    auto_reason = "cannot read effective server CPU affinity";
    return;
  }
  core_topology.clear();
  std::vector<CpuTopologyEntry> topology;
  for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (!CPU_ISSET(cpu, &auto_available)) continue;
    const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
    int package, core;
    if (!(std::ifstream(path + "physical_package_id") >> package) || !(std::ifstream(path + "core_id") >> core)) {
      auto_reason = "cannot verify physical CPU topology";
      return;
    }
    topology.push_back({cpu, package, core});
  }
  auto_plan = make_auto_cpu_plan(topology, auto_limits);
  DBUG_EXECUTE_IF("rapid_auto_insufficient_cpus", { auto_plan.available = false; });
  const auto limits = auto_plan.available ? auto_plan.limits : std::array<size_t, 3>{1, 1, 1};
  auto_limits = limits;
  if (!auto_plan.available) {
    auto_reason = "fewer than five available physical CPU cores; software admission retained";
    return;
  }
  DBUG_EXECUTE_IF("rapid_auto_no_native_services", {
    auto_reason = "native resource services unavailable; software admission retained";
    return;
  });
  if (!mgr || !mgr->resource_group_support()) return;
  if (!auto_owner) {
    auto_reason = "native metadata lock owner unavailable; software admission retained";
    return;
  }
  Pins existing(auto_owner.get());
  for (const auto &name : mgr->resource_group_names()) {
    auto *group = existing.get(name.c_str());
    if (!group || (name != mgr->usr_default_resource_group_name() && name != mgr->sys_default_resource_group_name() &&
                   group->is_bound_to_threads())) {
      auto_reason = "DBA resource group assignment present; software admission retained";
      return;
    }
  }
  // Fail closed on existing per-thread restrictions instead of replacing them.
  DIR *tasks = opendir("/proc/self/task");
  if (!tasks) {
    auto_reason = "cannot enumerate source threads";
    return;
  }
  bool compatible = true;
  while (auto *entry = readdir(tasks)) {
    char *end = nullptr;
    auto tid = strtoull(entry->d_name, &end, 10);
    if (!tid || *end || rapid_threads.count(tid)) continue;
    cpu_set_t mask;
    if (sched_getaffinity(tid, sizeof(mask), &mask)) {
      if (errno == ESRCH) continue;
      compatible = false;
      break;
    }
    if (!CPU_EQUAL(&mask, &auto_available)) {
      compatible = false;
      break;
    }
  }
  closedir(tasks);
  if (!compatible) {
    auto_reason = "existing source CPU restriction present; software admission retained";
    return;
  }
  auto_registry = mysql_plugin_registry_acquire();
  if (!auto_registry || auto_registry->acquire("pfs_notification_v3", &auto_service_handle)) {
    auto_reason = "thread notification service unavailable; software admission retained";
    return;
  }
  auto_notifications = reinterpret_cast<SERVICE_TYPE(pfs_notification_v3) *>(auto_service_handle);
  PSI_notification callbacks{};
  callbacks.thread_create = auto_thread_callback;
  callbacks.session_connect = auto_thread_callback;
  callbacks.session_disconnect = auto_thread_callback;
  auto_notification_handle = auto_notifications->register_notification(&callbacks, true);
  if (!auto_notification_handle) {
    auto_reason = "cannot register source thread notifications";
    return;
  }
  for (size_t i = 0; i < auto_groups.size(); ++i) {
    MDL_key key;
    dd::Resource_group::create_mdl_key(auto_names[i], &key);
    MDL_request request;
    MDL_REQUEST_INIT_BY_KEY(&request, &key, MDL_EXCLUSIVE, MDL_EXPLICIT);
    if (mgr->get_resource_group(auto_names[i]) || auto_owner->mdl_context.try_acquire_lock(&request) ||
        !request.ticket) {
      auto_reason = "reserved AUTO resource group name is in use; software admission retained";
      return;
    }
    auto_tickets[i] = request.ticket;
    auto ranges = std::make_unique<std::vector<resourcegroups::Range>>();
    for (auto cpu : auto_plan.cpus[i == 4 ? 3 : i]) ranges->emplace_back(cpu, cpu);
    const auto type =
        i < 2 || i == 4 ? resourcegroups::Type::USER_RESOURCE_GROUP : resourcegroups::Type::SYSTEM_RESOURCE_GROUP;
    auto_groups[i] = mgr->create_and_add_in_resource_group_hash({auto_names[i], strlen(auto_names[i])}, type, true,
                                                                std::move(ranges), 0);
    if (!auto_groups[i]) {
      auto_reason = "cannot create AUTO native resource group";
      return;
    }
    request.ticket->downgrade_lock(MDL_INTENTION_EXCLUSIVE);
  }
  cpu_set_t tp;
  CPU_ZERO(&tp);
  for (auto cpu : auto_plan.cpus[0]) CPU_SET(cpu, &tp);
  tasks = opendir("/proc/self/task");
  if (!tasks) {
    auto_reason = "cannot enumerate source threads";
    return;
  }
  auto_native.store(true);
  while (auto *entry = readdir(tasks)) {
    char *end = nullptr;
    auto tid = strtoull(entry->d_name, &end, 10);
    if (!tid || *end || rapid_threads.count(tid)) continue;
    cpu_set_t current;
    const auto start = thread_start(tid);
    if (!start || sched_getaffinity(tid, sizeof(current), &current)) continue;
    if (!CPU_EQUAL(&current, &auto_available)) {
      degrade_auto("source affinity changed during AUTO setup; software admission retained");
      break;
    }
    auto source = pin_source_thread(tid);
    if (explicitly_assigned(tid)) {
      source = THD_ptr{};
      degrade_auto("DBA assignment changed during AUTO setup; software admission retained");
      break;
    }
    auto_bindings[tid] = {current, tp, start};
    bool denied = false;
    DBUG_EXECUTE_IF("rapid_auto_bind_denied", {
      denied = true;
      errno = EPERM;
    });
    if (denied || sched_setaffinity(tid, sizeof(tp), &tp)) {
      if (errno == ESRCH) {
        auto_bindings.erase(tid);
        continue;
      }
      source = THD_ptr{};
      degrade_auto("CPU affinity permission denied; software admission retained");
      break;
    }
  }
  closedir(tasks);
  if (auto_native.load()) {
    auto_reason = "none";
  }
#endif
}
std::string auto_resource_status() {
  if (work_governor().mode() != IsolationMode::AUTO) return {};
  std::lock_guard lock(cpu_mutex);
  std::ostringstream out;
  out << "; configured=AUTO effective=" << (auto_native.load() ? "STRICT_CPU" : "SOFT") << " limits=" << auto_limits[0]
      << "/" << auto_limits[1] << "/" << auto_limits[2];
  if (auto_native.load()) {
    const char *roles[] = {"TP", "AP", "SYNC", "MAINT"};
    for (size_t i = 0; i < 4; ++i) {
      out << " " << roles[i] << "=";
      auto cpus = auto_plan.cpus[i];
      std::sort(cpus.begin(), cpus.end());
      for (size_t j = 0; j < cpus.size(); ++j) {
        if (j) out << ',';
        const auto first = cpus[j];
        out << first;
        while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) ++j;
        if (cpus[j] != first) out << '-' << cpus[j];
      }
    }
  }
  out << " fallback_reason=" << auto_reason;
  return out.str();
}
WorkerRegistration::WorkerRegistration() {
#ifdef __linux__
  m_tid = syscall(SYS_gettid);
  std::lock_guard lock(cpu_mutex);
  ++rapid_threads[m_tid];
  if (auto_native.load()) {
    cpu_set_t current;
    if (!sched_getaffinity(m_tid, sizeof(current), &current)) {
      for (const auto &cpus : auto_plan.cpus) {
        cpu_set_t owned;
        CPU_ZERO(&owned);
        for (auto cpu : cpus) CPU_SET(cpu, &owned);
        if (CPU_EQUAL(&current, &owned)) {
          auto_bindings[m_tid] = {auto_available, current, thread_start(m_tid)};
          break;
        }
      }
    }
  }
#endif
}
WorkerRegistration::~WorkerRegistration() {
#ifdef __linux__
  std::lock_guard lock(cpu_mutex);
  if (m_tid && --rapid_threads[m_tid] == 0) rapid_threads.erase(m_tid);
  if (!auto_native.load()) restore_binding(m_tid);
  auto_bindings.erase(m_tid);
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
#ifdef __linux__
        cpu_set_t external_mask, destination_mask;
        const bool externally_changed = work_governor().mode() == IsolationMode::AUTO &&
                                        cpu_mask(destination, destination_mask) &&
                                        !sched_getaffinity(0, sizeof(external_mask), &external_mask) &&
                                        !CPU_EQUAL(&external_mask, &destination_mask);
#endif
        bool failed = original != destination && !mgr->move_resource_group(destination, original);
        if (!failed) thd->resource_group_ctx()->m_cur_resource_group = original_context;
#ifdef __linux__
        const auto &restore_mask = externally_changed ? external_mask : original_mask;
        if (have_mask && sched_setaffinity(0, sizeof(restore_mask), &restore_mask)) failed = true;
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
    if (tid && --native_scopes[tid] == 0) native_scopes.erase(tid);
    if (tid && !auto_native.load() && !native_scopes.count(tid)) restore_binding(tid);
#endif
#ifdef HAVE_PSI_THREAD_INTERFACE
    if (instrument) {
      NotificationGuard notification_guard;
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
  if (mode == IsolationMode::AUTO && !auto_native.load()) {
    m_state = std::move(state);
    return nullptr;
  }
  if (mode == IsolationMode::OFF || mode == IsolationMode::OBSERVE ||
      ((!name || !*name) && !(kind == WorkClass::MAINT && maint_user_resource_group && *maint_user_resource_group))) {
    if (mode == IsolationMode::STRICT_CPU) return "Rapid STRICT_CPU resource group is not configured";
    m_state = std::move(state);
    return nullptr;
  }
  const auto native_error = [&]() -> const char * {
    auto *mgr = Manager::instance();
    if (!mgr || !mgr->resource_group_support()) return "Rapid native CPU resource groups are unavailable";
    std::unique_lock lock(cpu_mutex);
    // Register raw std::threads as SYSTEM threads, without changing user THDs.
#ifdef HAVE_PSI_THREAD_INTERFACE
    NotificationGuard notification_guard;
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
    if (attrs.m_groupname_length > sizeof(attrs.m_groupname)) return "Rapid native resource group name is invalid";
#ifdef __linux__
    if (mode == IsolationMode::AUTO && !default_group(attrs)) {
      const std::string assigned(attrs.m_groupname, attrs.m_groupname_length);
      const bool internal = rapid_threads.count(attrs.m_thread_os_id) &&
                            std::any_of(std::begin(auto_names), std::end(auto_names),
                                        [&](const char *value) { return assigned == value; });
      if (!internal) {
        degrade_auto("DBA resource group assignment present; software admission retained");
        return nullptr;
      }
    }
#endif
    const auto expected =
        attrs.m_system_thread ? resourcegroups::Type::SYSTEM_RESOURCE_GROUP : resourcegroups::Type::USER_RESOURCE_GROUP;
    if (kind != WorkClass::MAINT && attrs.m_system_thread != (kind != WorkClass::AP))
      return "Rapid native resource group thread type mismatch";
    if (kind == WorkClass::MAINT && !attrs.m_system_thread) name = maint_user_name();
    if (!name || !*name) {
      if (mode == IsolationMode::STRICT_CPU) return "Rapid STRICT_CPU resource group is not configured";
      return nullptr;
    }
    auto *destination = state->pins.get(name);
    if (!destination || !destination->enabled() || destination->type() != expected)
      return "Rapid configured resource group is missing, disabled or has the wrong type";
#ifdef __linux__
    state->tid = syscall(SYS_gettid);
    ++rapid_threads[state->tid];
    ++native_scopes[state->tid];
#endif
    if (mode == IsolationMode::STRICT_CPU || mode == IsolationMode::AUTO) {
      if (const char *error = check_plan(state->pins, lock, 20, mode == IsolationMode::AUTO)) {
        if (mode != IsolationMode::AUTO) return error;
#ifdef __linux__
        degrade_auto(error);
#endif
        return nullptr;
      }
    }
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
    if (mode == IsolationMode::STRICT_CPU || mode == IsolationMode::AUTO) {
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
    return nullptr;
  }();
  if (native_error) {
    if (mode != IsolationMode::AUTO) return native_error;
    auto lease = std::move(state->lease);
    state.reset();
    if (thd->killed) return native_error;
    {
      std::lock_guard lock(cpu_mutex);
#ifdef __linux__
      degrade_auto(native_error);
#else
      auto_native.store(false);
      auto_reason = native_error;
#endif
    }
    state = std::make_unique<State>(thd);
    state->lease = std::move(lease);
  }
  m_state = std::move(state);
  return nullptr;
}
}  // namespace ShannonBase::ResMgmt
