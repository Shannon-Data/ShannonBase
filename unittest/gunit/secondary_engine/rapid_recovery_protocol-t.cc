// Copyright (c) 2026, Shannon Data AI and/or its affiliates.
// SPDX-License-Identifier: GPL-2.0-only
#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include <new>
#include <sstream>
#include <thread>
#include "storage/rapid_engine/recovery/checkpoint_store.h"
#include "storage/rapid_engine/recovery/native_stream.h"
#include "storage/rapid_engine/recovery/notification_tracker.h"
#include "storage/rapid_engine/recovery/source_validation.h"
#include "storage/rapid_engine/recovery/table_persistence.h"
#include "storage/rapid_engine/utils/crc.h"
#include "storage/rapid_engine/utils/try_operation.h"
using namespace ShannonBase::Recovery;
TEST(NativeColumnStream, CannotReadTheFollowingColumn) {
  std::istringstream source("dataNEXT");
  NativeColumnStream slice(source.rdbuf(), 4);
  std::istream column(&slice);
  char bytes[8]{};
  column.read(bytes, sizeof(bytes));
  EXPECT_TRUE(column.fail());
  EXPECT_EQ(column.gcount(), 4);
  EXPECT_EQ(std::string(bytes, 4), "data");
  EXPECT_EQ(slice.remaining(), 0u);
  EXPECT_EQ(source.peek(), 'N');
}
TEST(NativeColumnStream, PeekAndGetPreserveTheBoundary) {
  std::istringstream source("abNEXT");
  NativeColumnStream slice(source.rdbuf(), 2);
  std::istream column(&slice);
  EXPECT_EQ(column.peek(), 'a');
  EXPECT_EQ(slice.remaining(), 2u);
  EXPECT_EQ(column.get(), 'a');
  EXPECT_EQ(column.get(), 'b');
  EXPECT_EQ(column.peek(), std::char_traits<char>::eof());
  EXPECT_EQ(source.peek(), 'N');
}
TEST(NativeColumnStream, TruncatedInputRetainsUnconsumedLength) {
  std::istringstream source("ab");
  NativeColumnStream slice(source.rdbuf(), 4);
  std::istream column(&slice);
  char bytes[4]{};
  column.read(bytes, sizeof(bytes));
  EXPECT_TRUE(column.fail());
  EXPECT_EQ(column.gcount(), 2);
  EXPECT_EQ(slice.remaining(), 2u);
}
namespace {
class StubSourceValidator : public SourceRecoveryValidator {
 public:
  SourceValidationResult result{SourceValidationResult::VERIFIED};
  unsigned calls{0};
  bool throws{false};
  bool expires{false};
  const SourceValidationContext *seen{nullptr};
  SourceValidationResult validate(const SourceValidationContext &context) override {
    ++calls;
    seen = &context;
    if (throws) throw std::bad_alloc();
    if (expires) std::this_thread::sleep_until(context.deadline);
    return result;
  }
};
SourceValidationContext validation_context() {
  return {nullptr, nullptr, nullptr, nullptr, std::chrono::steady_clock::now() + std::chrono::minutes(1)};
}
}  // namespace
TEST(RapidSourceValidation, FastNeverCallsRegisteredValidator) {
  auto validator = std::make_shared<StubSourceValidator>();
  validator->throws = true;
  SourceValidationPolicy policy{SourceValidationMode::FAST, validator};
  EXPECT_EQ(policy.validate(validation_context()), SourceValidationResult::SKIPPED);
  EXPECT_EQ(validator->calls, 0u);
}
TEST(RapidSourceValidation, FullWithoutValidatorIsUnavailable) {
  SourceValidationPolicy policy{SourceValidationMode::FULL, nullptr};
  EXPECT_EQ(policy.validate(validation_context()), SourceValidationResult::UNAVAILABLE);
}
TEST(RapidSourceValidation, FullVerifiedPassesCompleteContext) {
  auto validator = std::make_shared<StubSourceValidator>();
  SourceValidationPolicy policy{SourceValidationMode::FULL, validator};
  auto context = validation_context();
  EXPECT_EQ(policy.validate(context), SourceValidationResult::VERIFIED);
  EXPECT_EQ(validator->seen, &context);
  EXPECT_EQ(validator->calls, 1u);
}
TEST(RapidSourceValidation, FullMismatchRemainsFailure) {
  auto validator = std::make_shared<StubSourceValidator>();
  validator->result = SourceValidationResult::MISMATCH;
  SourceValidationPolicy policy{SourceValidationMode::FULL, validator};
  EXPECT_EQ(policy.validate(validation_context()), SourceValidationResult::MISMATCH);
}
TEST(RapidSourceValidation, FullCannotSkipVerification) {
  auto validator = std::make_shared<StubSourceValidator>();
  validator->result = SourceValidationResult::SKIPPED;
  SourceValidationPolicy policy{SourceValidationMode::FULL, validator};
  EXPECT_EQ(policy.validate(validation_context()), SourceValidationResult::UNAVAILABLE);
}
TEST(RapidSourceValidation, FullProviderExceptionIsUnavailable) {
  auto validator = std::make_shared<StubSourceValidator>();
  validator->throws = true;
  SourceValidationPolicy policy{SourceValidationMode::FULL, validator};
  EXPECT_EQ(policy.validate(validation_context()), SourceValidationResult::UNAVAILABLE);
}
TEST(RapidSourceValidation, FullExpiredDeadlineDoesNotCallValidator) {
  auto validator = std::make_shared<StubSourceValidator>();
  SourceValidationPolicy policy{SourceValidationMode::FULL, validator};
  auto context = validation_context();
  context.deadline = std::chrono::steady_clock::now();
  EXPECT_EQ(policy.validate(context), SourceValidationResult::UNAVAILABLE);
  EXPECT_EQ(validator->calls, 0u);
}
TEST(RapidSourceValidation, FullOverrunCannotReportVerified) {
  auto validator = std::make_shared<StubSourceValidator>();
  validator->expires = true;
  SourceValidationPolicy policy{SourceValidationMode::FULL, validator};
  auto context = validation_context();
  context.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
  EXPECT_EQ(policy.validate(context), SourceValidationResult::UNAVAILABLE);
}
TEST(RapidSourceValidation, PolicySnapshotRetainsModeAndValidator) {
  auto saved = SourceRecoveryValidation::current();
  auto validator = std::make_shared<StubSourceValidator>();
  SourceRecoveryValidation::install({SourceValidationMode::FULL, validator});
  auto snapshot = SourceRecoveryValidation::current();
  SourceRecoveryValidation::install(saved);
  EXPECT_EQ(snapshot.validate(validation_context()), SourceValidationResult::VERIFIED);
  EXPECT_EQ(validator->calls, 1u);
}
TEST(RapidRecoveryProtocol, CommitBeforeApplyCannotCheckpoint) {
  NotificationTracker tracker;
  const auto seq = tracker.notify(42);
  tracker.committed(42, {"binlog.000001", 100, ""});
  std::lock_guard guard(tracker.mutex());
  EXPECT_FALSE(tracker.quiescent());
  tracker.applied(seq);
  EXPECT_TRUE(tracker.quiescent());
  EXPECT_TRUE(tracker.needs_checkpoint());
}
TEST(RapidRecoveryProtocol, ApplyBeforeCommitCannotCheckpoint) {
  NotificationTracker tracker;
  const auto seq = tracker.notify(42);
  tracker.applied(seq);
  std::lock_guard guard(tracker.mutex());
  EXPECT_FALSE(tracker.quiescent());
  tracker.committed(42, {"binlog.000001", 100, ""});
  EXPECT_TRUE(tracker.quiescent());
}
TEST(RapidRecoveryProtocol, AbortedQueuedRowsMustDrain) {
  NotificationTracker tracker;
  const auto seq = tracker.notify(42);
  tracker.aborted(42);
  std::lock_guard guard(tracker.mutex());
  EXPECT_FALSE(tracker.quiescent());
  tracker.applied(seq);
  EXPECT_TRUE(tracker.quiescent());
  EXPECT_FALSE(tracker.position().valid());
}
TEST(RapidRecoveryProtocol, MissingBinlogCommitDisablesCheckpoint) {
  NotificationTracker tracker;
  auto seq = tracker.notify(42);
  tracker.committed(42, {});
  tracker.applied(seq);
  std::lock_guard guard(tracker.mutex());
  EXPECT_FALSE(tracker.quiescent());
  EXPECT_FALSE(tracker.needs_checkpoint());
}
namespace {
struct RevocationDirectory {
  std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("rapid-revoke-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ~RevocationDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  std::string snapshots() const { return (root / "rapid_snapshots").string(); }
};
class FailingStore : public CheckpointStore {
 public:
  bool publish(const std::string &, const std::string &, uint64_t, const std::filesystem::path &) override {
    return false;
  }
  bool restore(const std::string &, const std::string &, const std::filesystem::path &) override { return false; }
  bool invalidate(const std::string &, const std::string &) override { return false; }
  bool erase(const std::string &, const std::string &) override { return false; }
};
class ThrowingStore : public FailingStore {
 public:
  bool invalidate(const std::string &, const std::string &) override { throw std::bad_alloc(); }
};
}  // namespace
TEST(RapidRecoveryProtocol, RevokeProviderExceptionReturnsFailureAndFencesRecovery) {
  RevocationDirectory directory;
  CheckpointStores::install(std::make_shared<ThrowingStore>());
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  CheckpointStores::install(nullptr);
  EXPECT_FALSE(manager.revoke_fast_recovery());
  EXPECT_TRUE(manager.recovery_required());
  EXPECT_TRUE(std::filesystem::exists(manager.recovery_taint_path()));
  ShannonBase::Imcs::TablePersistenceManager reopened(directory.snapshots(), "db", "table");
  EXPECT_TRUE(reopened.open());
  EXPECT_TRUE(reopened.recovery_required());
  std::lock_guard guard(manager.notifications()->mutex());
  EXPECT_TRUE(manager.notifications()->disabled());
}
TEST(RapidRecoveryProtocol, RevokeProviderFailureFencesRecovery) {
  RevocationDirectory directory;
  CheckpointStores::install(std::make_shared<FailingStore>());
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  CheckpointStores::install(nullptr);
  EXPECT_FALSE(manager.revoke_fast_recovery());
  EXPECT_TRUE(manager.recovery_required());
  EXPECT_TRUE(std::filesystem::exists(manager.recovery_taint_path()));
  ShannonBase::Imcs::TablePersistenceManager reopened(directory.snapshots(), "db", "table");
  EXPECT_TRUE(reopened.open());
  EXPECT_TRUE(reopened.recovery_required());
  std::lock_guard guard(manager.notifications()->mutex());
  EXPECT_TRUE(manager.notifications()->disabled());
}
TEST(RapidRecoveryProtocol, ProviderLifetimeSurvivesRegistryChange) {
  auto remote = std::make_shared<FailingStore>();
  CheckpointStores::install(remote);
  auto held = CheckpointStores::current();
  CheckpointStores::install(nullptr);
  EXPECT_EQ(remote, held);
  EXPECT_FALSE(held->restore("db", "table", "/tmp"));
  EXPECT_TRUE(CheckpointStores::current()->restore("db", "table", "/tmp"));
}

TEST(RapidRecoveryProtocol, NewCommitDuringUploadNeedsAnotherCheckpoint) {
  NotificationTracker tracker;
  auto first = tracker.notify(42);
  tracker.committed(42, {"binlog.000001", 100, ""});
  tracker.applied(first);
  auto second = tracker.notify(43);
  tracker.committed(43, {"binlog.000001", 200, ""});
  tracker.applied(second);
  tracker.checkpoint_published(first);
  EXPECT_TRUE(tracker.needs_checkpoint());
  tracker.checkpoint_published(second);
  EXPECT_FALSE(tracker.needs_checkpoint());
}
TEST(RapidRecoveryProtocol, FreshSourceLoadCanPublishInitialCheckpoint) {
  NotificationTracker tracker;
  tracker.initialize({"binlog.000001", 100, ""});
  EXPECT_TRUE(tracker.needs_checkpoint());
  tracker.checkpoint_published(0);
  EXPECT_FALSE(tracker.needs_checkpoint());
}
TEST(RapidRecoveryProtocol, AnotherLongWriterBlocksCheckpoint) {
  NotificationTracker tracker;
  auto first = tracker.notify(42);
  auto second = tracker.notify(43);
  tracker.applied(first);
  tracker.applied(second);
  tracker.committed(42, {"binlog.000001", 100, ""});
  std::lock_guard lock(tracker.mutex());
  EXPECT_FALSE(tracker.quiescent());
  tracker.aborted(43);
  EXPECT_TRUE(tracker.quiescent());
}

TEST(RapidOperationStatus, SemanticFailureIsPreserved) {
  EXPECT_FALSE(ShannonBase::Utils::try_bool_operation([] { return false; }));
  EXPECT_TRUE(ShannonBase::Utils::try_bool_operation([] { return true; }));
}
TEST(RapidOperationStatus, ProviderExceptionUnwindsResourcesAndReturnsFailure) {
  bool released = false;
  struct Resource {
    bool &released;
    ~Resource() { released = true; }
  };
  EXPECT_FALSE(ShannonBase::Utils::try_bool_operation([&]() -> bool {
    Resource owned{released};
    throw std::bad_alloc();
  }));
  EXPECT_TRUE(released);
}

TEST(RapidRecoveryProtocol, MarkerInspectionErrorRejectsRecovery) {
  RevocationDirectory directory;
  auto table_path = std::filesystem::path(directory.snapshots()) / "db" / "table";
  std::filesystem::create_directories(table_path.parent_path());
  std::ofstream(table_path) << "not a directory";
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  EXPECT_TRUE(manager.recovery_tainted());
}

namespace {
void write_watermark_manifest(ShannonBase::Imcs::TablePersistenceManager &manager, uint16_t version, uint64_t snapshot,
                              uint64_t persisted,
                              const std::vector<ShannonBase::Imcs::ManifestImcuEntry> &entries = {}) {
  std::string bytes;
  auto pod = [&](auto value) { bytes.append(reinterpret_cast<const char *>(&value), sizeof(value)); };
  auto str = [&](const std::string &value) {
    pod(static_cast<uint32_t>(value.size()));
    bytes.append(value);
  };
  pod(ShannonBase::Imcs::MANIFEST_MAGIC);
  pod(version);
  pod(uint64_t{17});
  pod(uint64_t{1});
  pod(uint64_t{99});
  if (version >= 3) {
    pod(snapshot);
    pod(persisted);
  }
  str("binlog.000001");
  pod(uint64_t{100});
  str("digest");
  pod(static_cast<uint32_t>(entries.size()));
  for (const auto &entry : entries) {
    pod(entry.imcu_id);
    pod(static_cast<uint8_t>(entry.state));
    pod(entry.snapshot_sequence);
    pod(entry.snapshot_size);
    pod(entry.snapshot_crc);
    str(entry.snapshot_file);
  }
  pod(ShannonBase::Utils::crc32c_compute(bytes.data(), bytes.size(), 0));
  const auto path = manager.manifest_path(1);
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), bytes.size());
}
}  // namespace
TEST(RapidRecoveryProtocol, ManifestPreservesDistinctSnapshotAndPersistedScn) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  write_watermark_manifest(manager, 3, 100, 150);
  const auto manifest = manager.load_manifest(1);
  ASSERT_TRUE(manifest.ok());
  EXPECT_EQ(100u, manifest.value.snapshot_scn);
  EXPECT_EQ(150u, manifest.value.persisted_scn);
  EXPECT_EQ(100u, manifest.value.binlog.offset);
}
TEST(RapidRecoveryProtocol, LegacyManifestDoesNotInventScnProgress) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  write_watermark_manifest(manager, 2, 100, 150);
  const auto manifest = manager.load_manifest(1);
  ASSERT_TRUE(manifest.ok());
  EXPECT_EQ(0u, manifest.value.snapshot_scn);
  EXPECT_EQ(0u, manifest.value.persisted_scn);
}
TEST(RapidRecoveryProtocol, TableMetadataPreservesIdentityAndWatermarks) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  ShannonBase::Imcs::RecoveryTableState state{17, 3, 100, 150};
  ASSERT_TRUE(manager.persist_table_state(state));
  const auto loaded = manager.load_table_state();
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(17u, loaded.value.table_id);
  EXPECT_EQ(3u, loaded.value.generation);
  EXPECT_EQ(100u, loaded.value.snapshot_scn);
  EXPECT_EQ(150u, loaded.value.persisted_scn);
  EXPECT_FALSE(manager.recovery_tainted());
}
TEST(RapidRecoveryProtocol, StaleMetadataSurvivesWithoutRevocationMarkers) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  ShannonBase::Imcs::RecoveryTableState state{17, 3, 100, 150, ShannonBase::Imcs::PersistedLoadState::STALE, 15};
  ASSERT_TRUE(manager.persist_table_state(state));
  EXPECT_FALSE(std::filesystem::exists(manager.recovery_taint_path()));
  EXPECT_FALSE(std::filesystem::exists(manager.recovery_taint_alt_path()));
  ShannonBase::Imcs::TablePersistenceManager reopened(directory.snapshots(), "db", "table");
  ASSERT_TRUE(reopened.open());
  EXPECT_TRUE(reopened.recovery_required());
  EXPECT_EQ(state.stale_reason, reopened.load_table_state().value.stale_reason);
}
TEST(RapidRecoveryProtocol, AvailableMetadataCannotClearLiveRevocation) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  manager.require_recovery();
  ASSERT_TRUE(manager.persist_table_state({17, 3, 100, 150}));
  ASSERT_TRUE(manager.load_table_state().ok());
  EXPECT_EQ(ShannonBase::Imcs::PersistedLoadState::STALE, manager.load_table_state().value.state);
}
TEST(RapidRecoveryProtocol, ExplicitEpochResetRemovesStaleMetadata) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  ASSERT_TRUE(manager.persist_table_state({17, 3, 100, 150, ShannonBase::Imcs::PersistedLoadState::STALE, 15}));
  ASSERT_TRUE(manager.reset_epoch());
  EXPECT_FALSE(manager.recovery_required());
  EXPECT_FALSE(std::filesystem::exists(manager.table_state_path()));
  EXPECT_FALSE(manager.recovery_tainted());
  ASSERT_TRUE(manager.persist_table_state({17, 1, 200, 250}));
  EXPECT_EQ(ShannonBase::Imcs::PersistedLoadState::AVAILABLE, manager.load_table_state().value.state);
}
TEST(RapidRecoveryProtocol, CorruptMetadataFailsClosed) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  ASSERT_TRUE(manager.persist_table_state({17, 3, 100, 150}));
  std::fstream file(manager.table_state_path(), std::ios::in | std::ios::out | std::ios::binary);
  file.seekp(6);
  file.put('x');
  file.close();
  EXPECT_FALSE(manager.load_table_state().ok());
  EXPECT_TRUE(manager.recovery_tainted());
}
TEST(RapidRecoveryProtocol, TruncatedMetadataFailsClosed) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  ASSERT_TRUE(manager.persist_table_state({17, 3, 100, 150}));
  std::filesystem::resize_file(manager.table_state_path(), 12);
  EXPECT_FALSE(manager.load_table_state().ok());
  EXPECT_TRUE(manager.recovery_tainted());
}
TEST(RapidRecoveryProtocol, MetadataRejectsInvalidStateAndWatermarks) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  EXPECT_FALSE(manager.persist_table_state({17, 3, 150, 100}));
  EXPECT_FALSE(
      manager.persist_table_state({17, 3, 100, 150, static_cast<ShannonBase::Imcs::PersistedLoadState>(9), 15}));
  EXPECT_FALSE(manager.persist_table_state({17, 3, 100, 150, ShannonBase::Imcs::PersistedLoadState::STALE, 0}));
  EXPECT_FALSE(manager.persist_table_state({17, 3, 100, 150, ShannonBase::Imcs::PersistedLoadState::STALE, 255}));
  EXPECT_FALSE(manager.recovery_tainted());
}
TEST(RapidRecoveryProtocol, ManifestRejectsPersistenceBeforeSnapshot) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  write_watermark_manifest(manager, 3, 150, 100);
  EXPECT_FALSE(manager.load_manifest(1).ok());
}

