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
#include <memory>

#include "my_inttypes.h"
#include "sql/partitioning/partition_handler.h"

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
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

  // Partition_helper consults this to decide whether an ordered cross-partition
  // index scan needs a secondary sort by rowid (position()/rnd_pos_in_part(),
  // neither of which this handler implements) to break ties on equal index
  // values: see Partition_helper::init_record_priority_queue()'s REF_NOT_USED
  // vs REF_STORED_IN_PQ branch. Every Rapid index_read()/index_next() already
  // returns the complete row in one step -- the same guarantee a clustered
  // index gives -- for any index, not just the primary key, so that
  // ref-based re-fetch is never needed here.
  bool primary_key_is_clustered() const override { return true; }

  // Used by Partition_helper::open_partitioning() to bind its m_table pointer.
  TABLE *get_table() const override { return table; }

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
  // Chains into ha_rapid::open()/close(), then sets up/tears down
  // Partition_helper's own state (m_table, m_tot_parts, the "key not found"
  // bitmap, etc. -- see Partition_helper::open_partitioning()). Without this,
  // that state is left default-constructed garbage, since nothing else calls
  // open_partitioning() for a handler outside of ha_partition/ha_innopart's
  // own open() override.
  int open(const char *name, int mode, unsigned int test_if_locked, const dd::Table *table_def) override;

  int close() override;

  int rnd_init(bool scan) override;

  int rnd_next(uchar *record) override { return (Partition_helper::ph_rnd_next(record)); }

  int rnd_end() override;

  int rnd_pos(uchar *record, uchar *pos) override;

  int index_init(uint keynr, bool sorted) override;

  int index_end() override;

  int index_next(uchar *record) override { return (Partition_helper::ph_index_next(record)); }

  int index_next_same(uchar *record, const uchar *, uint keylen) override {
    return (Partition_helper::ph_index_next_same(record, keylen));
  }

  int index_prev(uchar *record) override { return (Partition_helper::ph_index_prev(record)); }

  int index_first(uchar *record) override { return (Partition_helper::ph_index_first(record)); }

  int index_last(uchar *record) override { return (Partition_helper::ph_index_last(record)); }

  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map, ha_rkey_function find_flag) override {
    return (Partition_helper::ph_index_read_map(buf, key, keypart_map, find_flag));
  }

  int index_read_last_map(uchar *buf, const uchar *key, key_part_map keypart_map) override {
    return (Partition_helper::ph_index_read_last_map(buf, key, keypart_map));
  }

  int index_read_idx_map(uchar *buf, uint index, const uchar *key, key_part_map keypart_map,
                         ha_rkey_function find_flag) override {
    return (Partition_helper::ph_index_read_idx_map(buf, index, key, keypart_map, find_flag));
  }

  int read_range_first(const key_range *start_key, const key_range *end_key, bool eq_range_arg, bool sorted) override {
    return (Partition_helper::ph_read_range_first(start_key, end_key, eq_range_arg, sorted));
  }

  int read_range_next() override { return (Partition_helper::ph_read_range_next()); }

 private:
  THD *m_thd{nullptr};

  std::shared_ptr<RapidShare> m_share;

  /** this is set to 1 when we are starting a table scan but have
  not yet fetched any row, else false */
  bool m_start_of_scan{false};

  bool m_current_part_empty = false;

  struct PartIndexScanState {
    bool valid{false};
    std::vector<uchar> key;
    ha_rkey_function find_flag{HA_READ_KEY_EXACT};
  };
  std::vector<PartIndexScanState> m_part_scan_state;
  uint m_cursor_part_id{NO_CURRENT_PART_ID};

  // Points m_cursor at part_id's IMCS table, switching (and thus resetting
  // the cursor's index position) only if it isn't already there.
  // Returns HA_ERR_END_OF_FILE if no data was ever loaded for this partition.
  int switch_to_partition(uint part_id);

  // Tries to resume part_id's scan after a switch, using saved state.
  // Returns true (with *error set) if a resume reseek was performed;
  // false if there was no saved state to resume from (caller should
  // perform a fresh index_next()/index_prev() instead).
  bool try_resume_partition(uint part_id, uchar *buf, int *error);

  // Records how to continue part_id's scan after successfully reading buf.
  void save_scan_position(uint part_id, const uchar *buf, bool reverse);

  // Records how to continue part_id's scan after a start lookup (index_read_map
  // -style) found no matching key, anchored on the search key it was given.
  void save_miss_position(uint part_id, const uchar *key, uint key_len, bool reverse);
};

}  // namespace ShannonBase
#endif /* __HA_SHANNON_RAPIDPART_H__ */
