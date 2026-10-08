/* Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
 * Unit test for the Rapid engine WAL (TablePersistenceManager).
 *
 * The WAL is the only record of a change between the moment it is applied to
 * volatile IMCS memory and the moment a checkpoint makes it durable, so its
 * failure modes are silent by construction: a replay that stops early, an
 * uncommitted operation that gets applied anyway, or an LSN that is reused
 * after a restart all produce a Rapid image that simply disagrees with InnoDB.
 * The MTR suite can only observe that indirectly (shannon_wal_checkpoint_
 * recovery and shannon_checkpoint_generation restart the server and compare
 * row counts), and it cannot reach the corruption paths at all, because it has
 * no way to hand the engine a torn or bit-flipped log.
 *
 * The invariants pinned here:
 *
 *   1. LSNs are assigned monotonically and are never reused across a reopen.
 *   2. Every record type survives a write/replay round trip unchanged.
 *   3. A ROW_PREPARE is applied only when its ROW_COMMIT is present, and only
 *      when the commit's digest describes that exact prepare.
 *   4. A torn tail is recoverable; corruption in the middle of the log is NOT
 *      -- it must abort recovery rather than silently report success over a
 *      partial replay.
 *   5. WAL GC never discards a prefix that no checkpoint covers.
 *   6. Once a commit's durability is unknown, the log refuses further appends.
 */

#include "storage/rapid_engine/recovery/table_persistence.h"

#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <deque>
#include "storage/rapid_engine/populate/log_populate.h"
#include "storage/rapid_engine/populate/log_dml_notification.h"
#include "my_dbug.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/recovery/durable_fs.h"

namespace shannon_rapid_wal_unittest {

namespace fs = std::filesystem;

using ShannonBase::ErrorCode;
using ShannonBase::Imcs::TablePersistenceManager;
using ShannonBase::Imcs::Imcu;
using ShannonBase::Imcs::ManifestImcuEntry;
using ShannonBase::Imcs::ManifestImcuState;
using ShannonBase::Imcs::RecoveryManifest;
using ShannonBase::Imcs::WAL_MUT_DELETE;
using ShannonBase::Imcs::WAL_MUT_INSERT;
using ShannonBase::Imcs::WAL_MUT_UPDATE;
using ShannonBase::Imcs::WalCell;
using ShannonBase::Imcs::WalOpType;
using ShannonBase::Imcs::WalRecord;

constexpr const char *kDb = "wal_db";
constexpr const char *kTbl = "wal_tbl";

// recover() short-circuits on an empty IMCU set, so replay needs a live IMCU
// to route records to. A default-constructed Imcu carries id 0 and owns
// nothing, which is exactly the cold-start shape: load_snapshot() finds no
// snapshot file and returns NOT_FOUND, leaving the checkpoint LSN at 0 so the
// whole WAL is replayed. Every record written below therefore targets IMCU 0.
constexpr uint32_t kImcu = 0;

constexpr uint64_t kTxn = 4242;
constexpr uint64_t kScn = 777;

// The WAL API marks a NULL cell with InnoDB's UNIV_SQL_NULL sentinel, but
// including univ.i / ut0dbg.h here would drag InnoDB's mutex configuration
// into a gunit binary that is not built with it. The sentinel is a fixed
// on-disk contract (univ.i defines it as UINT32_UNDEFINED), so mirror the
// value rather than the header.
constexpr size_t kSqlNull = static_cast<size_t>(0xFFFFFFFFu);

std::vector<uint8_t> Bytes(const char *s) { return std::vector<uint8_t>(s, s + strlen(s)); }

class RapidWalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static int seq = 0;
    m_dir = fs::temp_directory_path() /
            ("rapid_wal_ut_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(seq++));
    std::error_code ec;
    fs::remove_all(m_dir, ec);
    ASSERT_TRUE(fs::create_directories(m_dir, ec)) << ec.message();
    m_mgr = MakeManager();
    ASSERT_TRUE(m_mgr->open());
  }

  void TearDown() override {
    if (m_mgr) m_mgr->close();
    m_mgr.reset();
    std::error_code ec;
    fs::remove_all(m_dir, ec);
  }

  std::unique_ptr<TablePersistenceManager> MakeManager() const {
    return std::make_unique<TablePersistenceManager>(m_dir.string(), kDb, kTbl);
  }

  fs::path WalPath() const { return m_mgr->wal_path(); }

  /** Replay the WAL and collect every record handed to apply_fn. */
  ShannonBase::Result<size_t> Replay(TablePersistenceManager *mgr, std::vector<WalRecord> *out) const {
    Imcu imcu;  // id 0, no owner, no snapshot: cold start
    std::vector<Imcu *> imcus{&imcu};
    return mgr->recover(imcus, [out](const WalRecord &rec) {
      out->push_back(rec);
      return ErrorCode::OK;
    });
  }

  std::vector<WalRecord> ReplayExpectOk(TablePersistenceManager *mgr) const {
    std::vector<WalRecord> got;
    auto res = Replay(mgr, &got);
    EXPECT_EQ(ErrorCode::OK, res.error);
    EXPECT_EQ(got.size(), res.value);
    return got;
  }

  /** Overwrite `len` bytes at `offset` of the WAL with `fill`. */
  void PokeWal(std::streamoff offset, size_t len, char fill) const {
    std::fstream f(WalPath(), std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(f.is_open());
    f.seekp(offset);
    std::string junk(len, fill);
    f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    ASSERT_TRUE(f.good());
  }

  // persist_manifest() writes into <partition>/checkpoints/, which checkpoint()
  // creates on the live path before it publishes a generation. A test that
  // persists a manifest directly has to do the same, or the write fails simply
  // because the directory is not there.
  void EnsureCheckpointDirs() const {
    std::error_code ec;
    fs::create_directories(m_mgr->manifest_path(1).parent_path(), ec);
    ASSERT_FALSE(ec) << ec.message();
  }

  uint64_t WalSize() const {
    std::error_code ec;
    const auto n = fs::file_size(WalPath(), ec);
    return ec ? 0 : static_cast<uint64_t>(n);
  }

  fs::path m_dir;
  std::unique_ptr<TablePersistenceManager> m_mgr;
};

// ---------------------------------------------------------------- LSN policy

// LSNs are 1-based and strictly increasing, and written_lsn is the *next* LSN
// to hand out. A repeat would make two different changes indistinguishable
// during replay.
TEST_F(RapidWalTest, LsnsAreAssignedMonotonically) {
  EXPECT_EQ(1u, m_mgr->written_lsn());

  const auto v = Bytes("v1");
  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 10, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->log_update(kImcu, 0, 10, kTxn, kScn + 1, v.data(), v.size()));
  const uint64_t del_lsn = m_mgr->log_delete(kImcu, 0, 10, kTxn, kScn + 2);

  EXPECT_EQ(3u, del_lsn);
  EXPECT_EQ(4u, m_mgr->written_lsn());

  const auto got = ReplayExpectOk(m_mgr.get());
  ASSERT_EQ(3u, got.size());
  EXPECT_EQ(1u, got[0].lsn);
  EXPECT_EQ(2u, got[1].lsn);
  EXPECT_EQ(3u, got[2].lsn);
}

// A restart must not hand out an LSN the log already contains: replay orders
// records by LSN, so a reused LSN reorders history.
TEST_F(RapidWalTest, ReopenResumesAboveTheHighestLoggedLsn) {
  const auto v = Bytes("payload");
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(m_mgr->log_write(kImcu, 0, i, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->sync());
  m_mgr->close();

  auto reopened = MakeManager();
  ASSERT_TRUE(reopened->open());
  EXPECT_EQ(6u, reopened->written_lsn());

  ASSERT_TRUE(reopened->log_write(kImcu, 0, 99, kTxn, kScn, v.data(), v.size()));
  const auto got = ReplayExpectOk(reopened.get());
  ASSERT_EQ(6u, got.size());
  EXPECT_EQ(6u, got.back().lsn);
  reopened->close();
}

// written >= durable >= applied is the watermark invariant the recovery code
// relies on; mark_applied() may only move forward.
TEST_F(RapidWalTest, WatermarksOnlyMoveForward) {
  const auto v = Bytes("x");
  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 1, kTxn, kScn, v.data(), v.size()));
  EXPECT_EQ(0u, m_mgr->durable_lsn());  // nothing fsynced yet

  ASSERT_TRUE(m_mgr->sync());
  EXPECT_EQ(1u, m_mgr->durable_lsn());

  m_mgr->mark_applied(1);
  EXPECT_EQ(1u, m_mgr->applied_lsn());
  m_mgr->mark_applied(0);  // a stale publish must not roll the watermark back
  EXPECT_EQ(1u, m_mgr->applied_lsn());

  EXPECT_GE(m_mgr->written_lsn(), m_mgr->durable_lsn());
  EXPECT_GE(m_mgr->durable_lsn(), m_mgr->applied_lsn());
}

// ------------------------------------------------------------- record replay