TEST(RapidRecoveryProtocol, ManifestRejectsDuplicateNativeChunks) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  ShannonBase::Imcs::ManifestImcuEntry entry;
  write_watermark_manifest(manager, 3, 100, 150, {entry, entry});
  EXPECT_FALSE(manager.load_manifest(1).ok());
}
TEST(RapidRecoveryProtocol, ManifestRejectsUnknownChunkState) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  ShannonBase::Imcs::ManifestImcuEntry entry;
  entry.state = static_cast<ShannonBase::Imcs::ManifestImcuState>(255);
  write_watermark_manifest(manager, 3, 100, 150, {entry});
  EXPECT_FALSE(manager.load_manifest(1).ok());
}
TEST(RapidRecoveryProtocol, NativeObjectMustBelongToItsGeneration) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  ShannonBase::Imcs::ManifestImcuEntry entry;
  entry.state = ShannonBase::Imcs::ManifestImcuState::CHECKPOINTED;
  entry.snapshot_size = 100;
  entry.snapshot_file = "checkpoint-2/imcu_0.snap";
  write_watermark_manifest(manager, 3, 100, 150, {entry});
  EXPECT_FALSE(manager.load_manifest(1).ok());
  entry.snapshot_file = "checkpoint-1/imcu_0.snap";
  write_watermark_manifest(manager, 3, 100, 150, {entry});
  auto restored = manager.load_manifest(1);
  ASSERT_TRUE(restored.ok());
  ASSERT_EQ(restored.value.imcus.size(), 1u);
  EXPECT_EQ(restored.value.imcus[0].snapshot_file, entry.snapshot_file);
}
TEST(RapidRecoveryProtocol, NativeObjectMustContainItsHeader) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  ShannonBase::Imcs::ManifestImcuEntry entry;
  entry.state = ShannonBase::Imcs::ManifestImcuState::CHECKPOINTED;
  entry.snapshot_size = 1;
  entry.snapshot_file = "checkpoint-1/imcu_0.snap";
  write_watermark_manifest(manager, 3, 100, 150, {entry});
  EXPECT_FALSE(manager.load_manifest(1).ok());
}

TEST(RapidRecoveryProtocol, UncheckpointedChunkCannotCertifyNativeRecovery) {
  RevocationDirectory directory;
  ShannonBase::Imcs::TablePersistenceManager manager(directory.snapshots(), "db", "table");
  ShannonBase::Imcs::ManifestImcuEntry entry;
  write_watermark_manifest(manager, 3, 100, 150, {entry});
  // Qualification must fail before any column object is needed: there is no
  // native data or Rapid WAL that could reconstruct this declared chunk.
  EXPECT_FALSE(manager.recover({nullptr}).ok());
}
