#ifndef SHANNON_PROPAGATION_MODE_H
#define SHANNON_PROPAGATION_MODE_H

#include <atomic>
#include <cstdint>
#include <string>

class THD;
class TABLE;

namespace ShannonBase::Populate {
enum class ChangePropagationMode : unsigned long { CAPTURE = 0, COMMITTED_BINLOG = 1 };

// The configured default is sampled at load admission. Loaded CAPTURE tables
// retain their backend when the default changes; a future implementation must
// bind this mode to each loaded table rather than switching live producers.
inline std::atomic<ChangePropagationMode> configured_change_propagation_mode{ChangePropagationMode::CAPTURE};

constexpr bool propagation_query_supported(ChangePropagationMode mode, bool autocommit, bool explicit_transaction) {
  return mode == ChangePropagationMode::CAPTURE || (autocommit && !explicit_transaction);
}
constexpr bool propagation_backend_available(ChangePropagationMode mode) {
  return mode == ChangePropagationMode::CAPTURE;
}

// Reserved rev.2 interface. DML checks logging coverage without copying row
// images. Commit publishes an authoritative source boundary; the background
// consumer parses only the complete committed prefix and enqueues changes.
// Admission stays closed until an implementation supplies this contract.
struct CommittedBinlogBoundary {
  std::string file;
  uint64_t end_position{0};
};
class CommittedBinlogConsumer {
 public:
  virtual ~CommittedBinlogConsumer() = default;
  virtual bool check_dml_log_coverage(THD *, const TABLE *, uint64_t statement_id) = 0;
  virtual bool publish_commit_boundary(const CommittedBinlogBoundary &) noexcept = 0;
  virtual bool wait_until_applied(const CommittedBinlogBoundary &) = 0;
  virtual void stop() noexcept = 0;
};
}  // namespace ShannonBase::Populate
#endif