// Every field of a legacy single-cell record has to survive the round trip:
// a value that comes back attached to the wrong row or column is applied to
// the wrong cell during recovery.
TEST_F(RapidWalTest, LegacyRecordsRoundTrip) {
  const auto ins = Bytes("inserted");
  const auto upd = Bytes("updated-longer-value");

  ASSERT_TRUE(m_mgr->log_write(kImcu, 3, 100, kTxn, kScn, ins.data(), ins.size()));
  ASSERT_TRUE(m_mgr->log_write(kImcu, 4, 100, kTxn, kScn, nullptr, kSqlNull));
  ASSERT_TRUE(m_mgr->log_update(kImcu, 3, 100, kTxn + 1, kScn + 1, upd.data(), upd.size()));
  ASSERT_TRUE(m_mgr->log_update(kImcu, 5, 100, kTxn + 1, kScn + 1, nullptr, kSqlNull));
  ASSERT_GT(m_mgr->log_delete(kImcu, 3, 100, kTxn + 2, kScn + 2), 0u);

  const auto got = ReplayExpectOk(m_mgr.get());
  ASSERT_EQ(5u, got.size());

  EXPECT_EQ(WalOpType::INSERT, got[0].op_type);
  EXPECT_EQ(3u, got[0].col_id);
  EXPECT_EQ(100u, got[0].row_id);
  EXPECT_EQ(kTxn, got[0].txn_id);
  EXPECT_EQ(kScn, got[0].scn);
  EXPECT_EQ(ins, got[0].val_data);

  // A NULL cell is its own op type and carries no payload: writing zero bytes
  // instead would restore an empty string, which is not the same value.
  EXPECT_EQ(WalOpType::NULL_INSERT, got[1].op_type);
  EXPECT_EQ(kSqlNull, got[1].val_len);
  EXPECT_TRUE(got[1].val_data.empty());

  EXPECT_EQ(WalOpType::UPDATE, got[2].op_type);
  EXPECT_EQ(upd, got[2].val_data);
  EXPECT_EQ(kTxn + 1, got[2].txn_id);

  EXPECT_EQ(WalOpType::NULL_UPDATE, got[3].op_type);
  EXPECT_EQ(kSqlNull, got[3].val_len);

  EXPECT_EQ(WalOpType::DELETE, got[4].op_type);
  EXPECT_EQ(0u, got[4].val_len);
  EXPECT_TRUE(got[4].val_data.empty());
}

// A committed multi-column mutation is delivered to replay as one record
// carrying every cell -- not as one record per cell, which would let recovery
// stop half way through a row.
TEST_F(RapidWalTest, CommittedRowPrepareIsAppliedWithEveryCell) {
  std::vector<WalCell> cells(3);
  cells[0].col_id = 0;
  cells[0].value = Bytes("c0");
  cells[1].col_id = 1;
  cells[1].is_null = true;  // a NULL cell inside the group
  cells[2].col_id = 2;
  cells[2].value = Bytes("c2-value");

  uint32_t op_crc = 0;
  const uint64_t op_id = m_mgr->log_row_prepare(kImcu, 55, kTxn, kScn, WAL_MUT_UPDATE, cells, &op_crc);
  ASSERT_GT(op_id, 0u);
  ASSERT_GT(m_mgr->log_row_commit(op_id, kImcu, static_cast<uint32_t>(cells.size()), op_crc), 0u);

  const auto got = ReplayExpectOk(m_mgr.get());
  // The COMMIT marker is bookkeeping; only the prepare is replayed.
  ASSERT_EQ(1u, got.size());
  EXPECT_EQ(WalOpType::ROW_PREPARE, got[0].op_type);
  EXPECT_EQ(op_id, got[0].lsn);
  EXPECT_EQ(55u, got[0].row_id);
  EXPECT_EQ(WAL_MUT_UPDATE, got[0].mut_type);
  ASSERT_EQ(3u, got[0].cells.size());
  EXPECT_EQ(Bytes("c0"), got[0].cells[0].value);
  EXPECT_TRUE(got[0].cells[1].is_null);
  EXPECT_TRUE(got[0].cells[1].value.empty());
  EXPECT_EQ(2u, got[0].cells[2].col_id);
  EXPECT_EQ(Bytes("c2-value"), got[0].cells[2].value);
}

// A DELETE group carries no cell redo, and replay hands it over rewritten as a
// plain DELETE so the apply path does not have to special-case it.
TEST_F(RapidWalTest, CommittedDeleteGroupIsDeliveredAsDelete) {
  uint32_t op_crc = 0;
  const uint64_t op_id = m_mgr->log_row_prepare(kImcu, 7, kTxn, kScn, WAL_MUT_DELETE, {}, &op_crc);
  ASSERT_GT(op_id, 0u);
  ASSERT_GT(m_mgr->log_row_commit(op_id, kImcu, 0, op_crc), 0u);

  const auto got = ReplayExpectOk(m_mgr.get());
  ASSERT_EQ(1u, got.size());
  EXPECT_EQ(WalOpType::DELETE, got[0].op_type);
  EXPECT_EQ(7u, got[0].row_id);
  EXPECT_EQ(0u, got[0].val_len);
  EXPECT_TRUE(got[0].val_data.empty());
}

// A prepare whose commit never reached the log is an operation that was in
// flight when the server died: it must NOT be applied. Applying it would
// resurrect a change the transaction never committed.
TEST_F(RapidWalTest, UncommittedRowPrepareIsNotApplied) {
  std::vector<WalCell> cells(1);
  cells[0].col_id = 0;
  cells[0].value = Bytes("never-committed");

  uint32_t op_crc = 0;
  ASSERT_GT(m_mgr->log_row_prepare(kImcu, 9, kTxn, kScn, WAL_MUT_INSERT, cells, &op_crc), 0u);
  ASSERT_TRUE(m_mgr->sync());

  std::vector<WalRecord> got;
  auto res = Replay(m_mgr.get(), &got);
  EXPECT_EQ(ErrorCode::OK, res.error);
  EXPECT_EQ(0u, res.value);
  EXPECT_TRUE(got.empty());
}

// A committed prepare that follows an uncommitted one still has to be applied:
// the abandoned operation must not swallow the rest of the log.
TEST_F(RapidWalTest, UncommittedPrepareDoesNotBlockLaterOperations) {
  std::vector<WalCell> cells(1);
  cells[0].col_id = 0;
  cells[0].value = Bytes("orphan");
  uint32_t crc_a = 0;
  ASSERT_GT(m_mgr->log_row_prepare(kImcu, 1, kTxn, kScn, WAL_MUT_INSERT, cells, &crc_a), 0u);

  cells[0].value = Bytes("committed");
  uint32_t crc_b = 0;
  const uint64_t op_b = m_mgr->log_row_prepare(kImcu, 2, kTxn, kScn, WAL_MUT_INSERT, cells, &crc_b);
  ASSERT_GT(op_b, 0u);
  ASSERT_GT(m_mgr->log_row_commit(op_b, kImcu, 1, crc_b), 0u);

  const auto got = ReplayExpectOk(m_mgr.get());
  ASSERT_EQ(1u, got.size());
  EXPECT_EQ(2u, got[0].row_id);
  EXPECT_EQ(Bytes("committed"), got[0].cells[0].value);
}

// The commit marker carries a digest of the prepare it commits. A commit whose
// cell count does not describe the prepare means the two records do not belong
// together, and pairing them anyway would apply the wrong redo.
TEST_F(RapidWalTest, CommitDigestMismatchAbortsRecovery) {
  std::vector<WalCell> cells(2);
  cells[0].col_id = 0;
  cells[0].value = Bytes("a");
  cells[1].col_id = 1;
  cells[1].value = Bytes("b");

  uint32_t op_crc = 0;
  const uint64_t op_id = m_mgr->log_row_prepare(kImcu, 3, kTxn, kScn, WAL_MUT_INSERT, cells, &op_crc);
  ASSERT_GT(op_id, 0u);
  // Claim one cell for a two-cell prepare.
  ASSERT_GT(m_mgr->log_row_commit(op_id, kImcu, 1, op_crc), 0u);

  std::vector<WalRecord> got;
  auto res = Replay(m_mgr.get(), &got);
  EXPECT_EQ(ErrorCode::CORRUPTION, res.error);
  EXPECT_TRUE(got.empty());
}

// A commit with no prepare in front of it means the log lost records. Treating
// it as a no-op would complete recovery over a hole.
TEST_F(RapidWalTest, CommitWithoutPrepareAbortsRecovery) {
  ASSERT_GT(m_mgr->log_row_commit(/*op_id=*/12345, kImcu, 1, /*operation_crc=*/0), 0u);

  std::vector<WalRecord> got;
  auto res = Replay(m_mgr.get(), &got);
  EXPECT_EQ(ErrorCode::CORRUPTION, res.error);
}

// ------------------------------------------------------- rolled-back txns

// An abort record has to survive a round trip through the log at all.
//
// Imcu commits its row operations to the WAL per statement, before the host
// InnoDB transaction has decided anything, so the log on its own claims every
// one of them happened. log_abort() is the compensation. It was written
// through the legacy single-cell layout but read_record() had no case for
// OP_ABORT and fell through to `default: return BAD_MAGIC`, so the first
// rollback a table ever performed made its whole WAL unreadable: the abort
// pre-scan stopped at that record, the main replay stopped there too, and
// recovery reported CORRUPTION and gave up the fast lane entirely.
TEST_F(RapidWalTest, AbortRecordDoesNotPoisonTheLog) {
  const auto v = Bytes("committed-before-the-abort");
  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 10, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->log_abort(kTxn + 1));  // some OTHER transaction rolled back
  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 11, kTxn, kScn, v.data(), v.size()));

  const auto got = ReplayExpectOk(m_mgr.get());
  // The abort marker itself is not a mutation, so it is not delivered; the two
  // records around it are, and neither may be lost to it.
  ASSERT_EQ(2u, got.size());
  EXPECT_EQ(10u, got[0].row_id);
  EXPECT_EQ(11u, got[1].row_id);
}

