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

#include "sql/mysqld.h"  // innodb_hton
#include "sql/sql_class.h"
#include "storage/innobase/handler/ha_innodb.h"      // innobase_register_trx, isolation mapping
#include "storage/innobase/include/ha_prototypes.h"  // check_trx_exists
#include "storage/rapid_engine/imcs/imcu.h"

namespace ShannonBase {
// defined in ha_shannon_rapid.cc
extern handlerton *shannon_rapid_hton_ptr;

namespace {
/**
  Register the exact THD-owned InnoDB transaction with MySQL/InnoDB.

  Rapid may be the only executor touching user data for a statement, so simply
  borrowing thd_to_trx()/check_trx_exists() is not enough: InnoDB still has to
  receive its normal statement/transaction callbacks in order to own ReadView
  rotation (READ COMMITTED) and transaction finalization (COMMIT/ROLLBACK).
  innobase_register_trx() is explicitly idempotent for the same transaction.
*/
bool register_innodb_trx(THD *thd, trx_t *trx) {
  if (thd == nullptr || trx == nullptr || innodb_hton == nullptr) return false;
  innobase_register_trx(innodb_hton, thd, trx);
  return true;
}

Transaction::ISOLATION_LEVEL isolation_level_from_thd(THD *thd) {
  switch (thd_tx_isolation(thd)) {
    case ISO_READ_UNCOMMITTED:
      return Transaction::ISOLATION_LEVEL::READ_UNCOMMITTED;
    case ISO_READ_COMMITTED:
      return Transaction::ISOLATION_LEVEL::READ_COMMITTED;
    case ISO_REPEATABLE_READ:
      return Transaction::ISOLATION_LEVEL::READ_REPEATABLE;
    case ISO_SERIALIZABLE:
    default:
      return Transaction::ISOLATION_LEVEL::SERIALIZABLE;
  }
}

std::mutex g_transaction_subscribers_mutex;
std::vector<TransactionSubscriber *> g_transaction_subscribers;

template <typename Callback>
void notify_transaction_subscribers(Callback &&callback) {
  // Keep the registry lock while invoking callbacks. This gives unsubscribe()
  // a strict lifetime boundary: once it returns, no callback can still hold
  // the subscriber's raw pointer.
  std::lock_guard<std::mutex> lock(g_transaction_subscribers_mutex);
  for (auto *subscriber : g_transaction_subscribers) {
    if (subscriber != nullptr) callback(*subscriber);
  }
}
}  // namespace

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

Transaction *Transaction::find_trx(THD *const thd) {
  if (thd == nullptr) return nullptr;
  auto *ha_data = get_ha_data_or_null(thd);
  return ha_data != nullptr ? ha_data->get_trx() : nullptr;
}

Transaction *Transaction::get_or_create_trx(THD *thd) {
  if (thd == nullptr) return nullptr;

  auto *trx = find_trx(thd);
  if (trx != nullptr) return trx;

  trx = new Transaction(thd);
  if (trx->m_primary_trx == nullptr) {
    delete trx;
    return nullptr;
  }

  get_ha_data(thd)->set_trx(trx);
  return trx;
}

void Transaction::free_trx_from_thd(THD *const thd) {
  if (thd == nullptr) return;

  auto *&ha_data = get_ha_data_or_null(thd);
  if (ha_data == nullptr) return;

  Transaction *trx = ha_data->get_trx();
  ha_data->set_trx(nullptr);
  delete trx;
  delete ha_data;
  ha_data = nullptr;
}

void Transaction::subscribe(TransactionSubscriber *subscriber) {
  if (subscriber == nullptr) return;

  std::lock_guard<std::mutex> lock(g_transaction_subscribers_mutex);
  if (std::find(g_transaction_subscribers.begin(), g_transaction_subscribers.end(), subscriber) ==
      g_transaction_subscribers.end()) {
    g_transaction_subscribers.push_back(subscriber);
  }
}

void Transaction::unsubscribe(TransactionSubscriber *subscriber) {
  if (subscriber == nullptr) return;

  std::lock_guard<std::mutex> lock(g_transaction_subscribers_mutex);
  g_transaction_subscribers.erase(
      std::remove(g_transaction_subscribers.begin(), g_transaction_subscribers.end(), subscriber),
      g_transaction_subscribers.end());
}

Transaction::Transaction(THD *thd) : m_thd(thd) {
  // check_trx_exists() creates the THD's *InnoDB-owned* transaction object if
  // needed and returns that same primary transaction. Rapid merely keeps a
  // borrowed pointer; there is no second trx_allocate_for_mysql() universe.
  if (m_thd != nullptr) {
    m_primary_trx = check_trx_exists(m_thd);
    m_iso_level = isolation_level_from_thd(m_thd);
  }
}

Transaction::~Transaction() {
  release_snapshot();

  // A still-active Rapid physical transaction means Rapid-local versions were
  // left unfinished. Clean those up, but never touch the borrowed InnoDB trx.
  if (m_coord_state == CoordState::ACTIVE) {
    TransactionCoordinator::instance().unregister_transaction(this);
    m_coord_state = CoordState::UNREGISTERED;
  }

  m_start_scn = 0;
  m_commit_scn = 0;
  m_physical_txn_id.store(0, std::memory_order_release);

  if (m_thd != nullptr) {
    notify_transaction_subscribers(
        [this](TransactionSubscriber &subscriber) { subscriber.on_transaction_detach(m_thd); });
  }

  m_stmt_active = false;
  m_primary_trx = nullptr;
}

void Transaction::sync_coordinator_state(CoordState intent) {
  // Coordinator state belongs to Rapid physical MVCC and must not be inferred
  // from the lifecycle/state of the borrowed primary InnoDB transaction.
  switch (intent) {
    case CoordState::ACTIVE:
      if (m_coord_state == CoordState::UNREGISTERED && m_physical_txn_id.load(std::memory_order_acquire) != 0) {
        m_start_scn = TransactionCoordinator::instance().register_transaction(this, m_iso_level);
        m_coord_state = CoordState::ACTIVE;
      }
      break;
    case CoordState::UNREGISTERED:
    case CoordState::FINALIZING:
      break;
  }
}

int Transaction::begin() {
  if (m_thd == nullptr) return HA_ERR_GENERIC;

  // The facade can survive multiple SQL transactions on the same THD. Refresh
  // both pieces of primary SQL state at each begin instead of accepting a
  // caller-supplied isolation value that can drift from the THD.
  m_primary_trx = check_trx_exists(m_thd);
  if (m_primary_trx == nullptr) return HA_ERR_GENERIC;
  m_iso_level = isolation_level_from_thd(m_thd);

  // Even when the statement is executed entirely by Rapid, register the exact
  // primary transaction with InnoDB/MySQL.  This keeps statement-end ReadView
  // rotation and transaction-end commit/rollback under InnoDB ownership.
  if (!register_innodb_trx(m_thd, m_primary_trx)) return HA_ERR_GENERIC;

  // If Rapid is the first engine that needs a transaction for this statement,
  // initialize/start the *primary* InnoDB transaction as a read-only snapshot
  // owner.  Do not create a second transaction and do not change isolation on
  // an already-started transaction.
  if (!trx_is_started(m_primary_trx)) {
    m_primary_trx->isolation_level =
        innobase_trx_map_isolation_level(static_cast<enum_tx_isolation>(thd_tx_isolation(m_thd)));
    trx_start_if_not_started(m_primary_trx, false, UT_LOCATION_HERE);
  }

  // SQL reads do not participate in Rapid's physical write coordinator. Keep
  // an existing physical transaction classification/start SCN intact if a
  // legacy internal mutation path has registered one.
  if (m_coord_state == CoordState::UNREGISTERED) {
    m_read_only = true;
    m_start_scn = TransactionCoordinator::instance().get_current_scn();
  }
  return SHANNON_SUCCESS;
}

int Transaction::begin_stmt() {
  if (m_stmt_active) rollback_stmt();

  // Re-register on every SQL statement. InnoDB explicitly permits registering
  // the same transaction repeatedly and uses this to track statement borders.
  const int ret = begin();
  if (ret != SHANNON_SUCCESS) return ret;

  // InnoDB/server owns statement savepoints and undo. Rapid's statement flag
  // only tracks its own callback boundary.
  m_stmt_active = true;
  return SHANNON_SUCCESS;
}

int Transaction::commit() {
  const int ret = commit_internal();
  if (ret != SHANNON_SUCCESS) return ret;

  if (m_thd != nullptr) {
    notify_transaction_subscribers(
        [this](TransactionSubscriber &subscriber) { subscriber.on_transaction_commit(m_thd); });
  }
  return SHANNON_SUCCESS;
}

int Transaction::rollback() {
  const int ret = rollback_internal();

  if (m_thd != nullptr) {
    notify_transaction_subscribers(
        [this](TransactionSubscriber &subscriber) { subscriber.on_transaction_rollback(m_thd); });
  }
  return ret;
}

int Transaction::commit_internal() {
  sync_coordinator_state(CoordState::FINALIZING);

  // Never call trx_commit_for_mysql() here. m_primary_trx is owned and
  // committed by InnoDB/server transaction coordination. This commit only
  // finalizes legacy/internal Rapid physical MVCC state if any was registered.
  if (m_coord_state == CoordState::ACTIVE && !TransactionCoordinator::instance().commit_transaction(this)) {
    if (!TransactionCoordinator::instance().rollback_transaction(this)) {
      TransactionCoordinator::instance().unregister_transaction(this);
    }
    release_snapshot();
    m_coord_state = CoordState::UNREGISTERED;
    m_start_scn = 0;
    m_commit_scn = 0;
    m_stmt_active = false;
    return HA_ERR_GENERIC;
  }

  release_snapshot();
  m_coord_state = CoordState::UNREGISTERED;
  m_start_scn = 0;
  m_stmt_active = false;
  return SHANNON_SUCCESS;
}

int Transaction::rollback_internal() {
  sync_coordinator_state(CoordState::FINALIZING);

  // Never call trx_rollback_for_mysql() on the borrowed primary transaction.
  // Roll back only Rapid-local physical versions, if this compatibility path
  // was used by an internal mutation.
  if (m_coord_state == CoordState::ACTIVE) {
    if (!TransactionCoordinator::instance().rollback_transaction(this)) {
      TransactionCoordinator::instance().unregister_transaction(this);
    }
    m_coord_state = CoordState::UNREGISTERED;
  }

  release_snapshot();
  m_start_scn = 0;
  m_commit_scn = 0;
  m_stmt_active = false;
  return SHANNON_SUCCESS;
}

int Transaction::commit_stmt() {
  // COPY_INFO can register Rapid only for handlerton callbacks without
  // executing a Rapid iterator, so m_stmt_active is not a precondition for
  // publishing the server statement boundary to subscribers.
  m_stmt_active = false;
  if (m_thd != nullptr) {
    notify_transaction_subscribers(
        [this](TransactionSubscriber &subscriber) { subscriber.on_statement_commit(m_thd); });
  }
  return SHANNON_SUCCESS;
}

int Transaction::rollback_stmt() {
  // As with commit_stmt(), publish the server statement boundary even when
  // Rapid did not execute the statement itself. Subscribers own any
  // subsystem-specific response such as COPY_INFO fail-closed quarantine.
  m_stmt_active = false;
  if (m_thd != nullptr) {
    notify_transaction_subscribers(
        [this](TransactionSubscriber &subscriber) { subscriber.on_statement_rollback(m_thd); });
  }
  return SHANNON_SUCCESS;
}

::ReadView *Transaction::acquire_snapshot() {
  if (m_primary_trx == nullptr) return nullptr;

  // The ReadView is allocated *on the primary InnoDB transaction*. Rapid may
  // request the view because it is the consistent-read executor, but the view
  // remains InnoDB state and is never copied into a Rapid snapshot.
  if (!MVCC::is_view_active(m_primary_trx->read_view) && m_iso_level > ISOLATION_LEVEL::READ_UNCOMMITTED) {
    if (!trx_is_started(m_primary_trx)) {
      trx_start_if_not_started(m_primary_trx, false, UT_LOCATION_HERE);
    }
    trx_assign_read_view(m_primary_trx);
  }

  ::ReadView *view = m_primary_trx->read_view;
  if (MVCC::is_view_active(view) && !m_snapshot_registered) {
    // This SCN is only a conservative Rapid before-image retention fence. SQL
    // creator visibility is decided exclusively by this InnoDB ReadView.
    m_snapshot_scn = TransactionCoordinator::instance().get_current_scn();
    TransactionCoordinator::instance().register_snapshot(this, m_snapshot_scn);
    m_snapshot_registered = true;
  }
  return view;
}

int Transaction::release_snapshot() {
  if (m_snapshot_registered) {
    TransactionCoordinator::instance().unregister_snapshot(this);
    m_snapshot_registered = false;
    m_snapshot_scn = 0;
  }

  // Do not view_close() m_primary_trx->read_view here. The SQL ReadView belongs
  // to InnoDB's transaction/statement lifecycle; Rapid only drops its own GC
  // retention fence.
  return SHANNON_SUCCESS;
}

bool Transaction::changes_visible(Transaction::ID trx_id, const char *table_name) {
  ::ReadView *view = get_snapshot();
  if (MVCC::is_view_active(view)) {
    table_name_t name;
    name.m_name = const_cast<char *>(table_name);
    return view->changes_visible(trx_id, name);
  }
  return false;
}

bool Transaction::has_snapshot() const {
  return m_primary_trx != nullptr && MVCC::is_view_active(m_primary_trx->read_view);
}

::ReadView *Transaction::get_snapshot() const { return m_primary_trx != nullptr ? m_primary_trx->read_view : nullptr; }

void Transaction::register_imcu_modification(std::shared_ptr<ShannonBase::Imcs::Imcu> imcu) {
  if (!imcu) return;

  // This method represents a legacy/internal Rapid *physical* mutation, not the
  // notification/COPY_INFO path. Direct notification propagation is owned by
  // the Populate subsystem and bypasses this THD-local coordinator.
  const Transaction::ID txn_id = get_id();
  if (txn_id == 0) {
    // Read-only primary transactions use id 0, which must never become a Rapid
    // coordinator key or masquerade as a source writer transaction.
    ib::error() << "Rapid: refusing to register a physical IMCU mutation with primary trx id 0";
    return;
  }

  m_read_only = false;
  if (m_coord_state == CoordState::UNREGISTERED) {
    // Freeze the source writer identity before entering the Rapid physical
    // coordinator. InnoDB may reset trx_t::id before Rapid's commit callback.
    m_physical_txn_id.store(txn_id, std::memory_order_release);
    m_start_scn = TransactionCoordinator::instance().register_transaction(this, m_iso_level);
    m_coord_state = CoordState::ACTIVE;
  } else if (m_physical_txn_id.load(std::memory_order_acquire) != txn_id) {
    ib::error() << "Rapid: primary trx id changed while a physical Rapid transaction is active (captured="
                << m_physical_txn_id.load(std::memory_order_acquire) << ", current=" << txn_id << ")";
    return;
  }
  if (m_coord_state == CoordState::ACTIVE) {
    TransactionCoordinator::instance().register_imcu_modification(m_physical_txn_id.load(std::memory_order_acquire),
                                                                  std::move(imcu));
  }
}

uint64_t TransactionCoordinator::register_transaction(Transaction *trx, Transaction::ISOLATION_LEVEL iso_level) {
  ut_a(trx != nullptr);
  // Lazy-start the worker thread on first write transaction: Start batch worker on first write txn, not at construction
  if (!trx->m_read_only) {
    bool expected = false;
    if (m_batch_worker_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      m_batch_commit_worker = std::thread(&TransactionCoordinator::batch_commit_worker_loop, this);
    }
  }

  const Transaction::ID txn_id = trx->m_physical_txn_id.load(std::memory_order_acquire);
  ut_ad(txn_id != 0);
  if (txn_id == 0) return m_max_observed_commit_scn.load(std::memory_order_acquire);

  uint64_t start_scn = m_max_observed_commit_scn.load(std::memory_order_acquire);

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
  ut_a(trx != nullptr);
  const Transaction::ID txn_id = trx->m_physical_txn_id.load(std::memory_order_acquire);
  if (txn_id == 0) return false;

  {
    std::shared_lock<std::shared_mutex> lock(m_txns_mutex);
    auto it = m_active_txns.find(txn_id);
    if (it == m_active_txns.end()) return false;
    if (it->second.modified_imcus.empty()) {
      lock.unlock();
      std::unique_lock<std::shared_mutex> wlock(m_txns_mutex);
      m_active_txns.erase(txn_id);
      update_min_active_scn();
      trx->m_physical_txn_id.store(0, std::memory_order_release);
      return true;
    }
  }

  // Rapid SCN is a physical version/publication clock. It must not be borrowed
  // from InnoDB trx->no, because SQL visibility is already decided by ReadView
  // and the two clocks deliberately have different semantics.
  const uint64_t commit_scn = Transaction::VersionManager::instance().allocate_scn();
  return commit_transaction(trx, commit_scn);
}

bool TransactionCoordinator::commit_transaction(Transaction *trx, uint64_t commit_scn) {
  ut_a(trx != nullptr);

  bool ok = commit_transaction_internal(trx, commit_scn);
  if (!ok) return false;

  observe_commit_scn(commit_scn);
  return true;
}

bool TransactionCoordinator::commit_transaction_internal(Transaction *trx, uint64_t commit_scn) {
  ut_a(trx != nullptr);
  const Transaction::ID txn_id = trx->m_physical_txn_id.load(std::memory_order_acquire);
  if (txn_id == 0) return false;

  std::vector<std::shared_ptr<ShannonBase::Imcs::Imcu>> imcus_to_commit;
  {
    std::shared_lock<std::shared_mutex> lock(m_txns_mutex);
    auto it = m_active_txns.find(txn_id);
    if (it == m_active_txns.end()) return false;
    imcus_to_commit = it->second.modified_imcus;
  }

  for (auto &imcu : imcus_to_commit) {
    if (!imcu) continue;
    // Publish column-version commit metadata before publishing the row journal.
    // Otherwise a new reader could observe a committed UPDATE row but still
    // treat its column version as ACTIVE and incorrectly reconstruct old data.
    imcu->commit_transaction(txn_id, commit_scn);
  }

  {
    std::unique_lock<std::shared_mutex> lock(m_txns_mutex);
    auto it = m_active_txns.find(txn_id);
    if (it == m_active_txns.end()) return false;

    it->second.commit_scn = commit_scn;
    it->second.status = TransactionInfo::COMMITTED;
    trx->m_commit_scn = commit_scn;
    m_active_txns.erase(it);
    update_min_active_scn();
  }
  trx->m_physical_txn_id.store(0, std::memory_order_release);

  for (auto &imcu : imcus_to_commit) {
    if (imcu) invalidate_visibility_cache(imcu.get());
  }
  return true;
}

bool TransactionCoordinator::rollback_transaction(Transaction *trx) {
  ut_a(trx != nullptr);
  const Transaction::ID txn_id = trx->m_physical_txn_id.load(std::memory_order_acquire);
  if (txn_id == 0) return false;

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

  for (auto &imcu : imcus_to_rollback) {
    if (imcu == nullptr) continue;
    // Restore UPDATE before-images first, then abort the row journal so INSERT /
    // DELETE physical metadata is reconciled with the same transaction outcome.
    if (!imcu->rollback_transaction(txn_id)) {
      ib::error() << "Rapid: failed to rollback IMCU value versions for txn " << txn_id;
    }
  }

  for (auto &imcu : imcus_to_rollback) {
    if (imcu) invalidate_visibility_cache(imcu.get());
  }
  trx->m_physical_txn_id.store(0, std::memory_order_release);
  return true;
}

void TransactionCoordinator::unregister_transaction(Transaction *trx) {
  ut_a(trx != nullptr);

  const Transaction::ID txn_id = trx->m_physical_txn_id.load(std::memory_order_acquire);
  std::unique_lock lock(m_txns_mutex);
  // Defensive cleanup: release_snapshot() normally removes this first, but an
  // external abort/disconnect must never leave GC permanently fenced.
  m_active_snapshots.erase(trx);
  // Transaction still in active list indicates improper commit/rollback, cleanup required
  auto it = m_active_txns.find(txn_id);
  if (it != m_active_txns.end()) {
    // Notify all IMCUs txn aborted.
    for (auto &imcu : it->second.modified_imcus) {
      if (imcu && !imcu->rollback_transaction(txn_id)) {
        ib::error() << "Rapid: failed to rollback IMCU value versions while unregistering txn " << txn_id;
      }
    }
    m_active_txns.erase(it);
    update_min_active_scn();
    m_total_aborted.fetch_add(1, std::memory_order_relaxed);
  }
  trx->m_physical_txn_id.store(0, std::memory_order_release);
}

void TransactionCoordinator::register_imcu_modification(Transaction::ID txn_id,
                                                        std::shared_ptr<ShannonBase::Imcs::Imcu> imcu) {
  std::unique_lock lock(m_txns_mutex);

  auto it = m_active_txns.find(txn_id);
  if (it == m_active_txns.end() || !imcu) return;

  auto &modified = it->second.modified_imcus;
  if (std::find(modified.begin(), modified.end(), imcu) == modified.end()) modified.push_back(std::move(imcu));
}

void TransactionCoordinator::register_snapshot(Transaction *trx, uint64_t snapshot_scn) {
  if (trx == nullptr) return;
  std::unique_lock lock(m_txns_mutex);
  m_active_snapshots[trx] = snapshot_scn;
  update_min_active_scn();
}

void TransactionCoordinator::unregister_snapshot(Transaction *trx) {
  if (trx == nullptr) return;
  std::unique_lock lock(m_txns_mutex);
  m_active_snapshots.erase(trx);
  update_min_active_scn();
}

Transaction::VersionManager::Snapshot TransactionCoordinator::create_snapshot() {
  std::shared_lock lock(m_txns_mutex);
  std::vector<Transaction::ID> active_txns;
  active_txns.reserve(m_active_txns.size());

  for (const auto &[txn_id, info] : m_active_txns) {
    if (info.status == TransactionInfo::ACTIVE) active_txns.push_back(txn_id);
  }

  uint64_t current_scn = get_current_scn();
  if (auto cached = get_cached_snapshot(current_scn, active_txns)) {
    m_snapshot_cache_hits.fetch_add(1, std::memory_order_relaxed);
    return *cached;
  }

  m_snapshot_cache_misses.fetch_add(1, std::memory_order_relaxed);

  Transaction::VersionManager::Snapshot snapshot;
  snapshot.scn = current_scn;
  snapshot.active_txns = active_txns;
  snapshot.created_at = std::chrono::steady_clock::now();

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
  while (m_batch_running.load(std::memory_order_acquire)) {
    std::vector<BatchCommitRequest> batch;
    {
      std::unique_lock<std::mutex> lock(m_batch_commit_mutex);
      m_batch_commit_cv.wait(
          lock, [this] { return !m_pending_commits.empty() || !m_batch_running.load(std::memory_order_acquire); });

      if (!m_batch_running.load(std::memory_order_acquire)) break;
      if (m_pending_commits.empty()) continue;

      // Drain up to m_batch_commit_size entries
      size_t n = std::min(m_pending_commits.size(), m_batch_commit_size);
      batch.insert(batch.end(), std::make_move_iterator(m_pending_commits.begin()),
                   std::make_move_iterator(m_pending_commits.begin() + n));
      m_pending_commits.erase(m_pending_commits.begin(), m_pending_commits.begin() + n);
    }
    process_batch_commits(batch);
  }
}

void TransactionCoordinator::process_batch_commits(std::vector<BatchCommitRequest> &batch) {
  uint64_t base_scn = Transaction::VersionManager::instance().allocate_scn_batch(batch.size());

  for (size_t i = 0; i < batch.size(); ++i) {
    auto &req = batch[i];
    uint64_t commit_scn = base_scn + i;
    commit_transaction_internal(req.trx, commit_scn);
    req.commit_scn_promise.set_value(commit_scn);
  }
}

void TransactionCoordinator::update_min_active_scn() {
  // Caller holds m_txns_mutex exclusively.
  //
  // Rapid publication SCN and InnoDB ReadView are deliberately NOT treated as
  // the same clock. Therefore an arbitrary Rapid snapshot_scn is not a safe
  // purge boundary for an active primary ReadView. Until we plumb InnoDB's
  // native purge low-limit into Rapid, retain all MVCC history while any SQL
  // ReadView is active.
  if (!m_active_snapshots.empty()) {
    Transaction::VersionManager::instance().set_min_active_scn(0);
    return;
  }

  uint64_t min_scn = UINT64_MAX;
  for (const auto &[tid, info] : m_active_txns) {
    (void)tid;
    min_scn = std::min(min_scn, info.start_scn);
  }

  if (min_scn == UINT64_MAX) min_scn = get_current_scn();
  Transaction::VersionManager::instance().set_min_active_scn(min_scn);
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

void TransactionCoordinator::observe_commit_scn(uint64_t commit_scn) {
  uint64_t prev = m_max_observed_commit_scn.load(std::memory_order_relaxed);
  while (commit_scn > prev &&
         !m_max_observed_commit_scn.compare_exchange_weak(prev, commit_scn, std::memory_order_acq_rel)) {
  }

  // Re-evaluate the GC watermark against the newly-published physical clock.
  // update_min_active_scn() requires the transaction mutex exclusively.
  std::unique_lock lock(m_txns_mutex);
  update_min_active_scn();
}

void TransactionJournal::add_entry(Entry &&entry) {
  row_id_t row_id = entry.row_id;
  Transaction::ID txn_id = entry.txn_id;
  auto &shard = m_shards[shard_of(row_id)];
  std::unique_lock lock(shard.mutex);

  auto new_entry = std::make_unique<Entry>(std::move(entry));
  auto it = shard.entries.find(row_id);
  if (it != shard.entries.end()) {
    new_entry->prev = it->second.release();
    it->second = std::move(new_entry);
  } else {
    shard.entries[row_id] = std::move(new_entry);
  }

  if (shard.entries[row_id]->status == ACTIVE) {
    shard.txn_entries[txn_id].push_back(shard.entries[row_id].get());
    shard.active_txns.insert(txn_id);
  }

  m_entry_count.fetch_add(1);
  m_total_size.fetch_add(sizeof(Entry));
}

void TransactionJournal::commit_transaction(Transaction::ID txn_id, uint64_t commit_scn) {
  // A transaction may have entries in multiple shards.
  for (size_t i = 0; i < NUM_JOURNAL_SHARDS; ++i) {
    std::unique_lock lock(m_shards[i].mutex);
    auto it = m_shards[i].txn_entries.find(txn_id);
    if (it == m_shards[i].txn_entries.end()) continue;

    for (Entry *entry : it->second) {
      if (!entry) continue;
      entry->scn = commit_scn;
      entry->status = COMMITTED;
    }
    m_shards[i].active_txns.erase(txn_id);
    m_shards[i].txn_entries.erase(it);
  }
}

void TransactionJournal::abort_transaction(Transaction::ID txn_id, ShannonBase::bit_array_t *del_mask,
                                           ShannonBase::Imcs::RowDirectory *row_dir) {
  size_t marked = 0;
  for (size_t i = 0; i < NUM_JOURNAL_SHARDS; ++i) {
    std::unique_lock lock(m_shards[i].mutex);
    auto it = m_shards[i].txn_entries.find(txn_id);
    if (it == m_shards[i].txn_entries.end()) continue;

    for (Entry *entry : it->second) {
      if (!entry) continue;
      entry->status = ABORTED;
      ++marked;

      //   - ABORTED INSERT: row was written to CUs but should not be
      //     visible → SET the delete bit.
      //   - ABORTED DELETE: row was marked deleted but the delete
      //     didn't actually happen → CLEAR the delete bit.
      if (del_mask) {
        if (static_cast<ShannonBase::OPER_TYPE>(entry->operation) == ShannonBase::OPER_TYPE::OPER_INSERT) {
          ShannonBase::Utils::Util::bit_array_set(del_mask, entry->row_id);
        } else if (static_cast<ShannonBase::OPER_TYPE>(entry->operation) == ShannonBase::OPER_TYPE::OPER_DELETE) {
          ShannonBase::Utils::Util::bit_array_reset(del_mask, entry->row_id);
          if (row_dir) row_dir->clear_deleted(entry->row_id);
        }
      }
    }
    m_shards[i].active_txns.erase(txn_id);
    m_shards[i].txn_entries.erase(it);
  }
  if (marked > 0) m_aborted_count.fetch_add(marked, std::memory_order_release);
}

bool TransactionJournal::is_row_visible(row_id_t row_id, Transaction::ID reader_txn_id, uint64_t reader_scn,
                                        bool no_journal_visible, Transaction *reader_trx,
                                        const char *table_name) const {
  auto &shard = m_shards[shard_of(row_id)];
  std::shared_lock lock(shard.mutex);
  auto it = shard.entries.find(row_id);
  if (it == shard.entries.end()) return no_journal_visible;

  const bool sql_reader = reader_trx != nullptr;
  const bool use_primary_read_view = sql_reader && table_name != nullptr && reader_trx->has_snapshot();

  // TransactionJournal is a delta chain over the IMCU base image.  LOAD rows
  // have no synthetic INSERT entry, so falling off such a chain means the base
  // row still exists.  A chain rooted at INSERT has no base row.
  bool base_row_exists = true;
  Entry *entry = it->second.get();
  while (entry != nullptr) {
    const auto operation = static_cast<ShannonBase::OPER_TYPE>(entry->operation);
    if (entry->prev == nullptr && operation == ShannonBase::OPER_TYPE::OPER_INSERT) base_row_exists = false;

    // ABORTED entries never define a visible state.
    if (entry->status == ABORTED) {
      entry = entry->prev;
      continue;
    }

    // Read-your-writes only exists for a real InnoDB writer id. Read-only
    // InnoDB transactions use id 0, and Rapid also uses txn_id 0 for
    // transaction-independent LOAD/system versions; 0 == 0 is not ownership.
    if (reader_txn_id != 0 && entry->txn_id == reader_txn_id) {
      return operation != ShannonBase::OPER_TYPE::OPER_DELETE;
    }

    // Rapid outcome publication is asynchronous with respect to the primary
    // commit. An entry can still be marked ACTIVE for a very small interval
    // after InnoDB has committed. For SQL readers, the primary ReadView is the
    // authoritative creator-state oracle even in that interval. This closes
    // the primary-COMMIT -> Rapid-outcome callback visibility gap without
    // fencing the table for the whole writer transaction.
    if (entry->status == ACTIVE) {
      const bool creator_already_visible =
          sql_reader && use_primary_read_view && reader_trx->changes_visible(entry->txn_id, table_name);
      if (!creator_already_visible) {
        entry = entry->prev;
        continue;
      }

      switch (operation) {
        case ShannonBase::OPER_TYPE::OPER_INSERT:
        case ShannonBase::OPER_TYPE::OPER_UPDATE:
          return true;
        case ShannonBase::OPER_TYPE::OPER_DELETE:
          return false;
        default:
          entry = entry->prev;
          continue;
      }
    }

    if (entry->status == COMMITTED) {
      bool creator_visible = true;
      if (sql_reader) {
        // SQL readers have exactly one visibility oracle: the primary InnoDB
        // ReadView. txn_id 0 denotes a transaction-independent committed
        // LOAD/system version and therefore has no creator to test. If a SQL
        // reader unexpectedly lacks a ReadView, fail closed for real creators
        // instead of silently creating a second Rapid-SCN snapshot universe.
        creator_visible =
            (entry->txn_id == 0) || (use_primary_read_view && reader_trx->changes_visible(entry->txn_id, table_name));
      } else {
        // Rapid-internal/recovery callers have no SQL ReadView. Their physical
        // version traversal is intentionally SCN based.
        creator_visible = entry->scn <= reader_scn;
      }

      if (!creator_visible) {
        entry = entry->prev;
        continue;
      }

      switch (operation) {
        case ShannonBase::OPER_TYPE::OPER_INSERT:
        case ShannonBase::OPER_TYPE::OPER_UPDATE:
          return true;
        case ShannonBase::OPER_TYPE::OPER_DELETE:
          return false;
        default:
          break;
      }
    }

    entry = entry->prev;
  }

  return base_row_exists;
}

void TransactionJournal::check_visibility_batch(row_id_t start_row, size_t count, Transaction::ID reader_txn_id,
                                                uint64_t reader_scn, bit_array_t &visibility_mask) const {
  for (size_t i = 0; i < count; i++) {
    row_id_t row_id = start_row + i;
    bool visible = is_row_visible(row_id, reader_txn_id, reader_scn);
    (visible) ? Utils::Util::bit_array_set(&visibility_mask, i) : Utils::Util::bit_array_reset(&visibility_mask, i);
  }
}

ShannonBase::OPER_TYPE TransactionJournal::get_row_state_at_scn(
    row_id_t row_id, uint64_t target_scn, std::bitset<SHANNON_MAX_COLUMNS> *modified_columns) const {
  auto &shard = m_shards[shard_of(row_id)];
  std::shared_lock lock(shard.mutex);
  auto it = shard.entries.find(row_id);
  if (it == shard.entries.end()) return ShannonBase::OPER_TYPE::OPER_NONE;

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
  size_t purged = 0;
  size_t aborted_removed = 0;
  for (size_t s = 0; s < NUM_JOURNAL_SHARDS; ++s) {
    std::unique_lock lock(m_shards[s].mutex);
    auto &shard = m_shards[s];
    for (auto it = shard.entries.begin(); it != shard.entries.end();) {
      Entry *const original_head = it->second.get();
      Entry *head = original_head;
      Entry *current = head;
      Entry *prev_valid = nullptr;

      bool found_visible = false;
      while (current != nullptr) {
        if (current->status == ABORTED) {
          ++aborted_removed;
          Entry *to_delete = current;
          current = current->prev;
          if (prev_valid)
            prev_valid->prev = current;
          else
            head = current;
          to_delete->prev = nullptr;
          delete to_delete;
          purged++;
          m_entry_count.fetch_sub(1);
          m_total_size.fetch_sub(sizeof(Entry));
          continue;
        }

        if (current->status == COMMITTED && current->scn < min_active_scn && found_visible) {
          Entry *to_delete = current;
          current = current->prev;
          if (prev_valid) prev_valid->prev = current;
          to_delete->prev = nullptr;
          delete to_delete;
          purged++;
          m_entry_count.fetch_sub(1);
          m_total_size.fetch_sub(sizeof(Entry));
        } else {
          if (current->status == COMMITTED) {
            found_visible = true;
            prev_valid = current;
          }
          current = current->prev;
        }
      }

      // If the chain's front entry was itself freed above (the ABORTED branch
      // deletes `current` even on its very first iteration, when current ==
      // original_head), `it->second` still internally owns that now-freed
      // pointer. release() drops it without a second delete, then reset()
      // hands ownership to whatever entry (possibly null) survived as head.
      if (head != original_head) {
        it->second.release();
        it->second.reset(head);
      }

      if (head == nullptr ||
          (head->prev == nullptr && head->status == COMMITTED && head->scn < min_active_scn &&
           static_cast<ShannonBase::OPER_TYPE>(head->operation) == ShannonBase::OPER_TYPE::OPER_INSERT)) {
        it = shard.entries.erase(it);
      } else {
        ++it;
      }
    }

    for (auto txn_it = shard.txn_entries.begin(); txn_it != shard.txn_entries.end();) {
      if (shard.active_txns.find(txn_it->first) == shard.active_txns.end()) {
        txn_it = shard.txn_entries.erase(txn_it);
      } else {
        ++txn_it;
      }
    }
  }
  if (aborted_removed > 0) {
    assert(m_aborted_count.load(std::memory_order_acquire) >= aborted_removed);
    m_aborted_count.fetch_sub(aborted_removed, std::memory_order_release);
  }
  return purged;
}

size_t TransactionJournal::purge_aborted() {
  size_t purged = 0;
  for (size_t s = 0; s < NUM_JOURNAL_SHARDS; ++s) {
    std::unique_lock lock(m_shards[s].mutex);
    auto &shard = m_shards[s];
    for (auto it = shard.entries.begin(); it != shard.entries.end();) {
      Entry *head = it->second.get();
      if (head->status == ABORTED && head->prev == nullptr) {
        it = shard.entries.erase(it);
        purged++;
        m_entry_count.fetch_sub(1);
        m_total_size.fetch_sub(sizeof(Entry));
      } else {
        ++it;
      }
    }
  }
  if (purged > 0) {
    assert(m_aborted_count.load(std::memory_order_acquire) >= purged);
    m_aborted_count.fetch_sub(purged, std::memory_order_release);
  }
  return purged;
}

void TransactionJournal::dump(std::ostream &out) const {
  out << "Transaction Journal: " << m_entry_count.load() << " entries\n";
  for (size_t s = 0; s < NUM_JOURNAL_SHARDS; ++s) {
    std::shared_lock lock(m_shards[s].mutex);
    for (const auto &[row_id, entry] : m_shards[s].entries) {
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
}
}  // namespace ShannonBase