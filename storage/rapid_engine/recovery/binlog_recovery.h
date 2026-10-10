#ifndef SHANNONBASE_BINLOG_RECOVERY_H
#define SHANNONBASE_BINLOG_RECOVERY_H
#include <functional>
#include "storage/rapid_engine/populate/log_commons.h"
#include "storage/rapid_engine/recovery/notification_tracker.h"
class THD;
struct TABLE;
namespace ShannonBase::Recovery {
// Recovery-only reader of local committed MySQL transactions. Never executes
// replication events against the primary. Failure discards the private restore.
class BinlogRecovery {
 public:
  static BinlogPosition current_position();
  static bool certify(BinlogPosition &position);
  static bool replay(THD *thd, TABLE *table, const BinlogPosition &start,
                     const std::function<bool(Populate::change_record_buff_t &)> &apply);
};
}  // namespace ShannonBase::Recovery
#endif