// Everything an aborted transaction wrote is void, whether it was logged as a
// legacy single-cell record or as a prepare/commit pair, and whether it was
// written before or after the abort marker.
TEST_F(RapidWalTest, AbortedTransactionIsNotReplayed) {
  constexpr uint64_t kGoodTxn = kTxn;
  constexpr uint64_t kBadTxn = kTxn + 1;

  const auto keep = Bytes("keep");
  const auto drop = Bytes("drop");

  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 1, kGoodTxn, kScn, keep.data(), keep.size()));
  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 2, kBadTxn, kScn, drop.data(), drop.size()));

  std::vector<WalCell> cells(1);
  cells[0].col_id = 0;
  cells[0].value = drop;
  uint32_t crc = 0;
  const uint64_t op_id = m_mgr->log_row_prepare(kImcu, 3, kBadTxn, kScn, WAL_MUT_INSERT, cells, &crc);
  ASSERT_GT(op_id, 0u);
  ASSERT_GT(m_mgr->log_row_commit(op_id, kImcu, 1, crc), 0u);

  // The abort lands AFTER the operations it cancels -- which is why recover()
  // needs a pre-scan and cannot decide this in one forward pass.
  ASSERT_TRUE(m_mgr->log_abort(kBadTxn));

  const auto got = ReplayExpectOk(m_mgr.get());
  ASSERT_EQ(1u, got.size());
  EXPECT_EQ(1u, got[0].row_id);
  EXPECT_EQ(kGoodTxn, got[0].txn_id);
}

// The abort must still be honoured after the log has been closed and reopened:
// it is durable state, not something recover() only knows within one session.
TEST_F(RapidWalTest, AbortSurvivesAReopen) {
  const auto v = Bytes("rolled-back");
  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 42, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->log_abort(kTxn));
  m_mgr->close();

  auto reopened = MakeManager();
  ASSERT_TRUE(reopened->open());
  std::vector<WalRecord> got;
  const auto res = Replay(reopened.get(), &got);
  EXPECT_EQ(ErrorCode::OK, res.error);
  EXPECT_TRUE(got.empty());
  reopened->close();
}

// --------------------------------------------------------- damaged log files

// A crash in the middle of an append leaves a partial record at the very end.
// That is expected, and open() repairs it by dropping the torn tail; every
// intact record before it must survive.
TEST_F(RapidWalTest, TornTailIsRepairedAndEarlierRecordsSurvive) {
  const auto v = Bytes("durable");
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(m_mgr->log_write(kImcu, 0, i, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->sync());
  m_mgr->close();

  // Append the first few bytes of a fourth record and stop, as a killed
  // process would.
  {
    std::ofstream f(WalPath(), std::ios::binary | std::ios::app);
    ASSERT_TRUE(f.is_open());
    const char partial[] = {'L', 'W', 'A', 'L', 0x07, 0x00};
    f.write(partial, sizeof(partial));
  }
  const uint64_t torn_size = WalSize();

  auto reopened = MakeManager();
  ASSERT_TRUE(reopened->open());
  EXPECT_LT(WalSize(), torn_size) << "open() must truncate the torn tail";
  EXPECT_EQ(4u, reopened->written_lsn());

  const auto got = ReplayExpectOk(reopened.get());
  EXPECT_EQ(3u, got.size());
  reopened->close();
}

// Corruption anywhere but the tail is not recoverable. Reporting success over
// a partial replay would leave Rapid quietly disagreeing with InnoDB, so both
// open() and recover() have to refuse.
TEST_F(RapidWalTest, MidLogCorruptionIsFatal) {
  const auto v = Bytes("first");
  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 1, kTxn, kScn, v.data(), v.size()));
  const uint64_t first_len = WalSize();
  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 2, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 3, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->sync());
  m_mgr->close();

  // Corrupt the LSN field of the second record: its magic and its length
  // fields still parse, so the record is read to the end and only the CRC
  // disagrees -- the case that is indistinguishable from a clean log unless
  // the checksum is actually verified.
  PokeWal(static_cast<std::streamoff>(first_len) + 4, 4, '\x5A');

  auto reopened = MakeManager();
  std::vector<WalRecord> got;
  auto res = Replay(reopened.get(), &got);
  EXPECT_EQ(ErrorCode::CORRUPTION, res.error);

  EXPECT_FALSE(reopened->open()) << "a corrupt WAL must not be reopened for append";
}

// A wrecked record header (bad magic) is corruption too, not end-of-log.
TEST_F(RapidWalTest, BadMagicMidLogIsFatal) {
  const auto v = Bytes("first");
  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 1, kTxn, kScn, v.data(), v.size()));
  const uint64_t first_len = WalSize();
  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 2, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->sync());
  m_mgr->close();

  PokeWal(static_cast<std::streamoff>(first_len), 4, '\x00');

  auto reopened = MakeManager();
  std::vector<WalRecord> got;
  auto res = Replay(reopened.get(), &got);
  EXPECT_EQ(ErrorCode::CORRUPTION, res.error);
}

