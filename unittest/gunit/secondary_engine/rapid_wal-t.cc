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
 * Unit test for the Rapid engine WAL (CURecoveryManager).
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

#include "storage/rapid_engine/imcs/cu_recovery.h"

#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "storage/rapid_engine/imcs/imcu.h"

namespace shannon_rapid_wal_unittest {

namespace fs = std::filesystem;

using ShannonBase::ErrorCode;
using ShannonBase::Imcs::CURecoveryManager;
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

  std::unique_ptr<CURecoveryManager> MakeManager() const {
    return std::make_unique<CURecoveryManager>(m_dir.string(), kDb, kTbl);
  }

  fs::path WalPath() const { return m_mgr->wal_path(); }

  /** Replay the WAL and collect every record handed to apply_fn. */
  ShannonBase::Result<size_t> Replay(CURecoveryManager *mgr, std::vector<WalRecord> *out) const {
    Imcu imcu;  // id 0, no owner, no snapshot: cold start
    std::vector<Imcu *> imcus{&imcu};
    return mgr->recover(imcus, [out](const WalRecord &rec) {
      out->push_back(rec);
      return ErrorCode::OK;
    });
  }

  std::vector<WalRecord> ReplayExpectOk(CURecoveryManager *mgr) const {
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
  std::unique_ptr<CURecoveryManager> m_mgr;
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

}  // namespace shannon_rapid_wal_unittest
