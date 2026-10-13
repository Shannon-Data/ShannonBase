// Copyright (c) 2026, Shannon Data AI and/or its affiliates.
// SPDX-License-Identifier: GPL-2.0-only
#ifndef SHANNONBASE_SOURCE_RECOVERY_VALIDATION_H
#define SHANNONBASE_SOURCE_RECOVERY_VALIDATION_H

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>
#include "storage/rapid_engine/utils/try_operation.h"

class THD;
class TABLE;
namespace ShannonBase::Imcs {
class RpdTable;
struct RecoveryManifest;
}  // namespace ShannonBase::Imcs

namespace ShannonBase::Recovery {
enum class SourceValidationMode { FAST, FULL };
enum class SourceValidationResult { SKIPPED, VERIFIED, MISMATCH, UNAVAILABLE };

struct SourceValidationContext {
  THD *thd;
  TABLE *primary;
  Imcs::RpdTable *recovered;
  const Imcs::RecoveryManifest *manifest;
  std::chrono::steady_clock::time_point deadline;
};

/** Optional source-state verification after native restore and binlog replay.
 * The recovered table is unpublished. Source MDL fences writes until the
 * caller publishes the table and attaches capture. A FULL implementation must
 * verify every represented column, NULL, missing/extra row and current primary
 * key at this cut. Sampled checks, SCNs and binlog checks alone cannot return
 * VERIFIED. Check the deadline and THD cancellation cooperatively; failures,
 * incomplete checks and timeouts return UNAVAILABLE. Do not mutate TP state,
 * publish AP state or retain any context pointers after returning.
 */
class SourceRecoveryValidator {
 public:
  virtual ~SourceRecoveryValidator() = default;
  virtual SourceValidationResult validate(const SourceValidationContext &context) = 0;
};

struct SourceValidationPolicy {
  SourceValidationMode mode{SourceValidationMode::FAST};
  std::shared_ptr<SourceRecoveryValidator> validator;

  SourceValidationResult validate(const SourceValidationContext &context) const noexcept {
    // FAST deliberately avoids even calling a registered full-scan validator.
    if (mode == SourceValidationMode::FAST) return SourceValidationResult::SKIPPED;
    if (!validator || std::chrono::steady_clock::now() >= context.deadline) return SourceValidationResult::UNAVAILABLE;
    SourceValidationResult result = SourceValidationResult::UNAVAILABLE;
    if (!Utils::try_operation([&] { result = validator->validate(context); }))
      return SourceValidationResult::UNAVAILABLE;
    if (std::chrono::steady_clock::now() >= context.deadline) return SourceValidationResult::UNAVAILABLE;
    if (result != SourceValidationResult::VERIFIED && result != SourceValidationResult::MISMATCH)
      return SourceValidationResult::UNAVAILABLE;
    return result;
  }
};

/** Configure before recovery workers start; each job keeps an immutable policy
 * snapshot. Validators must support concurrent calls for different tables.
 * No built-in FULL validator or SQL mode switch is supplied in this version.
 */
class SourceRecoveryValidation {
 public:
  static SourceValidationPolicy current() {
    std::lock_guard lock(m_mutex);
    return m_policy;
  }
  static void install(SourceValidationPolicy policy) {
    std::lock_guard lock(m_mutex);
    m_policy = std::move(policy);
  }

 private:
  inline static std::mutex m_mutex;
  inline static SourceValidationPolicy m_policy;
};
}  // namespace ShannonBase::Recovery
#endif