// An apply failure is a recovery failure. Returning OK with a short count
// would present a half-restored table as fully recovered.
TEST_F(RapidWalTest, ApplyFailureAbortsRecovery) {
  const auto v = Bytes("row");
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(m_mgr->log_write(kImcu, 0, i, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->sync());

  size_t seen = 0;
  Imcu imcu;
  std::vector<Imcu *> imcus{&imcu};
  auto res = m_mgr->recover(imcus, [&seen](const WalRecord &) {
    return (++seen == 3) ? ErrorCode::IO_ERROR : ErrorCode::OK;
  });

  EXPECT_EQ(ErrorCode::IO_ERROR, res.error);
  EXPECT_EQ(2u, res.value) << "only the records applied before the failure count";
  EXPECT_EQ(3u, seen) << "recovery must stop at the first failure";
}

// A cold-start IMCU has no owner, so recover() must not compare the table id it
// cannot read against the manifest's.
TEST_F(RapidWalTest, ManifestIsNotComparedAgainstAnOwnerlessImcu) {
  Imcu imcu;  // id 0, no owner: the shape recover() sees on a cold start.
  EXPECT_EQ(nullptr, imcu.owner()) << "a cold-start IMCU owns no table";

  EnsureCheckpointDirs();
  RecoveryManifest manifest;
  manifest.table_id = 1;
  manifest.generation = 1;
  ManifestImcuEntry entry;
  entry.imcu_id = kImcu;
  entry.state = ManifestImcuState::NEVER_CHECKPOINTED;
  manifest.imcus.push_back(entry);
  ASSERT_TRUE(m_mgr->persist_manifest(manifest));

  std::vector<Imcu *> imcus{&imcu};
  const auto res = m_mgr->recover(imcus, [](const WalRecord &) { return ErrorCode::OK; });

  // No owner means no table id to compare, so the identity check is skipped.
  EXPECT_EQ(ErrorCode::OK, res.error);
  EXPECT_EQ(0u, res.value);
}

// Group commit. Concurrent writers must share flushes, and each must still see
// its own record durable when it returns -- the point of the change is fewer
// flushes, not weaker durability.
TEST_F(RapidWalTest, ConcurrentDurabilityWaitsShareOneFlush) {
  constexpr int kWriters = 8;
  std::vector<uint64_t> op_ids(kWriters, 0);
  // Spin barrier: every writer appends before any of them asks for durability,
  // so the first leader's flush covers all of them and the count is exact.
  std::atomic<int> phase{0};

  auto wait_for = [&phase](int target) {
    while (phase.load(std::memory_order_acquire) < target) std::this_thread::yield();
  };

  const uint64_t flushes_before = m_mgr->flush_count();
  std::vector<std::thread> writers;
  for (int t = 0; t < kWriters; ++t) {
    writers.emplace_back([&, t]() {
      wait_for(1);
      const auto v = Bytes("row");
      WalCell cell;
      cell.col_id = 0;
      cell.value = v;
      op_ids[t] = m_mgr->log_row_prepare(kImcu, static_cast<uint64_t>(t), kTxn, kScn, WAL_MUT_INSERT, {cell}, nullptr);
      phase.fetch_add(1, std::memory_order_acq_rel);
      wait_for(kWriters + 1);
      EXPECT_TRUE(m_mgr->wait_durable(op_ids[t]));
    });
  }
  phase.store(1, std::memory_order_release);
  // phase reaches kWriters + 1 only once every writer has appended.
  wait_for(kWriters + 1);
  for (auto &w : writers) w.join();

  uint64_t max_op_id = 0;
  for (uint64_t op_id : op_ids) {
    ASSERT_GT(op_id, 0u) << "every prepare got an LSN";
    max_op_id = std::max(max_op_id, op_id);
  }
  EXPECT_EQ(1u, m_mgr->flush_count() - flushes_before)
      << "one flush must cover every writer that was waiting in the same window";
  EXPECT_GE(m_mgr->durable_lsn(), max_op_id) << "the shared flush advanced durability past every record";
}

// An LSN that is already durable must not force another flush: the hot path
// calls this once per row, and re-flushing would put the disk back in it.
TEST_F(RapidWalTest, DurabilityWaitDoesNotFlushWhatIsAlreadyDurable) {
  WalCell cell;
  cell.col_id = 0;
  const auto v = Bytes("x");
  cell.value = v;
  const uint64_t op_id = m_mgr->log_row_prepare(kImcu, 0, kTxn, kScn, WAL_MUT_INSERT, {cell}, nullptr);
  ASSERT_GT(op_id, 0u);
  ASSERT_TRUE(m_mgr->wait_durable(op_id));

  const uint64_t flushes = m_mgr->flush_count();
  EXPECT_TRUE(m_mgr->wait_durable(op_id));
  EXPECT_EQ(flushes, m_mgr->flush_count()) << "an already-durable LSN must not re-flush";
  EXPECT_GE(m_mgr->durable_lsn(), op_id);
}

// A durability wait with no WAL to flush must fail rather than report success.
TEST_F(RapidWalTest, DurabilityWaitOnAClosedWalFails) {
  WalCell cell;
  cell.col_id = 0;
  const auto v = Bytes("x");
  cell.value = v;
  const uint64_t op_id = m_mgr->log_row_prepare(kImcu, 0, kTxn, kScn, WAL_MUT_INSERT, {cell}, nullptr);
  ASSERT_GT(op_id, 0u);

  m_mgr->close();
  EXPECT_FALSE(m_mgr->wait_durable(op_id)) << "no WAL means the record cannot be made durable";
  // Nothing was written, so this is a plain failure, not an unknown outcome.
  EXPECT_FALSE(m_mgr->recovery_required()) << "a closed WAL is not a failed flush";
}

// ------------------------------------------------------------------- WAL GC

// Without a durable checkpoint no WAL prefix is provably redundant, so GC must
// keep everything however high the caller's LSN is.
TEST_F(RapidWalTest, TruncateKeepsEverythingWithoutACheckpoint) {
  const auto v = Bytes("keep-me");
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(m_mgr->log_write(kImcu, 0, i, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->sync());

  ASSERT_TRUE(m_mgr->truncate_wal(/*up_to_lsn=*/1000));

  const auto got = ReplayExpectOk(m_mgr.get());
  EXPECT_EQ(4u, got.size()) << "no checkpoint exists, so nothing may be discarded";
}

// With a checkpoint that covers LSNs below the manifest's base, the prefix is
// redundant and may go -- but never past the base, and never past what the
// caller asked for.
TEST_F(RapidWalTest, TruncateStopsAtTheCheckpointFrontier) {
  const auto v = Bytes("x");
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(m_mgr->log_write(kImcu, 0, i, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->sync());

  EnsureCheckpointDirs();
  RecoveryManifest manifest;
  manifest.table_id = 1;
  manifest.generation = 1;
  manifest.wal_base_lsn = 3;  // records 1..2 are covered by the snapshot
  ManifestImcuEntry entry;
  entry.imcu_id = kImcu;
  entry.state = ManifestImcuState::NEVER_CHECKPOINTED;
  entry.snapshot_next_lsn = 3;
  manifest.imcus.push_back(entry);
  ASSERT_TRUE(m_mgr->persist_manifest(manifest));

  // Asking to drop everything must still stop at the frontier.
  ASSERT_TRUE(m_mgr->truncate_wal(/*up_to_lsn=*/1000));

  const auto got = ReplayExpectOk(m_mgr.get());
  ASSERT_EQ(4u, got.size());
  EXPECT_EQ(3u, got.front().lsn) << "records below the checkpoint frontier are redundant";
  EXPECT_EQ(6u, got.back().lsn);
}

// Truncation rewrites the file; the next append must continue above the
// highest surviving LSN rather than restart inside the kept range.
TEST_F(RapidWalTest, AppendsAfterTruncationKeepAscendingLsns) {
  const auto v = Bytes("x");
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(m_mgr->log_write(kImcu, 0, i, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->sync());
  ASSERT_TRUE(m_mgr->truncate_wal(1000));

  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 42, kTxn, kScn, v.data(), v.size()));
  const auto got = ReplayExpectOk(m_mgr.get());
  ASSERT_EQ(4u, got.size());
  EXPECT_EQ(4u, got.back().lsn);
  EXPECT_EQ(42u, got.back().row_id);
}

// ------------------------------------------------------------------ durable fs

/**
 * create_directories() must fsync the directories it creates: otherwise a crash
 * can leave the WAL's own directory missing while every file written into it
 * was durable.
 *
 * The fsync itself is not observable without crashing the machine, so these
 * pin the walk around it -- full chain created, partial chains, idempotence.
 */
class RapidDurableFsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static int seq = 0;
    m_root = fs::temp_directory_path() /
             ("rapid_durfs_ut_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(seq++));
    std::error_code ec;
    fs::remove_all(m_root, ec);
    ASSERT_TRUE(fs::create_directories(m_root, ec)) << ec.message();
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(m_root, ec);
  }
  fs::path m_root;
};

TEST_F(RapidDurableFsTest, CreatesAndSyncsAWholeMissingChain) {
  const fs::path deep = m_root / "a" / "b" / "c" / "d";
  ASSERT_FALSE(fs::exists(deep));

  EXPECT_TRUE(ShannonBase::Recovery::DurableFileSystem::create_directories(deep));

  // Every level exists, not only the deepest one.
  EXPECT_TRUE(fs::is_directory(m_root / "a"));
  EXPECT_TRUE(fs::is_directory(m_root / "a" / "b"));
  EXPECT_TRUE(fs::is_directory(m_root / "a" / "b" / "c"));
  EXPECT_TRUE(fs::is_directory(deep));
}

TEST_F(RapidDurableFsTest, IsIdempotentAndHandlesPartialChains) {
  const fs::path deep = m_root / "x" / "y" / "z";
  ASSERT_TRUE(ShannonBase::Recovery::DurableFileSystem::create_directories(deep));

  // Nothing missing: the early-out must still report success.
  EXPECT_TRUE(ShannonBase::Recovery::DurableFileSystem::create_directories(deep));
  EXPECT_TRUE(ShannonBase::Recovery::DurableFileSystem::create_directories(m_root / "x"));

  // Only the tail missing: the walk stops at the first existing ancestor.
  const fs::path extended = deep / "w" / "v";
  EXPECT_TRUE(ShannonBase::Recovery::DurableFileSystem::create_directories(extended));
  EXPECT_TRUE(fs::is_directory(extended));
}

TEST_F(RapidDurableFsTest, WrittenFilesSurviveInTheCreatedDirectories) {
  // Exercise the pairing that matters: a file written into the new chain.
  const fs::path dir = m_root / "p" / "q";
  ASSERT_TRUE(ShannonBase::Recovery::DurableFileSystem::create_directories(dir));

  const fs::path file = dir / "payload";
  ASSERT_TRUE(ShannonBase::Recovery::DurableFileSystem::persist_file(file, std::string("contents")));

  std::ifstream in(file, std::ios::binary);
  ASSERT_TRUE(in.is_open());
  std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ("contents", got);
}

// ---------------------------------------------------------------- epoch reset

/**
 * The slow recovery lane calls reset_epoch() before rebuilding from InnoDB,
 * which renumbers every row: the WAL and every generation on disk describe the
 * old layout and must go.
 *
 * open() alone is not enough -- it resumes written_lsn above the old high-water
 * mark, so the post-reload checkpoint publishes wal_base_lsn = 1 and
 * truncate_wal(1) keeps every stale record.
 */
TEST_F(RapidWalTest, ResetEpochDropsTheWalAndRestartsLsns) {
  const auto v = Bytes("pre-reload");
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(m_mgr->log_write(kImcu, 0, i, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->sync());
  ASSERT_EQ(6u, m_mgr->written_lsn());
  ASSERT_GT(WalSize(), 0u);

  ASSERT_TRUE(m_mgr->reset_epoch());

  // A fresh epoch hands out LSN 1 again, exactly as a never-used manager does.
  EXPECT_EQ(1u, m_mgr->written_lsn());
  EXPECT_EQ(0u, m_mgr->durable_lsn());
  EXPECT_EQ(0u, m_mgr->applied_lsn());
  EXPECT_EQ(0u, WalSize()) << "the previous epoch's records are still on disk";

  EXPECT_TRUE(ReplayExpectOk(m_mgr.get()).empty()) << "a reset WAL replayed records from the old layout";

  // And the log is usable again from LSN 1.
  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 77, kTxn, kScn, v.data(), v.size()));
  const auto got = ReplayExpectOk(m_mgr.get());
  ASSERT_EQ(1u, got.size());
  EXPECT_EQ(1u, got.front().lsn);
  EXPECT_EQ(77u, got.front().row_id);
}

/**
 * A surviving manifest would pin truncate_wal()'s frontier at the old base LSN,
 * and the next restart could restore rows the rebuilt table no longer has.
 */
TEST_F(RapidWalTest, ResetEpochRemovesEveryCheckpointGeneration) {
  EnsureCheckpointDirs();
  for (uint64_t gen = 1; gen <= 3; ++gen) {
    RecoveryManifest m;
    m.table_id = 17;
    m.generation = gen;
    m.wal_base_lsn = gen * 10;
    ManifestImcuEntry e;
    e.imcu_id = kImcu;
    e.state = ManifestImcuState::NEVER_CHECKPOINTED;
    e.snapshot_next_lsn = gen * 10;
    m.imcus.push_back(e);
    ASSERT_TRUE(m_mgr->persist_manifest(m));
  }
  ASSERT_EQ(3u, m_mgr->latest_generation());
  ASSERT_EQ(3u, m_mgr->list_manifest_generations().size());

  ASSERT_TRUE(m_mgr->reset_epoch());

  EXPECT_TRUE(m_mgr->list_manifest_generations().empty()) << "a stale generation outlived the epoch";
  EXPECT_EQ(0u, m_mgr->latest_generation());
  EXPECT_FALSE(m_mgr->load_manifest(3).ok()) << "the old manifest is still loadable";
}

