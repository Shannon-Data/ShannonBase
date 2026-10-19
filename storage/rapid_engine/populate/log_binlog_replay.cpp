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
*/
#include "storage/rapid_engine/populate/log_binlog_replay.h"
#include <filesystem>
#include "sql/binlog.h"
#include "sql/field.h"
#include "sql/sql_class.h"
#include "sql/table.h"
#include "storage/rapid_engine/populate/log_binlog_parser.h"
#include "storage/rapid_engine/trx/transaction.h"
#include "storage/rapid_engine/utils/binlog_position.h"

namespace ShannonBase::Populate {
namespace {
bool supported(TABLE *table) {
  if (!table || table->part_info || table->s->primary_key == MAX_KEY || table->s->foreign_keys ||
      table->s->foreign_key_parents)
    return false;
  for (uint i = 0; i < table->s->fields; ++i) {
    const auto *f = table->field[i];
    if (f->is_virtual_gcol() || f->gcol_info || (f->all_flags() & BLOB_FLAG) || f->real_type() == MYSQL_TYPE_JSON ||
        f->real_type() == MYSQL_TYPE_GEOMETRY || f->real_type() == MYSQL_TYPE_VECTOR || f->is_array())
      return false;
  }
  return true;
}
}  // namespace

bool BinlogReplay::replay(THD *thd, TABLE *table, const BinlogPosition &start,
                          const std::function<bool(change_record_buff_t &)> &apply) {
  if (!thd || !supported(table) || !mysql_bin_log.is_open() || !start.valid() || start.prefix_digest.empty())
    return false;
  // The caller retains write-excluding source MDL. Preserve recovery's strict
  // rejection of statement events and its fixed physical end, independently
  // of the online reader's (possibly absent) committed watermark.
  LOG_INFO current;
  if (mysql_bin_log.get_current_log(&current) || current.encrypted_header_size) return false;
  const BinlogPosition stop{std::filesystem::path(current.log_file_name).filename().string(), current.pos, {}};
  const std::vector<BinlogTableBinding> bindings{{table, table->file->get_table_id(), start}};
  auto cursor = start;
  const auto anchor_valid = [&] {
    auto anchor = start;
    return Utils::BinlogPositionReader::certify(anchor) && anchor.prefix_digest == start.prefix_digest;
  };
  if (!anchor_valid()) return false;
  while (cursor.file != stop.file || cursor.offset != stop.offset) {
    if (thd->killed) return false;
    std::vector<BinlogTransaction> batch;
    BinlogPosition end;
    if (!BinlogParser::read_batch(thd, bindings, cursor, stop, batch, end, nullptr, nullptr, true)) return false;
    if (end.file == cursor.file && end.offset == cursor.offset) return false;
    // Each bounded batch reopens input. Certify both the original anchor and
    // the next cut so RESET/reused names between batches cannot splice epochs.
    if (!anchor_valid() || !Utils::BinlogPositionReader::certify(end)) return false;
    for (auto &transaction : batch) {
      const uint64_t scn = TransactionCoordinator::instance().allocate_scn();
      for (auto &record : transaction.changes) {
        if (thd->killed) return false;
        record.m_commit_scn = scn;
        if (!apply(record)) return false;
      }
      // Allocation alone does not publish committed visibility for attachment.
      TransactionCoordinator::instance().observe_commit_scn(scn);
    }
    cursor = std::move(end);
  }
  return anchor_valid();
}
}  // namespace ShannonBase::Populate
