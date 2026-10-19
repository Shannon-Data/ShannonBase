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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#ifndef __SHANNONBASE_SOURCE_RECOVERY_VALIDATION_H__
#define __SHANNONBASE_SOURCE_RECOVERY_VALIDATION_H__

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

class THD;
class TABLE;
namespace ShannonBase {
namespace Imcs {
class RpdTable;
struct RecoveryManifest;
}  // namespace Imcs
}  // namespace ShannonBase

namespace ShannonBase {
namespace Recovery {
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
    const SourceValidationResult result = validator->validate(context);
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
}  // namespace Recovery
}  // namespace ShannonBase
#endif