// DBUG_EXECUTE_IF is compiled out when NDEBUG is set (include/my_dbug.h defines
// the no-op form in its NDEBUG branch), so this test is only meaningful in a
// build that keeps the hooks alive. DBUG_OFF did not express that: nothing in
// the tree defines it, so the guard was always true and the test compiled and
// ran in the Release CI build, where the hook it arms does nothing and every
// expectation below fails.
#if !defined(NDEBUG)
class ScopedPowerCut {
 public:
  ScopedPowerCut() { DBUG_PUSH("+d,rapid_simulate_power_loss"); }
  ~ScopedPowerCut() { DBUG_POP(); }
};

// close() does not flush the fd-backed WAL. Reopening with the cut armed is
// deterministic even though the operating system still has the appended bytes.
TEST_F(RapidWalTest, PowerCutDropsPrepareBeforeFirstFlush) {
  ScopedPowerCut cut;
  ASSERT_GT(m_mgr->log_row_prepare(kImcu, 9, kTxn, kScn, WAL_MUT_DELETE, {}), 0u);
  ASSERT_GT(WalSize(), 0u);
  m_mgr->close();
  auto reopened = MakeManager();
  ASSERT_TRUE(reopened->open());
  EXPECT_EQ(0u, WalSize());
  EXPECT_TRUE(ReplayExpectOk(reopened.get()).empty());
}

TEST_F(RapidWalTest, PowerCutKeepsDurablePrepareButDoesNotReplayIt) {
  ScopedPowerCut cut;
  const auto op = m_mgr->log_row_prepare(kImcu, 9, kTxn, kScn, WAL_MUT_DELETE, {});
  ASSERT_GT(op, 0u);
  ASSERT_TRUE(m_mgr->wait_durable(op));
  const auto durable_size = WalSize();
  m_mgr->close();
  auto reopened = MakeManager();
  ASSERT_TRUE(reopened->open());
  EXPECT_EQ(durable_size, WalSize());
  EXPECT_TRUE(ReplayExpectOk(reopened.get()).empty());
}

TEST_F(RapidWalTest, PowerCutReplaysCommitFlushedWithInjectionDisabled) {
  ScopedPowerCut cut;
  uint32_t crc = 0;
  auto op = m_mgr->log_row_prepare(kImcu, 10, kTxn, kScn, WAL_MUT_DELETE, {}, &crc);
  ASSERT_GT(op, 0u);
  ASSERT_GT(m_mgr->log_row_commit(op, kImcu, 0, crc), 0u);
  DBUG_SET("-d,rapid_simulate_power_loss");
  op = m_mgr->log_row_prepare(kImcu, 11, kTxn, kScn, WAL_MUT_DELETE, {}, &crc);
  ASSERT_GT(op, 0u);
  ASSERT_GT(m_mgr->log_row_commit(op, kImcu, 0, crc), 0u);
  const auto durable_size = WalSize();
  ASSERT_GT(m_mgr->log_row_prepare(kImcu, 12, kTxn, kScn, WAL_MUT_DELETE, {}), 0u);
  ASSERT_GT(WalSize(), durable_size);
  m_mgr->close();
  DBUG_SET("+d,rapid_simulate_power_loss");
  auto reopened = MakeManager();
  ASSERT_TRUE(reopened->open());
  EXPECT_EQ(durable_size, WalSize());
  const auto got = ReplayExpectOk(reopened.get());
  ASSERT_EQ(2u, got.size());
  EXPECT_EQ(10u, got[0].row_id);
  EXPECT_EQ(11u, got[1].row_id);
}

TEST_F(RapidWalTest, PowerCutKeepsFlushFromUnarmedThread) {
  ScopedPowerCut cut;
  ASSERT_GT(m_mgr->log_delete(kImcu, 0, 10, kTxn, kScn), 0u);
  ASSERT_TRUE(m_mgr->sync());
  ASSERT_GT(m_mgr->log_delete(kImcu, 0, 11, kTxn, kScn), 0u);
  bool synced = false;
  std::thread flusher([&] {
    DBUG_PUSH("");
    DBUG_SET("-d,rapid_simulate_power_loss");
    synced = m_mgr->sync();
    DBUG_POP();
  });
  flusher.join();
  ASSERT_TRUE(synced);
  const auto durable_size = WalSize();
  ASSERT_GT(m_mgr->log_row_prepare(kImcu, 12, kTxn, kScn, WAL_MUT_DELETE, {}), 0u);
  m_mgr->close();
  auto reopened = MakeManager();
  ASSERT_TRUE(reopened->open());
  EXPECT_EQ(durable_size, WalSize());
  const auto got = ReplayExpectOk(reopened.get());
  ASSERT_EQ(2u, got.size());
  EXPECT_EQ(11u, got.back().row_id);
}

TEST_F(RapidWalTest, PowerCutDropsFirstPrepareAfterEpochReset) {
  ScopedPowerCut cut;
  ASSERT_GT(m_mgr->log_delete(kImcu, 0, 10, kTxn, kScn), 0u);
  ASSERT_TRUE(m_mgr->sync());
  ASSERT_TRUE(m_mgr->reset_epoch());
  ASSERT_GT(m_mgr->log_row_prepare(kImcu, 11, kTxn, kScn, WAL_MUT_DELETE, {}), 0u);
  m_mgr->close();
  auto reopened = MakeManager();
  ASSERT_TRUE(reopened->open());
  EXPECT_EQ(0u, WalSize());
  EXPECT_TRUE(ReplayExpectOk(reopened.get()).empty());
}

TEST_F(RapidWalTest, PowerCutKeepsRewrittenWalAndDropsItsUnsyncedTail) {
  ScopedPowerCut cut;
  for (uint64_t row = 1; row <= 3; ++row)
    ASSERT_GT(m_mgr->log_delete(kImcu, 0, row, kTxn, kScn), 0u);
  ASSERT_TRUE(m_mgr->sync());
  EnsureCheckpointDirs();
  RecoveryManifest manifest;
  manifest.generation = 1;
  manifest.wal_base_lsn = 3;
  ASSERT_TRUE(m_mgr->persist_manifest(manifest));
  ASSERT_TRUE(m_mgr->truncate_wal(3));
  const auto durable_size = WalSize();
  ASSERT_GT(durable_size, 0u);
  ASSERT_GT(m_mgr->log_row_prepare(kImcu, 4, kTxn, kScn, WAL_MUT_DELETE, {}), 0u);
  m_mgr->close();
  auto reopened = MakeManager();
  ASSERT_TRUE(reopened->open());
  EXPECT_EQ(durable_size, WalSize());
  const auto got = ReplayExpectOk(reopened.get());
  ASSERT_EQ(1u, got.size());
  EXPECT_EQ(3u, got[0].row_id);
}

TEST_F(RapidWalTest, PowerCutDropsFirstPrepareAfterEmptyRewrite) {
  ScopedPowerCut cut;
  ASSERT_TRUE(m_mgr->truncate_wal(1));
  ASSERT_GT(m_mgr->log_row_prepare(kImcu, 9, kTxn, kScn, WAL_MUT_DELETE, {}), 0u);
  m_mgr->close();
  auto reopened = MakeManager();
  ASSERT_TRUE(reopened->open());
  EXPECT_EQ(0u, WalSize());
}

TEST_F(RapidWalTest, PowerCutRefusesMissingBoundaryForNonemptyWal) {
  ScopedPowerCut cut;
  ASSERT_GT(m_mgr->log_delete(kImcu, 0, 10, kTxn, kScn), 0u);
  ASSERT_TRUE(m_mgr->sync());
  const auto size = WalSize();
  m_mgr->close();
  ASSERT_TRUE(fs::remove(WalPath().parent_path() / "cu_wal.durable"));
  auto reopened = MakeManager();
  EXPECT_FALSE(reopened->open());
  EXPECT_EQ(size, WalSize());
}

TEST_F(RapidWalTest, PowerCutRefusesInvalidBoundaryWithoutChangingWal) {
  ScopedPowerCut cut;
  ASSERT_GT(m_mgr->log_delete(kImcu, 0, 10, kTxn, kScn), 0u);
  ASSERT_TRUE(m_mgr->sync());
  const auto size = WalSize();
  m_mgr->close();
  const uint64_t beyond_end = size + 1;
  const std::string oversized(reinterpret_cast<const char *>(&beyond_end), sizeof(beyond_end));
  for (const auto &payload : {std::string("short"), oversized, oversized + "extra"}) {
    ASSERT_TRUE(ShannonBase::Recovery::DurableFileSystem::write_file(
        WalPath().parent_path() / "cu_wal.durable", payload));
    auto reopened = MakeManager();
    EXPECT_FALSE(reopened->open());
    EXPECT_EQ(size, WalSize());
  }
}

TEST_F(RapidWalTest, MarkerWriteFailureInvalidatesBoundaryAndRefusesWrites) {
  ScopedPowerCut cut;
  ASSERT_GT(m_mgr->log_delete(kImcu, 0, 10, kTxn, kScn), 0u);
  ASSERT_TRUE(m_mgr->sync());
  const auto marker = WalPath().parent_path() / "cu_wal.durable";
  ASSERT_TRUE(fs::remove(marker));
  ASSERT_TRUE(fs::create_directory(marker));  // atomic replacement must fail
  ASSERT_GT(m_mgr->log_delete(kImcu, 0, 11, kTxn, kScn), 0u);
  EXPECT_FALSE(m_mgr->sync());
  EXPECT_TRUE(m_mgr->recovery_required());
  EXPECT_EQ(0u, m_mgr->log_delete(kImcu, 0, 12, kTxn, kScn));
  m_mgr->close();
  auto reopened = MakeManager();
  EXPECT_FALSE(reopened->open());
}

