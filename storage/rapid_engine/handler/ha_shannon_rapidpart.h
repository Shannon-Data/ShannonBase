/*****************************************************************************

Copyright (c) 2014, 2024, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/*
Copyright (c) 2023, Shannon Data AI and/or its affiliates.
The ShannonBase Partition handler: the interface between MySQL and Rapid. */

#ifndef __HA_SHANNON_RAPIDPART_H__
#define __HA_SHANNON_RAPIDPART_H__

#include <stddef.h>
#include <sys/types.h>

#include "my_inttypes.h"
#include "sql/partitioning/partition_handler.h"

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/data_table.h"
#include "storage/rapid_engine/include/rapid_const.h"

/* Forward declarations */
class Altered_partitions;
class partition_info;

/* Error Text */
static constexpr auto PARTITION_IN_SHARED_TABLESPACE =
    "Rapid : A partitioned table"
    " is not allowed in a shared tablespace.";

/** HA_DUPLICATE_POS and HA_READ_BEFORE_WRITE_REMOVAL is not
set from ha_innobase, but cannot yet be also supported in ha_rapidpart.
Full text and geometry is not yet supported. */
const handler::Table_flags HA_INNOPART_DISABLED_TABLE_FLAGS =
    (HA_CAN_FULLTEXT | HA_CAN_FULLTEXT_EXT | HA_CAN_GEOMETRY | HA_DUPLICATE_POS | HA_READ_BEFORE_WRITE_REMOVAL);

namespace ShannonBase {

struct RapidPartShare : public RapidShare {};

class ha_rapidpart : public ha_rapid, public Partition_helper, public Partition_handler {
 public:
  ha_rapidpart(handlerton *hton, TABLE_SHARE *table_arg);

  ~ha_rapidpart() override = default;
  THD *get_thd() const override {
    assert(false);
    return ha_thd();
  }

  int load_table(const TABLE &table, bool *skip_metadata_update) override;

  int unload_table(const char *db_name, const char *table_name, bool error_if_not_loaded) override;

  TABLE *get_table() const override {
    assert(false);
    return nullptr;
  }

  bool get_eq_range() const override {
    assert(false);
    return false;
  }

  void set_eq_range(bool) override { assert(false); }

  void set_range_key_part(KEY_PART_INFO *) override { assert(false); }

  int write_row_in_part(uint, uchar *) override {
    assert(false);
    return 0;
  }

  int update_row_in_part(uint, const uchar *, uchar *) override {
    assert(false);
    return 0;
  }

  int delete_row_in_part(uint, const uchar *) override {
    assert(false);
    return 0;
  }

  int initialize_auto_increment(bool) override {
    assert(false);
    return 0;
  }

  int rnd_init_in_part(uint, bool) override;

  int rnd_next_in_part(uint, uchar *) override;

  int rnd_end_in_part(uint, bool) override;

  void position_in_last_part(uchar *, const uchar *) override { assert(false); }

  int index_first_in_part(uint, uchar *) override;

  int index_last_in_part(uint, uchar *) override;

  int index_prev_in_part(uint, uchar *) override;

  int index_next_in_part(uint, uchar *) override;

  int index_next_same_in_part(uint, uchar *, const uchar *, uint) override;

  int index_read_map_in_part(uint, uchar *, const uchar *, key_part_map, ha_rkey_function) override;

  int index_read_last_map_in_part(uint, uchar *, const uchar *, key_part_map) override;

  int read_range_first_in_part(uint, uchar *, const key_range *, const key_range *, bool) override;

  int read_range_next_in_part(uint, uchar *) override;

  int index_read_idx_map_in_part(uint, uchar *, uint, const uchar *, key_part_map, ha_rkey_function) override;

  int write_row_in_new_part(uint) override;

  void get_dynamic_partition_info(ha_statistics *, ha_checksum *, uint) override { assert(false); }

  void set_part_info(partition_info *part_info, bool early) override {
    Partition_helper::set_part_info_low(part_info, early);
  }

  void initialize_partitioning(partition_info *part_info, bool early) {
    Partition_helper::set_part_info_low(part_info, early);
  }

  row_type get_partition_row_type(const dd::Table *, uint) override {
    assert(false);
    row_type ret{ROW_TYPE_DEFAULT};
    return ret;
  }

  Partition_handler *get_partition_handler() override { return (static_cast<Partition_handler *>(this)); }

 protected:
  int rnd_init(bool scan) override { return (Partition_helper::ph_rnd_init(scan)); }

  int rnd_next(uchar *record) override { return (Partition_helper::ph_rnd_next(record)); }

  int rnd_pos(uchar *record, uchar *pos) override;

  int records(ha_rows *num_rows) override;

  int index_next(uchar *record) override { return (Partition_helper::ph_index_next(record)); }

  int index_next_same(uchar *record, const uchar *, uint keylen) override {
    return (Partition_helper::ph_index_next_same(record, keylen));
  }

  int index_prev(uchar *record) override { return (Partition_helper::ph_index_prev(record)); }

  int index_first(uchar *record) override { return (Partition_helper::ph_index_first(record)); }

  int index_last(uchar *record) override { return (Partition_helper::ph_index_last(record)); }

 private:
  THD *m_thd{nullptr};
  RapidPartShare *m_share{nullptr};

  /** this is set to 1 when we are starting a table scan but have
  not yet fetched any row, else false */
  bool m_start_of_scan{false};
};

}  // namespace ShannonBase
#endif /* __HA_SHANNON_RAPIDPART_H__ */
