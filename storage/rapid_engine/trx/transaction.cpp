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

   The fundmental code for imcs. for transaction.
   Now that, we use innodb trx as rapid's. But, in future, we will impl
   our own trx implementation, because we use innodb trx id in rapid
   for our visibility check.
*/
#include "storage/rapid_engine/trx/transaction.h"

#include "sql/sql_class.h"  // THD

#include "storage/innobase/include/read0types.h"  //ReadView
#include "storage/innobase/include/trx0roll.h"    // rollback
#include "storage/innobase/include/trx0trx.h"     // trx_t

#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/include/rapid_context.h"

namespace ShannonBase {
// defined in ha_shannon_rapid.cc
extern handlerton *shannon_rapid_hton_ptr;

static ShannonBase::Rapid_ha_data *&get_ha_data_or_null(THD *const thd) {
  ShannonBase::Rapid_ha_data **ha_data =
      reinterpret_cast<ShannonBase::Rapid_ha_data **>(thd_ha_data(thd, ShannonBase::shannon_rapid_hton_ptr));
  return *ha_data;
}

static ShannonBase::Rapid_ha_data *&get_ha_data(THD *const thd) {
  auto *&ha_data = get_ha_data_or_null(thd);
  if (ha_data == nullptr) {
    ha_data = new ShannonBase::Rapid_ha_data();
  }
  return ha_data;
}

static void destroy_ha_data(THD *const thd) {
  ShannonBase::Rapid_ha_data *&ha_data = get_ha_data(thd);
  delete ha_data;
  ha_data = nullptr;
}

Transaction::Transaction(THD *thd) : m_thd(thd) {
  m_trx_impl = trx_allocate_for_mysql();
  m_trx_impl->mysql_thd = thd;
  m_trx_impl->auto_commit = (m_thd != nullptr && !thd_test_options(m_thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN));
}

Transaction::~Transaction() {
  release_snapshot();

  if (trx_is_started(m_trx_impl)) trx_rollback_for_mysql(m_trx_impl);

  m_registered_in_coordinator = false;
  m_start_scn = 0;
  m_commit_scn = 0;
  m_stmt_active = false;

  trx_free_for_mysql(m_trx_impl);
}

Transaction::ID Transaction::get_id() { return m_trx_impl->id; }

bool Transaction::is_active() { return trx_is_started(m_trx_impl); }

bool Transaction::is_auto_commit() { return m_trx_impl->auto_commit; }

bool Transaction::has_snapshot() const { return MVCC::is_view_active(m_trx_impl->read_view); }

::ReadView *Transaction::get_snapshot() const { return m_trx_impl->read_view; }

void Transaction::set_trx_on_thd(THD *const thd) { get_ha_data(thd)->set_trx(this); }

void Transaction::reset_trx_on_thd(THD *const thd) {
  get_ha_data(thd)->set_trx(nullptr);
  destroy_ha_data(thd);
}

Transaction *Transaction::get_trx_from_thd(THD *const thd) { return get_ha_data(thd)->get_trx(); }

Transaction *Transaction::get_or_create_trx(THD *thd) {
  auto *trx = Transaction::get_trx_from_thd(thd);
  if (trx == nullptr) {
    trx = new Transaction(thd);
    trx->set_trx_on_thd(thd);
  }
  return trx;
}

void Transaction::free_trx_from_thd(THD *const thd) {
  auto *trx = Transaction::get_trx_from_thd(thd);
  if (trx) {
    trx->reset_trx_on_thd(thd);
    delete trx;
  }
}

Transaction::ISOLATION_LEVEL Transaction::get_rpd_isolation_level(THD *thd) {
  ulong const tx_isolation = thd_tx_isolation(thd);

  if (tx_isolation == ISO_READ_UNCOMMITTED) {
    return ISOLATION_LEVEL::READ_UNCOMMITTED;
  } else if (tx_isolation == ISO_READ_COMMITTED) {
    return ISOLATION_LEVEL::READ_COMMITTED;
  } else if (tx_isolation == ISO_REPEATABLE_READ) {
    return ISOLATION_LEVEL::READ_REPEATABLE;
  } else {
    return ISOLATION_LEVEL::SERIALIZABLE;
  }
}

int Transaction::begin(ISOLATION_LEVEL iso_level) {
  m_iso_level = iso_level;

  trx_t::isolation_level_t is = trx_t::isolation_level_t::REPEATABLE_READ;
  switch (iso_level) {
    case ISOLATION_LEVEL::READ_UNCOMMITTED:
      is = trx_t::isolation_level_t::READ_UNCOMMITTED;
      break;
    case ISOLATION_LEVEL::READ_COMMITTED:
      is = trx_t::isolation_level_t::READ_COMMITTED;
      break;
    case ISOLATION_LEVEL::READ_REPEATABLE:
      is = trx_t::isolation_level_t::REPEATABLE_READ;
      break;
    case ISOLATION_LEVEL::SERIALIZABLE:
      is = trx_t::isolation_level_t::SERIALIZABLE;
      break;
  }
  switch (thd_sql_command(m_thd)) {
    case SQLCOM_INSERT:
    case SQLCOM_UPDATE:
    case SQLCOM_DELETE:
    case SQLCOM_REPLACE:
      m_read_only = false;
      break;
  }

  ut_a(m_trx_impl);
  // Check: If the transaction is registered but the underlying transaction has ended, it needs to be unregistered
  // first.
  if (m_registered_in_coordinator && m_trx_impl->state.load() == TRX_STATE_NOT_STARTED) {
    // This situation indicates that the state was not properly reset after the last commit/rollback, so the old
    // registration should be proactively unregistered.
    TransactionCoordinator::instance().unregister_transaction(this);
    m_registered_in_coordinator = false;
    m_start_scn = 0;
    m_commit_scn = 0;
  }

  m_trx_impl->isolation_level = is;

  trx_start_if_not_started(m_trx_impl, !m_read_only, UT_LOCATION_HERE);

  if (!m_registered_in_coordinator) {
    m_start_scn = TransactionCoordinator::instance().register_transaction(this, iso_level);
    m_registered_in_coordinator = true;
  }
  return SHANNON_SUCCESS;
}

int Transaction::begin_stmt(ISOLATION_LEVEL iso_level) {
  if (m_stmt_active) rollback_stmt();

  if (!is_active()) {
    int ret = begin(iso_level);
    if (ret != SHANNON_SUCCESS) return ret;
  }

  ut_a(m_trx_impl);
  ut_a(trx_is_started(m_trx_impl));

  m_stmt_active = true;
  m_trx_impl->op_info = "statement";

  trx_savept_t stmt_savepoint = trx_savept_take(m_trx_impl);
  m_trx_impl->last_sql_stat_start.least_undo_no = stmt_savepoint.least_undo_no;
  return SHANNON_SUCCESS;
}

int Transaction::commit() {
  dberr_t error = DB_SUCCESS;

  if (trx_is_started(m_trx_impl)) {
    if (m_registered_in_coordinator) {
      TransactionCoordinator::instance().commit_transaction(this);
      // reset status flags，alllow this transaction obj can be reused.
      m_registered_in_coordinator = false;
      m_start_scn = 0;
      m_commit_scn = 0;
    }
    error = trx_commit_for_mysql(m_trx_impl);
  }

  m_stmt_active = false;
  return (error != DB_SUCCESS) ? HA_ERR_GENERIC : SHANNON_SUCCESS;
}

int Transaction::rollback() {
  dberr_t error = DB_SUCCESS;

  if (trx_is_started(m_trx_impl)) {
    if (m_registered_in_coordinator) {
      TransactionCoordinator::instance().rollback_transaction(this);

      m_registered_in_coordinator = false;
      m_start_scn = 0;
      m_commit_scn = 0;
    }
    error = trx_rollback_for_mysql(m_trx_impl);
    m_stmt_active = false;
  }
  return (error != DB_SUCCESS) ? HA_ERR_GENERIC : SHANNON_SUCCESS;
}

int Transaction::rollback_stmt() {
  if (!m_stmt_active) return SHANNON_SUCCESS;

  dberr_t error = DB_SUCCESS;

  if (m_trx_impl && trx_is_started(m_trx_impl)) {
    error = trx_rollback_to_savepoint(m_trx_impl, nullptr);
    m_trx_impl->op_info = "";
  }

  m_stmt_active = false;
  return (error != DB_SUCCESS) ? HA_ERR_GENERIC : SHANNON_SUCCESS;
}

::ReadView *Transaction::acquire_snapshot() {
  if (!MVCC::is_view_active(m_trx_impl->read_view) && (m_trx_impl->isolation_level > TRX_ISO_READ_UNCOMMITTED))
    trx_assign_read_view(m_trx_impl);
  return m_trx_impl->read_view;
}

int Transaction::release_snapshot() {
  if (trx_sys->mvcc && m_trx_impl->read_view && MVCC::is_view_active(m_trx_impl->read_view))
    trx_sys->mvcc->view_close(m_trx_impl->read_view, false);
  return SHANNON_SUCCESS;
}

bool Transaction::changes_visible(Transaction::ID trx_id, const char *table_name) {
  if (MVCC::is_view_active(m_trx_impl->read_view)) {
    table_name_t name;
    name.m_name = const_cast<char *>(table_name);
    return m_trx_impl->read_view->changes_visible(trx_id, name);
  }
  return false;
}

void Transaction::register_imcu_modification(std::shared_ptr<ShannonBase::Imcs::Imcu> imcu) {
  if (m_registered_in_coordinator) TransactionCoordinator::instance().register_imcu_modification(get_id(), imcu);
}

uint64_t TransactionCoordinator::register_transaction(Transaction *trx, Transaction::ISOLATION_LEVEL iso_level) {
  assert(trx != nullptr);

  Transaction::ID txn_id = trx->get_id();
  uint64_t start_scn = Transaction::VersionManager::instance().get_current_scn();

  TransactionInfo info;
  info.txn_id = txn_id;
  info.trx = trx;
  info.start_scn = start_scn;
  info.start_time = std::chrono::system_clock::now();
  info.status = TransactionInfo::ACTIVE;
  {
    std::unique_lock lock(m_txns_mutex);
    m_active_txns[txn_id] = std::move(info);
    update_min_active_scn();
  }
  return start_scn;
}

bool TransactionCoordinator::commit_transaction(Transaction *trx) {
  assert(trx != nullptr);
  uint64_t commit_scn = Transaction::VersionManager::instance().allocate_scn();
  return commit_transaction_internal(trx, commit_scn);
}

bool TransactionCoordinator::commit_transaction_internal(Transaction *trx, uint64_t commit_scn) {
  assert(trx != nullptr);

  Transaction::ID txn_id = trx->get_id();
  TransactionInfo info;
  {
    std::unique_lock lock(m_txns_mutex);
    auto it = m_active_txns.find(txn_id);
    if (it == m_active_txns.end()) return false;

    uint64_t commit_scn = Transaction::VersionManager::instance().allocate_scn();
    it->second.commit_scn = commit_scn;
    it->second.status = TransactionInfo::COMMITTED;
    trx->m_commit_scn = commit_scn;
    info = std::move(it->second);
    m_active_txns.erase(it);
    update_min_active_scn();
  }

  for (auto *imcu : info.modified_imcus) {
    auto txn_jr = imcu->get_transaction_journal();
    if (txn_jr) txn_jr->commit_transaction(txn_id, info.commit_scn);
  }

  for (auto &imcu : imcus_to_commit) {
    if (imcu) invalidate_visibility_cache(imcu.get());
  }
  return true;
}

bool TransactionCoordinator::rollback_transaction(Transaction *trx) {
  assert(trx != nullptr);
  Transaction::ID txn_id = trx->get_id();

  std::vector<std::shared_ptr<ShannonBase::Imcs::Imcu>> imcus_to_rollback;
  {
    std::unique_lock<std::shared_mutex> lock(m_txns_mutex);
    auto it = m_active_txns.find(txn_id);
    if (it == m_active_txns.end()) return false;

    imcus_to_rollback = it->second.modified_imcus;
    m_active_txns.erase(it);
    update_min_active_scn();
    m_total_aborted.fetch_add(1, std::memory_order_relaxed);
  }

  for (auto *imcu : modified_imcus) {
    if (auto *journal = imcu->get_transaction_journal()) journal->abort_transaction(txn_id);
  }

  for (auto &imcu : imcus_to_rollback) {
    if (imcu) invalidate_visibility_cache(imcu.get());
  }
  return true;
}

void TransactionCoordinator::unregister_transaction(Transaction *trx) {
  assert(trx != nullptr);

  Transaction::ID txn_id = trx->get_id();
  std::unique_lock lock(m_txns_mutex);
  // Transaction still in active list indicates improper commit/rollback, cleanup required
  auto it = m_active_txns.find(txn_id);
  if (it != m_active_txns.end()) {
    // Notify all IMCUs txn aborted
    for (auto *imcu : it->second.modified_imcus) {
      if (imcu->get_transaction_journal()) imcu->get_transaction_journal()->abort_transaction(txn_id);
    }
    m_active_txns.erase(it);
    update_min_active_scn();
    m_total_aborted.fetch_add(1, std::memory_order_relaxed);
  }
}

void TransactionCoordinator::register_imcu_modification(Transaction::ID txn_id,
                                                        std::shared_ptr<ShannonBase::Imcs::Imcu> imcu) {
  std::unique_lock lock(m_txns_mutex);

  auto it = m_active_txns.find(txn_id);
  if (it != m_active_txns.end()) it->second.modified_imcus.push_back(imcu);
}

Transaction::VersionManager::Snapshot TransactionCoordinator::create_snapshot() {
  std::shared_lock lock(m_txns_mutex);
  std::vector<Transaction::ID> active_txns;
  active_txns.reserve(m_active_txns.size());

  for (const auto &[txn_id, info] : m_active_txns) {
    if (info.status == TransactionInfo::ACTIVE) active_txns.push_back(txn_id);
  }

  uint64_t current_scn = Transaction::VersionManager::instance().get_current_scn();
  // Try snapshot cache
  if (auto cached = get_cached_snapshot(current_scn, active_txns)) {
    m_snapshot_cache_hits.fetch_add(1, std::memory_order_relaxed);
    return *cached;
  }

  m_snapshot_cache_misses.fetch_add(1, std::memory_order_relaxed);

  auto snapshot = Transaction::VersionManager::instance().create_snapshot(active_txns);
  cache_snapshot(snapshot);
  return snapshot;
}

const bit_array_t *TransactionCoordinator::get_cached_visibility(void *imcu, uint64_t scn) {
  std::shared_lock lock(m_visibility_cache_mutex);
  VisibilityCacheKey key{imcu, scn};
  auto it = m_visibility_cache.find(key);

  if (it != m_visibility_cache.end()) {
    it->second.access_count.fetch_add(1, std::memory_order_relaxed);
    m_visibility_cache_hits.fetch_add(1, std::memory_order_relaxed);
    return it->second.bitmap.get();
  }

  m_visibility_cache_misses.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

void TransactionCoordinator::cache_visibility(void *imcu, uint64_t scn, std::unique_ptr<bit_array_t> bitmap) {
  std::unique_lock lock(m_visibility_cache_mutex);
  if (m_visibility_cache.size() >= m_max_visibility_cache_entries) evict_visibility_cache_lfu();

  VisibilityCacheKey key{imcu, scn};
  CachedVisibility entry;
  entry.bitmap = std::move(bitmap);
  entry.created_at = std::chrono::steady_clock::now();
  entry.access_count = 0;
  m_visibility_cache[key] = std::move(entry);
}

void TransactionCoordinator::invalidate_visibility_cache(void *imcu) {
  std::unique_lock lock(m_visibility_cache_mutex);
  for (auto it = m_visibility_cache.begin(); it != m_visibility_cache.end();) {
    if (it->first.imcu_ptr == imcu) {
      it = m_visibility_cache.erase(it);
    } else {
      ++it;
    }
  }
}

std::future<uint64_t> TransactionCoordinator::commit_transaction_async(Transaction *trx) {
  std::unique_lock lock(m_batch_commit_mutex);
  BatchCommitRequest req;
  req.trx = trx;
  auto future = req.commit_scn_promise.get_future();

  m_pending_commits.push_back(std::move(req));
  if (m_pending_commits.size() >= m_batch_commit_size) {
    m_batch_commit_cv.notify_one();
  }
  return future;
}

std::optional<TransactionCoordinator::TransactionInfo> TransactionCoordinator::get_transaction_info(
    Transaction::ID txn_id) const {
  std::shared_lock lock(m_txns_mutex);
  auto it = m_active_txns.find(txn_id);
  if (it != m_active_txns.end()) return it->second;
  return std::nullopt;
}

std::vector<TransactionCoordinator::TransactionInfo> TransactionCoordinator::get_active_transactions() const {
  std::shared_lock lock(m_txns_mutex);
  std::vector<TransactionInfo> result;
  result.reserve(m_active_txns.size());
  for (const auto &[txn_id, info] : m_active_txns) {
    result.push_back(info);
  }
  return result;
}

bool TransactionCoordinator::is_transaction_active(Transaction::ID txn_id) const {
  std::shared_lock lock(m_txns_mutex);
  return m_active_txns.find(txn_id) != m_active_txns.end();
}

std::optional<Transaction::VersionManager::Snapshot> TransactionCoordinator::get_cached_snapshot(
    uint64_t scn, const std::vector<Transaction::ID> &active_txns) {
  std::shared_lock lock(m_snapshot_cache_mutex);

  for (const auto &cached : m_snapshot_cache) {
    if (cached.scn == scn && cached.active_txns == active_txns) {
      return cached;
    }
  }
  return std::nullopt;
}

void TransactionCoordinator::cache_snapshot(const Transaction::VersionManager::Snapshot &snapshot) {
  std::unique_lock lock(m_snapshot_cache_mutex);

  if (m_snapshot_cache.size() >= m_max_snapshot_cache_size) {
    m_snapshot_cache.erase(m_snapshot_cache.begin());
  }
  m_snapshot_cache.push_back(snapshot);
}

void TransactionCoordinator::evict_visibility_cache_lfu() {
  auto to_remove = std::min_element(
      m_visibility_cache.begin(), m_visibility_cache.end(),
      [](const auto &a, const auto &b) { return a.second.access_count.load() < b.second.access_count.load(); });

  if (to_remove != m_visibility_cache.end()) {
    m_visibility_cache.erase(to_remove);
  }
}

void TransactionCoordinator::batch_commit_worker_loop() {
  std::vector<BatchCommitRequest> batch;
  while (m_batch_running.load()) {
    {
      std::unique_lock lock(m_batch_commit_mutex);
      m_batch_commit_cv.wait_for(lock, std::chrono::milliseconds(1), [this] {
        return m_pending_commits.size() >= m_batch_commit_size || !m_batch_running.load();
      });
      if (!m_pending_commits.empty()) {
        batch = std::move(m_pending_commits);
        m_pending_commits.clear();
      }
    }
    if (!batch.empty()) process_batch_commits(batch);
    batch.clear();
  }
}

void TransactionCoordinator::process_batch_commits(std::vector<BatchCommitRequest> &batch) {
  uint64_t base_scn = Transaction::VersionManager::instance().allocate_scn_batch(batch.size());

  for (size_t i = 0; i < batch.size(); ++i) {
    auto &req = batch[i];
    uint64_t commit_scn = base_scn + i;

    // Use internal method with pre-allocated SCN to avoid double SCN allocation
    commit_transaction_internal(req.trx, commit_scn);
    req.commit_scn_promise.set_value(commit_scn);
  }
}

void TransactionCoordinator::update_min_active_scn() {
  std::unordered_map<Transaction::ID, uint64_t> active_scns;
  for (const auto &[tid, info] : m_active_txns) {
    active_scns[tid] = info.start_scn;
  }
  Transaction::VersionManager::instance().update_min_active_scn(active_scns);
}

void TransactionCoordinator::dump_active_transactions(std::ostream &out) const {
  std::shared_lock lock(m_txns_mutex);

  out << "Active Transactions: " << m_active_txns.size() << "\n";
  out << "Current SCN: " << get_current_scn() << "\n";
  out << "Min Active SCN: " << get_min_active_scn() << "\n\n";

  for (const auto &[txn_id, info] : m_active_txns) {
    out << "  TXN " << txn_id << ":\n";
    out << "    Start SCN: " << info.start_scn << "\n";
    out << "    Status: " << static_cast<int>(info.status) << "\n";
    out << "    Modified IMCUs: " << info.modified_imcus.size() << "\n";

    auto duration = std::chrono::system_clock::now() - info.start_time;
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    out << "    Duration: " << seconds << " seconds\n\n";
  }
}

TransactionCoordinator::Statistics TransactionCoordinator::get_statistics() const {
  std::shared_lock lock(m_txns_mutex);

  Statistics stats;
  stats.active_count = m_active_txns.size();
  stats.total_committed = m_total_committed.load();
  stats.total_aborted = m_total_aborted.load();
  stats.current_scn = get_current_scn();
  stats.min_active_scn = get_min_active_scn();
  stats.snapshot_cache_hits = m_snapshot_cache_hits.load();
  stats.snapshot_cache_misses = m_snapshot_cache_misses.load();
  stats.visibility_cache_hits = m_visibility_cache_hits.load();
  stats.visibility_cache_misses = m_visibility_cache_misses.load();

  return stats;
}

void TransactionJournal::add_entry(Entry &&entry) {
  std::unique_lock lock(m_mutex);
  row_id_t row_id = entry.row_id;
  Transaction::ID txn_id = entry.txn_id;
  // Create new entry
  auto new_entry = std::make_unique<Entry>(std::move(entry));
  // Link to version chain
  auto it = m_entries.find(row_id);
  if (it != m_entries.end()) {
    new_entry->prev = it->second.release();
    it->second = std::move(new_entry);
  } else {
    m_entries[row_id] = std::move(new_entry);
  }

  // Add to transaction index
  m_txn_entries[txn_id].push_back(m_entries[row_id].get());

  // Mark transaction as active
  m_active_txns.insert(txn_id);

  m_entry_count.fetch_add(1);
  m_total_size.fetch_add(sizeof(Entry));
}

void TransactionJournal::commit_transaction(Transaction::ID txn_id, uint64_t commit_scn) {
  std::unique_lock lock(m_mutex);
  auto it = m_txn_entries.find(txn_id);
  if (it == m_txn_entries.end()) return;

  // Update status and SCN for all entries
  for (Entry *entry : it->second) {
    if (!entry) continue;
    entry->scn = commit_scn;
    entry->status = COMMITTED;
  }

  // Remove from active transaction set
  m_active_txns.erase(txn_id);
}

void TransactionJournal::abort_transaction(Transaction::ID txn_id) {
  std::unique_lock lock(m_mutex);
  auto it = m_txn_entries.find(txn_id);
  if (it == m_txn_entries.end()) return;

  // Mark all entries as aborted
  for (Entry *entry : it->second) {
    if (!entry) continue;
    entry->status = ABORTED;
  }

  // Remove from active transaction set
  m_active_txns.erase(txn_id);

  // Clean up index
  m_txn_entries.erase(it);
}

bool TransactionJournal::is_visible(row_id_t row_id, Transaction::ID reader_txn_id, uint64_t reader_scn) const {
  std::shared_lock lock(m_mutex);
  auto it = m_entries.find(row_id);
  // No history record, indicates initial data, visible
  if (it == m_entries.end()) return true;

  Entry *entry = it->second.get();
  // Traverse version chain (from new to old)
  while (entry != nullptr) {
    // 1. If it's the reader's own transaction, visible
    if (entry->txn_id == reader_txn_id) {
      return static_cast<ShannonBase::OPER_TYPE>(entry->operation) != ShannonBase::OPER_TYPE::OPER_DELETE &&
             entry->status != ABORTED;
    }

    // 2. If transaction not committed, not visible
    if (entry->status == ACTIVE) {
      entry = entry->prev;
      continue;
    }

    // 3. If transaction aborted, not visible
    if (entry->status == ABORTED) {
      entry = entry->prev;
      continue;
    }

    // 4. If transaction committed
    if (entry->status == COMMITTED) {
      // 4.1 If commit SCN is after reader snapshot, not visible
      if (entry->scn > reader_scn) {
        entry = entry->prev;
        continue;
      }

      // 4.2 If commit SCN is before reader snapshot, Check operation type
      if (static_cast<ShannonBase::OPER_TYPE>(entry->operation) == ShannonBase::OPER_TYPE::OPER_INSERT) {
        return true;  // Insert visible
      } else if (static_cast<ShannonBase::OPER_TYPE>(entry->operation) == ShannonBase::OPER_TYPE::OPER_DELETE) {
        return false;  // Delete not visible
      } else if (static_cast<ShannonBase::OPER_TYPE>(entry->operation) == ShannonBase::OPER_TYPE::OPER_UPDATE) {
        return true;  // Update visible
      }
    }
    entry = entry->prev;
  }
  // No visible version found
  return false;
}

void TransactionJournal::check_visibility_batch(row_id_t start_row, size_t count, Transaction::ID reader_txn_id,
                                                uint64_t reader_scn, bit_array_t &visibility_mask) const {
  std::shared_lock lock(m_mutex);
  for (size_t i = 0; i < count; i++) {
    row_id_t row_id = start_row + i;
    bool visible = is_visible(row_id, reader_txn_id, reader_scn);
    (visible) ? Utils::Util::bit_array_set(&visibility_mask, i) : Utils::Util::bit_array_reset(&visibility_mask, i);
  }
}

ShannonBase::OPER_TYPE TransactionJournal::get_row_state_at_scn(
    row_id_t row_id, uint64_t target_scn, std::bitset<SHANNON_MAX_COLUMNS> *modified_columns) const {
  std::shared_lock lock(m_mutex);
  auto it = m_entries.find(row_id);
  if (it == m_entries.end()) return ShannonBase::OPER_TYPE::OPER_NONE;

  Entry *entry = it->second.get();
  while (entry != nullptr) {
    if (entry->status == COMMITTED && entry->scn <= target_scn) {
      if (modified_columns &&
          static_cast<ShannonBase::OPER_TYPE>(entry->operation) == ShannonBase::OPER_TYPE::OPER_UPDATE) {
        *modified_columns = entry->modified_columns;
      }
      return static_cast<ShannonBase::OPER_TYPE>(entry->operation);
    }
    entry = entry->prev;
  }
  return ShannonBase::OPER_TYPE::OPER_NONE;
}

size_t TransactionJournal::purge(uint64_t min_active_scn) {
  std::unique_lock lock(m_mutex);
  size_t purged = 0;
  for (auto it = m_entries.begin(); it != m_entries.end();) {
    Entry *head = it->second.get();
    Entry *current = head;
    Entry *prev_valid = nullptr;

    // Keep the latest visible version
    bool found_visible = false;
    while (current != nullptr) {
      // If version is before minimum active SCN, and not the latest visible version
      if (current->status == COMMITTED && current->scn < min_active_scn && found_visible) {
        // Can clean up
        Entry *to_delete = current;
        current = current->prev;
        if (prev_valid) prev_valid->prev = current;
        delete to_delete;
        purged++;
        m_entry_count.fetch_sub(1);
        m_total_size.fetch_sub(sizeof(Entry));
      } else {
        // Keep
        if (current->status == COMMITTED) {
          found_visible = true;
          prev_valid = current;
        }
        current = current->prev;
      }
    }

    // If entire version chain is cleaned
    if (head == nullptr || (head->prev == nullptr && head->status == ABORTED)) {
      it = m_entries.erase(it);
    } else {
      ++it;
    }
  }
  return purged;
}

size_t TransactionJournal::purge_aborted() {
  std::unique_lock lock(m_mutex);
  size_t purged = 0;
  for (auto it = m_entries.begin(); it != m_entries.end();) {
    Entry *head = it->second.get();
    if (head->status == ABORTED && head->prev == nullptr) {
      // Only one aborted version, can delete
      it = m_entries.erase(it);
      purged++;
      m_entry_count.fetch_sub(1);
      m_total_size.fetch_sub(sizeof(Entry));
    } else {
      ++it;
    }
  }
  return purged;
}

void TransactionJournal::dump(std::ostream &out) const {
  std::shared_lock lock(m_mutex);
  out << "Transaction Journal: " << m_entry_count.load() << " entries\n";
  for (const auto &[row_id, entry] : m_entries) {
    Entry *current = entry.get();
    out << "  Row " << row_id << ": ";
    while (current != nullptr) {
      out << "[txn=" << current->txn_id << " scn=" << current->scn << " op=" << static_cast<int>(current->operation)
          << " status=" << static_cast<int>(current->status) << "] -> ";
      current = current->prev;
    }
    out << "NULL\n";
  }
}
}  // namespace ShannonBase