/** A failed manifest removal must fail closed instead of reloading over an old epoch. */
TEST_F(RapidWalTest, ResetEpochFailsClosedWhenAStaleGenerationCannotBeRemoved) {
  EnsureCheckpointDirs();
  RecoveryManifest manifest;
  manifest.table_id = 17;
  manifest.generation = 1;
  manifest.wal_base_lsn = 1;
  ManifestImcuEntry entry;
  entry.imcu_id = kImcu;
  entry.state = ManifestImcuState::NEVER_CHECKPOINTED;
  entry.snapshot_next_lsn = 1;
  manifest.imcus.push_back(entry);
  ASSERT_TRUE(m_mgr->persist_manifest(manifest));

  DBUG_SET("+d,rapid_reset_epoch_remove_generation_fail");
  const bool reset_succeeded = m_mgr->reset_epoch();
  DBUG_SET("");

  EXPECT_FALSE(reset_succeeded);
  EXPECT_TRUE(m_mgr->recovery_required());
  EXPECT_TRUE(m_mgr->load_manifest(1).ok()) << "the injected failure should leave the old manifest intact";
  const auto value = Bytes("must-not-append");
  EXPECT_FALSE(m_mgr->log_write(kImcu, 0, 0, kTxn, kScn, value.data(), value.size()));
}
#endif

/** End-to-end shape: reload, checkpoint, truncate. Without the reset,
 *  truncate_wal() computed a frontier of 1 and kept the stale prefix. */
TEST_F(RapidWalTest, ResetEpochLetsTheNextCheckpointTruncateTheOldRecords) {
  const auto v = Bytes("stale");
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(m_mgr->log_write(kImcu, 0, i, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->sync());

  // The reload.
  ASSERT_TRUE(m_mgr->reset_epoch());

  // Post-reload DML, then the checkpoint the reload schedules.
  const auto fresh = Bytes("fresh");
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 100 + i, kTxn, kScn, fresh.data(), fresh.size()));
  ASSERT_TRUE(m_mgr->sync());

  EnsureCheckpointDirs();
  RecoveryManifest m;
  m.table_id = 17;
  m.generation = 1;
  m.wal_base_lsn = 3;  // the snapshot covers the new epoch's LSNs 1..2
  ManifestImcuEntry e;
  e.imcu_id = kImcu;
  e.state = ManifestImcuState::NEVER_CHECKPOINTED;
  e.snapshot_next_lsn = 3;
  m.imcus.push_back(e);
  ASSERT_TRUE(m_mgr->persist_manifest(m));
  ASSERT_TRUE(m_mgr->truncate_wal(1000));

  const auto got = ReplayExpectOk(m_mgr.get());
  ASSERT_EQ(1u, got.size()) << "records from the previous epoch survived the checkpoint";
  EXPECT_EQ(3u, got.front().lsn);
  EXPECT_EQ(102u, got.front().row_id) << "a surviving record belongs to the old layout";
}

/** The reset must be durable: a reopen has to agree with the in-memory state. */
TEST_F(RapidWalTest, ResetEpochSurvivesAReopen) {
  const auto v = Bytes("pre-reload");
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(m_mgr->log_write(kImcu, 0, i, kTxn, kScn, v.data(), v.size()));
  ASSERT_TRUE(m_mgr->sync());
  ASSERT_TRUE(m_mgr->reset_epoch());
  m_mgr->close();

  auto reopened = MakeManager();
  ASSERT_TRUE(reopened->open());
  EXPECT_EQ(1u, reopened->written_lsn()) << "a reopen resumed the previous epoch's LSN counter";
  EXPECT_TRUE(ReplayExpectOk(reopened.get()).empty());
  reopened->close();
}

// ------------------------------------------------------------ manifest state

TEST_F(RapidWalTest, ManifestRoundTripsAndTracksGenerations) {
  EXPECT_EQ(0u, m_mgr->latest_generation());
  EXPECT_TRUE(m_mgr->list_manifest_generations().empty());
  EnsureCheckpointDirs();

  RecoveryManifest m1;
  m1.table_id = 17;
  m1.generation = 1;
  m1.schema_fingerprint = 0xABCDEF;
  m1.wal_base_lsn = 5;
  ManifestImcuEntry e;
  e.imcu_id = 0;
  e.state = ManifestImcuState::CHECKPOINTED;
  e.snapshot_next_lsn = 5;
  e.snapshot_size = 1024;
  e.snapshot_crc = 0x1234;
  e.snapshot_file = "imcu_0.snap";
  m1.imcus.push_back(e);
  ASSERT_TRUE(m_mgr->persist_manifest(m1));

  RecoveryManifest m2 = m1;
  m2.generation = 2;
  m2.wal_base_lsn = 9;
  m2.imcus[0].snapshot_next_lsn = 9;
  ASSERT_TRUE(m_mgr->persist_manifest(m2));

  EXPECT_EQ(2u, m_mgr->latest_generation());
  EXPECT_EQ((std::vector<uint64_t>{1, 2}), m_mgr->list_manifest_generations());

  auto loaded = m_mgr->load_manifest(1);
  ASSERT_EQ(ErrorCode::OK, loaded.error);
  EXPECT_EQ(17u, loaded.value.table_id);
  EXPECT_EQ(1u, loaded.value.generation);
  EXPECT_EQ(0xABCDEFu, loaded.value.schema_fingerprint);
  EXPECT_EQ(5u, loaded.value.wal_base_lsn);
  ASSERT_EQ(1u, loaded.value.imcus.size());
  EXPECT_EQ(ManifestImcuState::CHECKPOINTED, loaded.value.imcus[0].state);
  EXPECT_EQ(1024u, loaded.value.imcus[0].snapshot_size);
  EXPECT_EQ(0x1234u, loaded.value.imcus[0].snapshot_crc);

  m_mgr->remove_generation(1);
  EXPECT_EQ((std::vector<uint64_t>{2}), m_mgr->list_manifest_generations());
  EXPECT_EQ(ErrorCode::NOT_FOUND, m_mgr->load_manifest(1).error);
}

// A damaged manifest must be reported, not parsed into a plausible-looking
// checkpoint that recovery would then trust.
TEST_F(RapidWalTest, CorruptManifestIsRejected) {
  EnsureCheckpointDirs();
  RecoveryManifest m;
  m.table_id = 3;
  m.generation = 1;
  m.wal_base_lsn = 2;
  ASSERT_TRUE(m_mgr->persist_manifest(m));
  ASSERT_EQ(ErrorCode::OK, m_mgr->load_manifest(1).error);

  const auto path = m_mgr->manifest_path(1);
  {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(f.is_open());
    f.seekp(0);
    const char junk[] = {'\x00', '\x00', '\x00', '\x00'};
    f.write(junk, sizeof(junk));
  }
  EXPECT_NE(ErrorCode::OK, m_mgr->load_manifest(1).error);
}

// ------------------------------------------------------------- fail-stop

// Once a commit's durability is unknown the in-memory image can no longer be
// reconciled with the log, so the table is latched into recovery-required and
// every further append is refused rather than written on top of the doubt.
TEST_F(RapidWalTest, RecoveryRequiredRefusesFurtherAppends) {
  const auto v = Bytes("before");
  ASSERT_TRUE(m_mgr->log_write(kImcu, 0, 1, kTxn, kScn, v.data(), v.size()));
  EXPECT_FALSE(m_mgr->recovery_required());

  m_mgr->require_recovery();
  EXPECT_TRUE(m_mgr->recovery_required());

  EXPECT_FALSE(m_mgr->log_write(kImcu, 0, 2, kTxn, kScn, v.data(), v.size()));
  EXPECT_FALSE(m_mgr->log_update(kImcu, 0, 2, kTxn, kScn, v.data(), v.size()));
  EXPECT_EQ(0u, m_mgr->log_delete(kImcu, 0, 2, kTxn, kScn));
  EXPECT_EQ(0u, m_mgr->log_row_prepare(kImcu, 2, kTxn, kScn, WAL_MUT_INSERT, {}, nullptr));

  // The record written before the latch is still readable.
  const auto got = ReplayExpectOk(m_mgr.get());
  EXPECT_EQ(1u, got.size());
}

// Appending without open() must fail rather than silently drop the record.
TEST_F(RapidWalTest, AppendWithoutOpenFails) {
  auto closed = MakeManager();  // never opened
  const auto v = Bytes("nope");
  EXPECT_FALSE(closed->log_write(kImcu, 0, 1, kTxn, kScn, v.data(), v.size()));
  EXPECT_EQ(0u, closed->log_delete(kImcu, 0, 1, kTxn, kScn));
}


using ShannonBase::Recovery::WAL;

