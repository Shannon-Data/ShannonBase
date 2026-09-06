#include <gtest/gtest.h>
#include <limits>

#include "storage/rapid_engine/trx/transaction.h"

using namespace ShannonBase;

TEST(TransactionJournalPurge, PurgeCommittedAndAborted) {
  // Create a small journal
  TransactionJournal journal(1024);

  // Prepare entries for row 1 and row 2 across two transactions
  TransactionJournal::Entry e1;
  e1.row_id = 1;
  e1.operation = 1; // UPDATE
  e1.status = TransactionJournal::EntryStatus::ACTIVE;
  e1.txn_id = 100;
  e1.scn = 0;

  TransactionJournal::Entry e2;
  e2.row_id = 1;
  e2.operation = 1;
  e2.status = TransactionJournal::EntryStatus::ACTIVE;
  e2.txn_id = 101;
  e2.scn = 0;

  journal.add_entry(std::move(e1));
  journal.add_entry(std::move(e2));

  // Commit txn 100 at scn 10 and txn 101 at scn 20
  journal.commit_transaction(100, 10);
  journal.commit_transaction(101, 20);

  // After committing, purge with min_active_scn = 15 should remove versions <=15
  size_t purged = journal.purge(15);
  EXPECT_GT(purged, 0u);

  // Add an aborted transaction
  TransactionJournal::Entry e3;
  e3.row_id = 2;
  e3.operation = 2; // DELETE
  e3.status = TransactionJournal::EntryStatus::ACTIVE;
  e3.txn_id = 200;
  e3.scn = 0;
  journal.add_entry(std::move(e3));

  journal.abort_transaction(200);
  size_t aborted_purged = journal.purge_aborted();
  EXPECT_GT(aborted_purged, 0u);

  // Journal should have zero active transactions now
  EXPECT_TRUE(journal.is_all_committed());
}

namespace {

/// Build an ACTIVE journal entry; the caller commits or aborts its txn after.
TransactionJournal::Entry make_entry(uint64_t row_id, ShannonBase::OPER_TYPE op, Transaction::ID txn_id) {
  TransactionJournal::Entry e;
  e.row_id = row_id;
  e.operation = static_cast<decltype(e.operation)>(op);
  e.status = TransactionJournal::EntryStatus::ACTIVE;
  e.txn_id = txn_id;
  e.scn = 0;
  return e;
}

/// Add one UPDATE on `row` by `txn` and commit it at `scn`.
void commit_update(TransactionJournal &journal, uint64_t row, Transaction::ID txn, uint64_t scn) {
  journal.add_entry(make_entry(row, ShannonBase::OPER_TYPE::OPER_UPDATE, txn));
  journal.commit_transaction(txn, scn);
}

}  // namespace

// An uncommitted (ACTIVE) entry is still owned by a running transaction: purging
// it would discard the before-image its rollback depends on. No watermark, however
// high, may remove it.
TEST(TransactionJournalPurge, ActiveEntriesAreNeverPurged) {
  TransactionJournal journal(1024);
  journal.add_entry(make_entry(1, ShannonBase::OPER_TYPE::OPER_UPDATE, 100));

  const size_t before = journal.get_entry_count();
  EXPECT_EQ(journal.purge(std::numeric_limits<uint64_t>::max()), 0u);
  EXPECT_EQ(journal.get_entry_count(), before);
  EXPECT_FALSE(journal.is_all_committed());
}

// The retention invariant. Versions older than min_active_scn are reclaimable
// EXCEPT the newest one: a reader at min_active_scn still resolves the row
// through it. Dropping every version below the watermark would lose the row's
// state entirely.
TEST(TransactionJournalPurge, RetainsNewestCommittedVersion) {
  TransactionJournal journal(1024);
  commit_update(journal, 1, 100, 10);
  commit_update(journal, 1, 101, 20);
  commit_update(journal, 1, 102, 30);
  ASSERT_EQ(journal.get_entry_count(), 3u);

  // Watermark 25: scn 30 is the newest and must survive; 10 and 20 are behind it
  // and are reclaimable.
  EXPECT_EQ(journal.purge(25), 2u);
  EXPECT_EQ(journal.get_entry_count(), 1u);

  // The surviving version still answers visibility for a current reader.
  EXPECT_EQ(journal.get_row_state_at_scn(1, 40), ShannonBase::OPER_TYPE::OPER_UPDATE);
}

// Nothing at or above the watermark may be dropped, even when several committed
// versions are stacked on the row.
TEST(TransactionJournalPurge, RespectsWatermark) {
  TransactionJournal journal(1024);
  commit_update(journal, 1, 100, 10);
  commit_update(journal, 1, 101, 20);
  commit_update(journal, 1, 102, 30);

  EXPECT_EQ(journal.purge(5), 0u);
  EXPECT_EQ(journal.get_entry_count(), 3u);
}

// An aborted entry is visible to nobody, so it is reclaimable immediately and
// independently of the watermark.
TEST(TransactionJournalPurge, AbortedEntriesRemovedRegardlessOfScn) {
  TransactionJournal journal(1024);
  journal.add_entry(make_entry(7, ShannonBase::OPER_TYPE::OPER_UPDATE, 200));
  journal.abort_transaction(200);

  EXPECT_EQ(journal.purge(0), 1u);
  EXPECT_EQ(journal.get_entry_count(), 0u);
}

// GC runs repeatedly on the same table; a second pass at an unchanged watermark
// must be a no-op rather than eating into still-needed history.
TEST(TransactionJournalPurge, PurgeIsIdempotent) {
  TransactionJournal journal(1024);
  commit_update(journal, 1, 100, 10);
  commit_update(journal, 1, 101, 20);
  commit_update(journal, 1, 102, 30);

  const size_t first = journal.purge(25);
  EXPECT_GT(first, 0u);
  const size_t remaining = journal.get_entry_count();

  EXPECT_EQ(journal.purge(25), 0u);
  EXPECT_EQ(journal.get_entry_count(), remaining);
}

// The count purge reports must match what it actually freed, since the caller
// accounts freed bytes from it.
TEST(TransactionJournalPurge, EntryCountMatchesPurgedCount) {
  TransactionJournal journal(1024);
  commit_update(journal, 1, 100, 10);
  commit_update(journal, 1, 101, 20);
  commit_update(journal, 2, 102, 10);
  commit_update(journal, 2, 103, 20);

  const size_t before = journal.get_entry_count();
  const size_t purged = journal.purge(15);
  EXPECT_EQ(journal.get_entry_count(), before - purged);
}

// Purging entries on one row must not disturb an unrelated row, including when
// the two land in different journal shards.
TEST(TransactionJournalPurge, PurgeIsPerRowIndependent) {
  TransactionJournal journal(1024);
  commit_update(journal, 1, 100, 10);
  commit_update(journal, 1, 101, 20);
  journal.add_entry(make_entry(2, ShannonBase::OPER_TYPE::OPER_UPDATE, 102));  // stays ACTIVE

  journal.purge(15);

  // Row 2's uncommitted entry is untouched.
  EXPECT_FALSE(journal.is_all_committed());
  EXPECT_EQ(journal.get_row_state_at_scn(1, 30), ShannonBase::OPER_TYPE::OPER_UPDATE);
}
