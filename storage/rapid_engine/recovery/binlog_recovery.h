#ifndef __SHANNONBASE_BINLOG_RECOVERY_H__
#define __SHANNONBASE_BINLOG_RECOVERY_H__
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
  // Caller pins/locks the binlog index when validating an already resolved file.
  static bool certify_file(const char *path, BinlogPosition &position);
  static bool replay(THD *thd, TABLE *table, const BinlogPosition &start,
                     const std::function<bool(Populate::change_record_buff_t &)> &apply);
};
}  // namespace ShannonBase::Recovery
#endif