#ifndef NDEBUG
TEST_F(RapidWalTest, CaptureWalTerminalAllocationFailureStaysUnresolved) {
  WAL wal(m_dir / "capture");
  ASSERT_TRUE(wal.reset());
  ASSERT_TRUE(wal.checkpoint(1));
  ASSERT_NE(0u, wal.capture(1, "source committed"));
  DBUG_SET("+d,rapid_capture_terminal_bad_alloc");
  const bool committed = wal.committed(1);
  DBUG_SET("-d,rapid_capture_terminal_bad_alloc");
  EXPECT_FALSE(committed);
  EXPECT_EQ(0u, wal.capture(2, "must refuse further writes"));
  ASSERT_TRUE(wal.open());
  EXPECT_TRUE(wal.has_unresolved_transaction());
  bool applied = false;
  EXPECT_FALSE(wal.replay(0, [&](uint64_t, uint64_t, const std::string &) {
    applied = true;
    return true;
  }));
  EXPECT_FALSE(applied);
}
#endif

namespace {
struct DeferredTestPending {
  uint64_t lsn{0};
  std::shared_ptr<int> owner;
};
using DeferredTestQueue = ShannonBase::Populate::DeferredCommitQueue<DeferredTestPending, 2>;
}  // namespace

TEST(DeferredCommitQueueTest, UnflushedWorkSleepsUntilDurableProgress) {
  DeferredTestQueue queue;
  queue.start();
  ASSERT_TRUE(queue.try_push({101, {}}));
  std::atomic<uint64_t> flushed{100};
  std::atomic<unsigned> scans{0};
  std::promise<bool> outcome;
  auto result = outcome.get_future();
  const auto started = std::chrono::steady_clock::now();
  std::thread worker([&] {
    DeferredTestPending ready;
    const bool found = queue.wait_pop(ready, [&]() -> std::optional<uint64_t> {
      ++scans;
      return flushed.load();
    });
    outcome.set_value(found && ready.lsn == 101);
  });
  EXPECT_EQ(std::future_status::timeout, result.wait_for(std::chrono::milliseconds(180)));
  EXPECT_GE(scans.load(), 1u);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  EXPECT_LE(scans.load(), static_cast<unsigned>(elapsed.count() / 50 + 2));
  flushed.store(101);
  const auto status = result.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(std::future_status::ready, status);
  queue.stop();
  worker.join();
  EXPECT_TRUE(result.get());
}

TEST(DeferredCommitQueueTest, NewDurableWorkWakesPastUnflushedEntryAndStopIsPrompt) {
  DeferredTestQueue queue;
  queue.start();
  ASSERT_TRUE(queue.try_push({200, {}}));
  std::promise<uint64_t> outcome;
  auto result = outcome.get_future();
  std::thread worker([&] {
    DeferredTestPending ready;
    queue.wait_pop(ready, [] { return std::optional<uint64_t>{100}; });
    outcome.set_value(ready.lsn);
  });
  EXPECT_TRUE(queue.try_push({100, {}}));
  EXPECT_EQ(std::future_status::ready, result.wait_for(std::chrono::seconds(2)));
  queue.stop();
  worker.join();
  EXPECT_EQ(100u, result.get());
  DeferredTestPending ready;
  EXPECT_FALSE(queue.wait_pop(ready, [] { return std::optional<uint64_t>{100}; }));
  EXPECT_FALSE(queue.try_push({100, {}}));
}

TEST(DeferredCommitQueueTest, CapacityRejectsWithoutConsumingOwnershipAndDrainReleasesSlots) {
  DeferredTestQueue queue;
  queue.start();
  auto owner = std::make_shared<int>(1);
  std::weak_ptr<int> lifetime = owner;
  ASSERT_TRUE(queue.try_push({100, owner}));
  ASSERT_TRUE(queue.try_push({101, owner}));
  DeferredTestPending rejected{102, owner};
  EXPECT_FALSE(queue.try_push(std::move(rejected)));
  EXPECT_EQ(owner, rejected.owner);
  owner.reset();
  rejected.owner.reset();
  queue.stop();
  size_t durable = 0, unknown = 0;
  queue.drain(100, [&](DeferredTestPending &, bool confirmed) { confirmed ? ++durable : ++unknown; });
  EXPECT_EQ(1u, durable);
  EXPECT_EQ(1u, unknown);
  EXPECT_TRUE(lifetime.expired());
  queue.start();
  EXPECT_TRUE(queue.try_push({100, {}}));
  EXPECT_FALSE(queue.try_push({101, {}}, 1));
  queue.stop();
  queue.drain(std::nullopt, [](DeferredTestPending &, bool confirmed) { EXPECT_FALSE(confirmed); });
}

TEST(DeferredCommitQueueTest, StopWakesNonemptyUnflushedWait) {
  DeferredTestQueue queue;
  queue.start();
  ASSERT_TRUE(queue.try_push({101, {}}));
  std::promise<void> entered;
  auto waiting = entered.get_future();
  std::promise<bool> outcome;
  auto result = outcome.get_future();
  std::thread worker([&] {
    DeferredTestPending ready;
    bool first = true;
    outcome.set_value(queue.wait_pop(ready, [&]() -> std::optional<uint64_t> {
      if (first) { entered.set_value(); first = false; }
      return std::nullopt;
    }));
  });
  EXPECT_EQ(std::future_status::ready, waiting.wait_for(std::chrono::seconds(2)));
  queue.stop();
  EXPECT_EQ(std::future_status::ready, result.wait_for(std::chrono::seconds(2)));
  worker.join();
  EXPECT_FALSE(result.get());
}

TEST_F(RapidWalTest, DeferredStopDoesNotCertifyUnflushedSourceOutcomes) {
  WAL wal(m_dir / "capture");
  ASSERT_TRUE(wal.reset());
  ASSERT_TRUE(wal.checkpoint(1));
  ASSERT_NE(0u, wal.capture(1, "durable source"));
  ASSERT_NE(0u, wal.capture(2, "unknown source"));
  struct Pending { uint64_t txn; uint64_t lsn; };
  std::deque<Pending> queue{{1, 100}, {2, 101}};
  ShannonBase::Populate::DrainDeferredCommits(queue, 100, [&](Pending &p, bool durable) {
    if (durable) { EXPECT_TRUE(wal.committed(p.txn)); }
  });
  EXPECT_TRUE(queue.empty());
  ASSERT_TRUE(wal.open());
  EXPECT_TRUE(wal.has_unresolved_transaction());
  bool applied = false;
  EXPECT_FALSE(wal.replay(0, [&](uint64_t, uint64_t, const std::string &) { applied = true; return true; }));
  EXPECT_FALSE(applied);
}

TEST_F(RapidWalTest, DeferredStopRequiresLogAndAcceptsExactDurableWatermark) {
  struct Pending { uint64_t lsn; };
  std::deque<Pending> unknown{{0}, {100}};
  ShannonBase::Populate::DrainDeferredCommits(unknown, std::nullopt,
                                            [](Pending &, bool durable) { EXPECT_FALSE(durable); });
  WAL wal(m_dir / "capture");
  ASSERT_TRUE(wal.reset());
  ASSERT_TRUE(wal.checkpoint(1));
  ASSERT_NE(0u, wal.capture(1, "confirmed"));
  std::deque<Pending> confirmed{{100}};
  ShannonBase::Populate::DrainDeferredCommits(confirmed, 100,
                                            [&](Pending &, bool durable) { ASSERT_TRUE(durable); EXPECT_TRUE(wal.committed(1)); });
  ASSERT_TRUE(wal.open());
  size_t calls = 0;
  EXPECT_TRUE(wal.replay(0, [&](uint64_t, uint64_t, const std::string &) { ++calls; return true; }));
  EXPECT_EQ(1u, calls);
}

TEST_F(RapidWalTest, CaptureWalCommittedBeforeApplySurvivesRestart) {
  WAL wal(m_dir / "capture");
  ASSERT_TRUE(wal.reset());
  ASSERT_TRUE(wal.checkpoint(1));
  const auto sequence = wal.capture(1, "detached row image");
  ASSERT_NE(0u, sequence);
  ASSERT_TRUE(wal.committed(1));
  EXPECT_FALSE(wal.quiescent());  // committed, but still in the volatile queue
  EXPECT_FALSE(wal.checkpoint(2));
  ASSERT_TRUE(wal.open());
  uint64_t cut = 999;
  ASSERT_TRUE(wal.checkpoint_cut(1, &cut));
  EXPECT_EQ(0u, cut);
  size_t calls = 0;
  ASSERT_TRUE(wal.replay(cut, [&](uint64_t seq, uint64_t txn, const std::string &row) {
    EXPECT_EQ(sequence, seq);
    EXPECT_EQ(1u, txn);
    EXPECT_EQ("detached row image", row);
    ++calls;
    return true;
  }));
  EXPECT_EQ(1u, calls);
  EXPECT_TRUE(wal.quiescent());
}

TEST_F(RapidWalTest, CaptureWalAppliedIsNotSourceCommit) {
  WAL wal(m_dir / "capture");
  ASSERT_TRUE(wal.reset());
  ASSERT_TRUE(wal.checkpoint(1));
  const auto sequence = wal.capture(1, "uncommitted");
  ASSERT_NE(0u, sequence);
  wal.applied(sequence);
  EXPECT_FALSE(wal.quiescent());
  EXPECT_FALSE(wal.checkpoint(2));
  ASSERT_TRUE(wal.open());
  bool called = false;
  EXPECT_FALSE(wal.replay(0, [&](uint64_t, uint64_t, const std::string &) { called = true; return true; }));
  EXPECT_FALSE(called);  // ambiguity is detected before mutating staging data
}

