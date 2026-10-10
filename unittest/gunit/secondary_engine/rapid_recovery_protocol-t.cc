// Copyright (c) 2026, Shannon Data AI and/or its affiliates.
// SPDX-License-Identifier: GPL-2.0-only
#include <gtest/gtest.h>
#include "storage/rapid_engine/recovery/checkpoint_store.h"
#include "storage/rapid_engine/recovery/notification_tracker.h"
using namespace ShannonBase::Recovery;
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
class FailingStore : public CheckpointStore {
 public:
  bool publish(const std::string &, const std::string &, uint64_t, const std::filesystem::path &) override {
    return false;
  }
  bool restore(const std::string &, const std::string &, const std::filesystem::path &) override { return false; }
  bool invalidate(const std::string &, const std::string &) override { return false; }
  bool erase(const std::string &, const std::string &) override { return false; }
};
}  // namespace
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
