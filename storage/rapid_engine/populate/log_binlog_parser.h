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

   Copyright (c) 2026, Shannon Data AI and/or its affiliates.

   Committed binary-log change propagation.
*/
/*
 * COMMITTED_BINLOG propagation:
 *   DML hooks -> activity hint and logging coverage checks (no row capture).
 *   after_commit -> monotone, authoritative committed binlog position.
 *   background reader -> complete transactions -> EnqueueRowChange -> apply.
 *   publication -> one SCN after every affected table has applied; AP queries
 *                  wait for their captured commit position and pin that view.
 * Source DML never waits for apply or queue space. The reader waits on a full
 * queue. Periodic (200 ms), backlog (64 MiB), and query-demand triggers apply.
 *
 * Requires ROW/FULL logging, strict durability, ordered commits, a primary key,
 * and full-table loads. FK cascades and generated/array columns are unsupported.
 * AP queries require autocommit=1 outside explicit transactions/XA. Oversize
 * transactions, invalid logging, and lost/corrupt logs quarantine the secondary
 * copy: FORCED errors, ON falls back, and reload restores eligibility.
 * Initial limits: sequential transaction decoding/publication, global query
 * synchronization, <=64 MiB decoded transactions and 32-bit file offsets.
 *
 * Public HeatWave documentation establishes asynchronous propagation, query
 * synchronization, stale-table rejection and transaction eligibility. Its
 * private decoder/queue implementation is inferred, not publicly established.
 * References:
 * https://dev.mysql.com/doc/heatwave/en/mys-hw-change-propagation.html
 * https://dev.mysql.com/doc/heatwave/en/heatwave-errors.html (1001, 1142, 1144)
 * https://github.com/Shannon-Data/ShannonBase/issues/924
 */
#ifndef SHANNON_LOG_BINLOG_PARSER_H
#define SHANNON_LOG_BINLOG_PARSER_H

#include <vector>
#include "storage/rapid_engine/populate/log_commons.h"
#include "storage/rapid_engine/recovery/notification_tracker.h"

class THD;
class TABLE;
class LEX;
namespace ShannonBase::Populate {
struct BinlogTableBinding {
  TABLE *table;
  table_id_t id;
  Recovery::BinlogPosition floor;
};
struct BinlogTransaction {
  Recovery::BinlogPosition end;
  std::vector<change_record_buff_t> changes;
};
// One bounded, ordered read of the local binlog. No SQL or replication apply is
// executed. Only complete source transactions at or below committed_end escape.
class BinlogParser {
 public:
  static bool read_batch(THD *, const std::vector<BinlogTableBinding> &, const Recovery::BinlogPosition &start,
                         const Recovery::BinlogPosition &committed_end, std::vector<BinlogTransaction> &,
                         Recovery::BinlogPosition &assembled_end, const char **failure_reason = nullptr);
  static bool supports(const TABLE *);
};
// Shared with the load/capture code: resolve partitions and own off-page bytes
// while the decoded event and TABLE record buffers are still alive.
bool FinalizeBinlogRow(TABLE *, change_record_buff_t &);
// Binlog producer entry: ownership stays with the reader on BUFFER_FULL.
int EnqueueRowChange(change_record_buff_t &);
class CommittedBinlog {
 public:
  static const char *check_configuration(THD *);
  static bool attach(THD *, TABLE *, const Recovery::BinlogPosition &baseline);
  static void detach(table_id_t);
  static void shutdown();
  static void notify_dml(THD *, TABLE *) noexcept;
  // Called only from the authoritative after_commit observer. No row capture,
  // allocation, disk I/O, queue-space wait or Rapid transaction participant.
  static void publish_commit(const char *file, uint64_t offset) noexcept;
  static void notify_source_reset() noexcept;
  static const char *prepare_query(THD *, LEX *, uint64_t &view_scn);
  static uint64_t query_scn(THD *);
  static const char *table_failure_reason(table_id_t);
};
}  // namespace ShannonBase::Populate
#endif