TEST_F(RapidWalTest, CaptureWalAbortIsNotReplayed) {
  WAL wal(m_dir / "capture");
  ASSERT_TRUE(wal.reset());
  ASSERT_NE(0u, wal.capture(1, "rolled back"));
  ASSERT_TRUE(wal.aborted(1));
  ASSERT_TRUE(wal.open());
  bool called = false;
  EXPECT_TRUE(wal.replay(0, [&](uint64_t, uint64_t, const std::string &) { called = true; return true; }));
  EXPECT_FALSE(called);
  EXPECT_TRUE(wal.quiescent());
}

TEST_F(RapidWalTest, CaptureWalRejectsTornOutcome) {
  WAL wal(m_dir / "capture");
  ASSERT_TRUE(wal.reset());
  ASSERT_NE(0u, wal.capture(1, "committed primary"));
  ASSERT_TRUE(wal.committed(1));
  auto file = m_dir / "capture" / "rapid_wal.log";
  fs::resize_file(file, fs::file_size(file) - 1);
  EXPECT_FALSE(wal.open());
  EXPECT_FALSE(wal.replay(0, [](uint64_t, uint64_t, const std::string &) { return true; }));
}

TEST_F(RapidWalTest, CaptureWalRejectsCorruptPayload) {
  WAL wal(m_dir / "capture");
  ASSERT_TRUE(wal.reset());
  ASSERT_NE(0u, wal.capture(1, "row"));
  ASSERT_TRUE(wal.committed(1));
  std::fstream file(m_dir / "capture" / "rapid_wal.log", std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(44 + 25);  // first payload byte
  file.put('X');
  file.close();
  EXPECT_FALSE(wal.open());
}

TEST_F(RapidWalTest, CaptureWalCompactionPreservesRetainedCheckpoint) {
  WAL wal(m_dir / "capture");
  ASSERT_TRUE(wal.reset());
  ASSERT_TRUE(wal.checkpoint(1));
  auto first = wal.capture(1, "first");
  ASSERT_NE(0u, first);
  ASSERT_TRUE(wal.committed(1));
  wal.applied(first);
  ASSERT_TRUE(wal.checkpoint(2));
  uint64_t cut = 0;
  ASSERT_TRUE(wal.checkpoint_cut(2, &cut));
  auto second = wal.capture(2, "second");
  ASSERT_GT(second, first);
  ASSERT_TRUE(wal.committed(2));
  wal.applied(second);
  ASSERT_TRUE(wal.checkpoint(3));
  ASSERT_TRUE(wal.compact(cut));
  ASSERT_TRUE(wal.open());
  uint64_t obsolete = 0;
  EXPECT_FALSE(wal.checkpoint_cut(1, &obsolete));
  ASSERT_TRUE(wal.checkpoint_cut(2, &cut));
  std::vector<std::string> rows;
  ASSERT_TRUE(wal.replay(cut, [&](uint64_t, uint64_t, const std::string &row) { rows.push_back(row); return true; }));
  ASSERT_EQ(1u, rows.size());
  EXPECT_EQ("second", rows.front());
  EXPECT_GT(wal.capture(3, "third"), second);
}

TEST_F(RapidWalTest, CaptureWalNewEpochRejectsOldCheckpoint) {
  WAL wal(m_dir / "capture");
  ASSERT_TRUE(wal.reset());
  ASSERT_TRUE(wal.checkpoint(1));
  ASSERT_TRUE(wal.reset());
  uint64_t cut = 0;
  EXPECT_FALSE(wal.checkpoint_cut(1, &cut));
}

TEST_F(RapidWalTest, CaptureWalInvalidationSurvivesRestart) {
  WAL wal(m_dir / "capture");
  ASSERT_TRUE(wal.reset());
  ASSERT_TRUE(wal.checkpoint(1));
  ASSERT_TRUE(wal.invalidate());
  ASSERT_TRUE(wal.open());
  uint64_t cut = 0;
  EXPECT_FALSE(wal.checkpoint_cut(1, &cut));
  EXPECT_FALSE(wal.replay(0, [](uint64_t, uint64_t, const std::string &) { return true; }));
}

// This failure path requires a DBUG hook, which NDEBUG builds compile out.
#if !defined(NDEBUG)
TEST_F(RapidWalTest, CaptureWalWriteFailureRevokesCheckpoint) {
  WAL wal(m_dir / "capture");
  ASSERT_TRUE(wal.reset());
  ASSERT_TRUE(wal.checkpoint(1));
  DBUG_PUSH("+d,rapid_capture_wal_write_error");
  const auto sequence = wal.capture(1, "uncaptured source change");
  const bool invalidated = wal.invalidate();
  DBUG_POP();
  EXPECT_EQ(0u, sequence);
  ASSERT_TRUE(invalidated);
  EXPECT_TRUE(wal.disabled());
  uint64_t cut = 0;
  EXPECT_FALSE(wal.checkpoint_cut(1, &cut));
  EXPECT_FALSE(wal.open());
  EXPECT_FALSE(wal.checkpoint_cut(1, &cut));
}
#endif

TEST_F(RapidWalTest, CaptureWalConcurrentTransactionsHaveUniqueSequences) {
  WAL wal(m_dir / "capture");
  ASSERT_TRUE(wal.reset());
  std::vector<uint64_t> sequences(8);
  std::vector<std::thread> workers;
  for (size_t i = 0; i < sequences.size(); ++i) {
    workers.emplace_back([&, i] {
      sequences[i] = wal.capture(i + 1, "row" + std::to_string(i));
      EXPECT_NE(0u, sequences[i]);
      EXPECT_TRUE(wal.committed(i + 1));
      wal.applied(sequences[i]);
    });
  }
  for (auto &worker : workers) worker.join();
  std::sort(sequences.begin(), sequences.end());
  EXPECT_EQ(sequences.end(), std::adjacent_find(sequences.begin(), sequences.end()));
  EXPECT_TRUE(wal.quiescent());
  ASSERT_TRUE(wal.open());
  size_t calls = 0;
  EXPECT_TRUE(wal.replay(0, [&](uint64_t, uint64_t, const std::string &) { ++calls; return true; }));
  EXPECT_EQ(8u, calls);
}

}  // namespace shannon_rapid_wal_unittest

namespace shannon_rapid_participants_unittest {

struct TestImcu {
  uint32_t partition_local_id{0};
};
using Participants = ShannonBase::Populate::TransactionImcuParticipants<TestImcu>;

TEST(TransactionImcuParticipantsTest, RepeatedRowsFinalizeOneImcu) {
  Participants participants;
  auto imcu = std::make_shared<TestImcu>();
  for (unsigned i = 0; i < 1000; ++i) ASSERT_TRUE(participants.add(imcu));
  auto touched = participants.take();
  ASSERT_EQ(1U, touched.size());
  EXPECT_EQ(imcu, touched.begin()->second);
  EXPECT_TRUE(participants.empty());
  EXPECT_TRUE(participants.take().empty());
}

TEST(TransactionImcuParticipantsTest, PartitionLocalIdsDoNotMergeDistinctImcus) {
  Participants participants;
  auto source = std::make_shared<TestImcu>();
  auto destination = std::make_shared<TestImcu>();
  ASSERT_EQ(source->partition_local_id, destination->partition_local_id);
  ASSERT_TRUE(participants.add(source));
  ASSERT_TRUE(participants.add(destination));
  auto touched = participants.take();
  EXPECT_EQ(2U, touched.size());
  EXPECT_EQ(source, touched.at(source.get()));
  EXPECT_EQ(destination, touched.at(destination.get()));
}

TEST(TransactionImcuParticipantsTest, DrainKeepsOwnershipUntilFinalizationCompletes) {
  Participants participants;
  auto imcu = std::make_shared<TestImcu>();
  std::weak_ptr<TestImcu> lifetime = imcu;
  ASSERT_TRUE(participants.add(imcu));
  imcu.reset();
  EXPECT_FALSE(lifetime.expired());
  auto touched = participants.take();
  EXPECT_TRUE(participants.empty());
  EXPECT_FALSE(lifetime.expired());
  touched.clear();
  EXPECT_TRUE(lifetime.expired());
}

TEST(TransactionImcuParticipantsTest, LateApplicationAfterPublicationCanBeDrainedAgain) {
  Participants participants;
  auto first = std::make_shared<TestImcu>();
  auto late = std::make_shared<TestImcu>();
  ASSERT_TRUE(participants.add(first));
  auto committed = participants.take();
  ASSERT_TRUE(participants.add(late));
  auto after_commit = participants.take();
  ASSERT_EQ(1U, committed.size());
  ASSERT_EQ(1U, after_commit.size());
  EXPECT_EQ(first, committed.begin()->second);
  EXPECT_EQ(late, after_commit.begin()->second);
}

TEST(TransactionImcuParticipantsTest, SingleRowWorkDoesNotGrowWithUntouchedImcus) {
  for (size_t table_size : {1U, 64U, 4096U, 65536U}) {
    std::vector<std::shared_ptr<TestImcu>> table;
    table.reserve(table_size);
    for (size_t i = 0; i < table_size; ++i) table.push_back(std::make_shared<TestImcu>());
    Participants participants;
    ASSERT_TRUE(participants.add(table.front()));
    auto touched = participants.take();
    size_t finalized{0};
    for (const auto &[identity, imcu] : touched) {
      EXPECT_EQ(table.front().get(), identity);
      EXPECT_EQ(table.front(), imcu);
      ++finalized;
    }
    EXPECT_EQ(1U, finalized) << "table IMCUs=" << table_size;
    for (size_t i = 1; i < table_size; ++i) EXPECT_EQ(1, table[i].use_count());
  }
}

}  // namespace shannon_rapid_participants_unittest
