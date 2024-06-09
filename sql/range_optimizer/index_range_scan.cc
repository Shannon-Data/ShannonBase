/* Copyright (c) 2000, 2023, Oracle and/or its affiliates.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/range_optimizer/index_range_scan.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>

#include <algorithm>

#include "field_types.h"
#include "m_string.h"
#include "my_alloc.h"
#include "my_base.h"
#include "my_dbug.h"
#include "my_sys.h"
#include "mysql/components/services/bits/psi_bits.h"
#include "mysql/service_mysql_alloc.h"
#include "mysql/strings/m_ctype.h"
#include "sql/exchange.h"
#include "sql/iterators/basic_row_iterators.h"
#include "sql/join_optimizer/bit_utils.h"
#include "sql/key.h"
#include "sql/mysqld.h"
#include "sql/psi_memory_key.h"
#include "sql/range_optimizer/reverse_index_range_scan.h"
#include "sql/sql_bitmap.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_executor.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_parallel.h"
#include "sql/sql_select.h"
#include "sql/system_variables.h"
#include "sql/table.h"
#include "sql/thr_malloc.h"
#include "sql_string.h"
#include "template_utils.h"

IndexRangeScanIterator::IndexRangeScanIterator(
    THD *thd, TABLE *table_arg, ha_rows *examined_rows, double expected_rows,
    uint key_nr, bool need_rows_in_rowid_order, bool reuse_handler,
    MEM_ROOT *return_mem_root, uint mrr_flags, uint mrr_buf_size,
    Bounds_checked_array<QUICK_RANGE *> ranges_arg)
    : RowIDCapableRowIterator(thd, table_arg),
      ranges(ranges_arg),
      free_file(false),
      cur_range(nullptr),
      last_range(nullptr),
      mrr_flags(mrr_flags),
      mrr_buf_size(mrr_buf_size),
      mrr_buf_desc(nullptr),
      need_rows_in_rowid_order(need_rows_in_rowid_order),
      reuse_handler(reuse_handler),
      mem_root(return_mem_root),
      m_expected_rows(expected_rows),
      m_examined_rows(examined_rows) {
  DBUG_TRACE;

  in_ror_merged_scan = false;
  index = key_nr;
  key_part_info = table()->key_info[index].key_part;

  file = table()->file;
}

bool IndexRangeScanIterator::shared_init() {
  if (column_bitmap.bitmap == nullptr) {
    /* Allocate a bitmap for used columns */
    my_bitmap_map *bitmap =
        (my_bitmap_map *)mem_root->Alloc(table()->s->column_bitmap_size);
    if (bitmap == nullptr) {
      column_bitmap.bitmap = nullptr;
      return true;
    } else {
      bitmap_init(&column_bitmap, bitmap, table()->s->fields);
    }
  }

  if (file->inited) file->ha_index_or_rnd_end();
  return false;
}

IndexRangeScanIterator::~IndexRangeScanIterator() {
  DBUG_TRACE;
  if (table()->key_info[index].flags & HA_MULTI_VALUED_KEY && file)
    file->ha_extra(HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER);

  /* file is NULL for CPK scan on covering ROR-intersection */
  if (file) {
    if (file->inited) file->ha_index_or_rnd_end();
    if (free_file) {
      DBUG_PRINT("info",
                 ("Freeing separate handler %p (free: %d)", file, free_file));
      file->ha_external_lock(thd(), F_UNLCK);
      file->ha_close();
      destroy(file);
    }
  }
  my_free(mrr_buf_desc);
}

/*
  Range sequence interface implementation for array<QUICK_RANGE>: initialize

  SYNOPSIS
    quick_range_seq_init()
      init_param  Caller-opaque paramenter: IndexRangeScanIterator* pointer

  RETURN
    Opaque value to be passed to quick_range_seq_next
*/

range_seq_t quick_range_seq_init(void *init_param, uint, uint) {
  IndexRangeScanIterator *quick =
      static_cast<IndexRangeScanIterator *>(init_param);
  QUICK_RANGE **first = quick->ranges.begin();
  QUICK_RANGE **last = quick->ranges.end();
  quick->qr_traversal_ctx.first = first;
  quick->qr_traversal_ctx.cur = first;
  quick->qr_traversal_ctx.last = last;
  return &quick->qr_traversal_ctx;
}

/*
  Range sequence interface implementation for array<QUICK_RANGE>: get next

  SYNOPSIS
    quick_range_seq_next()
      rseq        Value returned from quick_range_seq_init
      range  OUT  Store information about the range here

  @note This function return next range, and 'next' means next range in the
  array of ranges relatively to the current one when the first keypart has
  ASC sort order, or previous range - when key part has DESC sort order.
  This is needed to preserve correct order of records in case of multiple
  ranges over DESC keypart.

  RETURN
    0  Ok
    1  No more ranges in the sequence
*/

uint quick_range_seq_next(range_seq_t rseq, KEY_MULTI_RANGE *range) {
  QUICK_RANGE_SEQ_CTX *ctx = reinterpret_cast<QUICK_RANGE_SEQ_CTX *>(rseq);

  if (ctx->cur == ctx->last) return 1; /* no more ranges */

  QUICK_RANGE *cur = *(ctx->cur);
  key_range *start_key = &range->start_key;
  key_range *end_key = &range->end_key;

  start_key->key = cur->min_key;
  start_key->length = cur->min_length;
  start_key->keypart_map = cur->min_keypart_map;
  start_key->flag =
      ((cur->flag & NEAR_MIN)
           ? HA_READ_AFTER_KEY
           : (cur->flag & EQ_RANGE) ? HA_READ_KEY_EXACT : HA_READ_KEY_OR_NEXT);
  end_key->key = cur->max_key;
  end_key->length = cur->max_length;
  end_key->keypart_map = cur->max_keypart_map;
  /*
    We use HA_READ_AFTER_KEY here because if we are reading on a key
    prefix. We want to find all keys with this prefix.
  */
  end_key->flag =
      (cur->flag & NEAR_MAX ? HA_READ_BEFORE_KEY : HA_READ_AFTER_KEY);
  range->range_flag = cur->flag;
  ctx->cur++;
  assert(ctx->cur <= ctx->last);
  return 0;
}

/// Does this TABLE have a primary key with a BLOB component?
static bool has_blob_primary_key(const TABLE *table) {
  if (table->s->is_missing_primary_key()) {
    return false;
  }

  const KEY &key = table->key_info[table->s->primary_key];
  return std::any_of(key.key_part, key.key_part + key.user_defined_key_parts,
                     [](const KEY_PART_INFO &key_part) {
                       return Overlaps(key_part.key_part_flag, HA_BLOB_PART);
                     });
}

bool IndexRangeScanIterator::Init() {
  empty_record(table());

  /*
    Only attempt to allocate a record buffer the first time the handler is
    initialized.
  */
  const bool first_init = !table()->file->inited;

  if (!inited) {
    if (need_rows_in_rowid_order ? init_ror_merged_scan() : shared_init()) {
      return true;
    }
    inited = true;
  } else {
    if (file->inited) file->ha_index_or_rnd_end();
  }
  if (shared_reset()) {
    return true;
  }

  // Set up a record buffer. Note that we don't use
  // table->m_record_buffer, since if we are part of a ROR scan, all range
  // selects in the scan share the same TABLE object (but not the same
  // handler).
  if (first_init && table()->file->inited) {
    // Rowid-ordered retrievals may add the primary key to the read_set at
    // a later stage. If the primary key contains a BLOB component, a
    // record buffer cannot be used, since BLOBs require storage space
    // outside of the record. So don't request a buffer in this case, even
    // though the current read_set gives the impression that using a
    // record buffer would be fine.
    const bool skip_record_buffer =
        need_rows_in_rowid_order &&
        Overlaps(table()->file->ha_table_flags(),
                 HA_PRIMARY_KEY_REQUIRED_FOR_POSITION) &&
        has_blob_primary_key(table());
    if (!skip_record_buffer) {
      if (set_record_buffer(table(), m_expected_rows)) {
        return true; /* purecov: inspected */
      }
    }
  }

  return false;
}

bool InitIndexRangeScan(TABLE *table, handler *file, int index,
                        unsigned mrr_flags, bool in_ror_merged_scan,
                        MY_BITMAP *column_bitmap) {
  DBUG_TRACE;

  /* set keyread to true if index is covering */
  if (!table->no_keyread && table->covering_keys.is_set(index))
    table->set_keyread(true);
  else
    table->set_keyread(false);
  if (!file->inited) {
    /*
      read_set is set to the correct value for ror_merge_scan here as a
      subquery execution during optimization might result in innodb not
      initializing the read set in index_read() leading to wrong
      results while merging.
    */
    MY_BITMAP *const save_read_set = table->read_set;
    MY_BITMAP *const save_write_set = table->write_set;
    const bool sorted = (mrr_flags & HA_MRR_SORTED);
    DBUG_EXECUTE_IF("bug14365043_2", DBUG_SET("+d,ha_index_init_fail"););

    /* Pass index specific read set for ror_merged_scan */
    if (in_ror_merged_scan) {
      /*
        We don't need to signal the bitmap change as the bitmap is always the
        same for this table->file
      */
      table->column_bitmaps_set_no_signal(column_bitmap, column_bitmap);
    }
    if (int error = file->ha_index_init(index, sorted); error != 0) {
      (void)report_handler_error(table, error);
      return true;
    }
    if (in_ror_merged_scan) {
      file->ha_extra(HA_EXTRA_KEYREAD_PRESERVE_FIELDS);
      /* Restore bitmaps set on entry */
      table->column_bitmaps_set_no_signal(save_read_set, save_write_set);
    }
  }
  // Enable & reset unique record filter for multi-valued index
  if (table->key_info[index].flags & HA_MULTI_VALUED_KEY) {
    file->ha_extra(HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER);
    // Add PK's fields to read_set as unique filter uses rowid to skip dups
    if (table->s->primary_key != MAX_KEY)
      table->mark_columns_used_by_index_no_reset(table->s->primary_key,
                                                 table->read_set);
  }

  return false;
}

bool IndexRangeScanIterator::shared_reset() {
  last_range = nullptr;
  cur_range = ranges.begin();

  if (InitIndexRangeScan(table(), file, index, mrr_flags, in_ror_merged_scan,
                         &column_bitmap)) {
    return true;
  }

  /* Allocate buffer if we need one but haven't allocated it yet */
  if (mrr_buf_size && !mrr_buf_desc) {
    unsigned buf_size = mrr_buf_size;
    uchar *mrange_buff = nullptr;
    while (buf_size &&
           !my_multi_malloc(key_memory_IndexRangeScanIterator_mrr_buf_desc,
                            MYF(MY_WME), &mrr_buf_desc, sizeof(*mrr_buf_desc),
                            &mrange_buff, buf_size, NullS)) {
      /* Try to shrink the buffers until both are 0. */
      buf_size /= 2;
    }
    if (!mrr_buf_desc) {
      table()->file->print_error(HA_ERR_OUT_OF_MEM, MYF(0));
      return true;
    }
    /* Initialize the handler buffer. */
    mrr_buf_desc->buffer = mrange_buff;
    mrr_buf_desc->buffer_end = mrange_buff + buf_size;
    mrr_buf_desc->end_of_used_area = mrange_buff;
  }

  HANDLER_BUFFER empty_buf;
  if (mrr_buf_desc == nullptr) {
    empty_buf.buffer = empty_buf.buffer_end = empty_buf.end_of_used_area =
        nullptr;
  }

  RANGE_SEQ_IF seq_funcs = {quick_range_seq_init, quick_range_seq_next,
                            nullptr};
  if (int error = file->multi_range_read_init(
          &seq_funcs, this, ranges.size(), mrr_flags,
          mrr_buf_desc ? mrr_buf_desc : &empty_buf);
      error != 0) {
    (void)report_handler_error(table(), error);
    return true;
  }

  return false;
}

int IndexRangeScanIterator::Read() {
  MY_BITMAP *const save_read_set = table()->read_set;
  MY_BITMAP *const save_write_set = table()->write_set;
  DBUG_TRACE;

  if (in_ror_merged_scan) {
    /*
      We don't need to signal the bitmap change as the bitmap is always the
      same for this table()->file
    */
    table()->column_bitmaps_set_no_signal(&column_bitmap, &column_bitmap);
  }

  char *dummy;
  int result = file->ha_multi_range_read_next(&dummy);

  if (in_ror_merged_scan) {
    /* Restore bitmaps set on entry */
    table()->column_bitmaps_set_no_signal(save_read_set, save_write_set);
    if (result == 0) {
      file->position(table()->record[0]);
    }
  }
  if (result == 0) {
    if (m_examined_rows != nullptr) {
      ++*m_examined_rows;
    }
    return 0;
  }
  return HandleError(result);
}

/*
  Check if current row will be retrieved by this IndexRangeScanIterator

  NOTES
    It is assumed that currently a scan is being done on another index
    which reads all necessary parts of the index that is scanned by this
    quick select.
    The implementation does a binary search on sorted array of disjoint
    ranges, without taking size of range into account.

    This function is used to filter out clustered PK scan rows in
    index_merge quick select.

  RETURN
    true  if current row will be retrieved by this quick select
    false if not
*/

bool IndexRangeScanIterator::row_in_ranges() {
  if (ranges.empty()) return false;

  QUICK_RANGE *res;
  size_t min = 0;
  size_t max = ranges.size() - 1;
  size_t mid = (max + min) / 2;

  while (min != max) {
    if (cmp_next(ranges[mid])) {
      /* current row value > mid->max */
      min = mid + 1;
    } else
      max = mid;
    mid = (min + max) / 2;
  }
  res = ranges[mid];
  return (!cmp_next(res) && !cmp_prev(res));
}

/*
  Compare if found key is over max-value
  Returns 0 if key <= range->max_key
  TODO: Figure out why can't this function be as simple as cmp_prev().
  At least it could use key_cmp() from key.cc, it's almost identical.
*/

int IndexRangeScanIterator::cmp_next(QUICK_RANGE *range_arg) {
  int cmp;
  if (range_arg->flag & NO_MAX_RANGE) return 0; /* key can't be to large */

  cmp = key_cmp(key_part_info, range_arg->max_key, range_arg->max_length);
  if (cmp < 0 || (cmp == 0 && !(range_arg->flag & NEAR_MAX))) return 0;
  return 1;  // outside of range
}

/*
  Returns 0 if found key is inside range (found key >= range->min_key).
*/

int IndexRangeScanIterator::cmp_prev(QUICK_RANGE *range_arg) {
  int cmp;
  if (range_arg->flag & NO_MIN_RANGE) return 0; /* key can't be to small */

  cmp = key_cmp(key_part_info, range_arg->min_key, range_arg->min_length);
  if (cmp > 0 || (cmp == 0 && !(range_arg->flag & NEAR_MIN))) return 0;
  return 1;  // outside of range
}

ParallelScanIterator::ParallelScanIterator(THD *thd, QEP_TAB *tab, TABLE *table,
                                           ha_rows *examined_rows, JOIN *join,
                                           Gather_operator *gather,
                                           bool stab_output, uint ref_length)
    : TableRowIterator(thd, table),
      m_record(table->record[0]),
      m_examined_rows(examined_rows),
      m_dop(gather->m_dop),
      m_join(join),
      m_gather(gather),
      m_record_gather(nullptr),
      m_order(nullptr),
      m_tab(tab),
      m_stable_sort(stab_output),
      m_ref_length(ref_length) {
  thd->pq_iterator = this;
}

/**
 * construct filesort on leader when needing stab_output or merge_sort
 *
 * @retavl: false if success, and otherwise true
 */
bool ParallelScanIterator::pq_make_filesort(Filesort **sort) {
  *sort = NULL;

  /** construct sort order based on group */
  if (m_join->pq_rebuilt_group) {
    assert(m_join->query_block->saved_group_list_ptrs);
    restore_list(m_join->query_block->saved_group_list_ptrs,
                 m_join->query_block->group_list);
    m_order = restore_optimized_group_order(m_join->query_block->group_list,
      m_join->saved_optimized_vars.optimized_group_flags);
  } else {
    /**
     * if sorting is built after the first rewritten table, then we have no
     *  need to rebuilt the sort order on leader, because leader will do
     *  SortingIterator. */
    if (m_join->pq_last_sort_idx >= (int)m_join->tables &&
        m_join->qep_tab[m_join->pq_last_sort_idx].filesort != nullptr) {
      return false;
    } else {
      if ((m_order = m_join->order.order) == nullptr) {
        if (m_join->m_ordered_index_usage == JOIN::ORDERED_INDEX_ORDER_BY &&
            m_join->query_block->saved_order_list_ptrs) {
          restore_list(m_join->query_block->saved_order_list_ptrs,
                       m_join->query_block->order_list);
          m_order = restore_optimized_group_order(
              m_join->query_block->order_list,
              m_join->saved_optimized_vars.optimized_order_flags);
        } else {
          std::vector<std::string> used_key_fields;
          if (get_table_key_fields(&m_join->qep_tab0[m_tab->pos],
                                   used_key_fields) ||
              DBUG_EVALUATE_IF("pq_msort_error1", true, false))
            return true;

          if (set_key_order(m_tab, used_key_fields, &m_order,
                            &m_join->ref_items[REF_SLICE_PQ_TMP]) ||
              DBUG_EVALUATE_IF("pq_msort_error2", true, false))
            return true;
        }
      }
    }
  }

  /** support stable sort on TABLE/INDEX SCAN */
  if (m_order || m_stable_sort) {
    *sort = m_tab->filesort;
    if (!(*sort)) {
      (*sort) = new (m_join->thd->pq_mem_root)
          Filesort(m_join->thd, {m_tab->table()}, false, m_order, HA_POS_ERROR,
                   false, false, false);
      if (!(*sort) || DBUG_EVALUATE_IF("pq_msort_error3", true, false))
        return true;
    }
  }
  return false;
}

/**
 * init the mq_record_gather
 */
bool ParallelScanIterator::pq_init_record_gather() {
  THD *thd = m_join->thd;
  Filesort *sort = nullptr;
  if (pq_make_filesort(&sort)) return true;

  m_record_gather = new (thd->pq_mem_root) MQ_record_gather(thd, m_tab);
  if (!m_record_gather ||
      m_record_gather->mq_scan_init(sort, m_gather->m_dop, m_ref_length,
                                    m_stable_sort) ||
      DBUG_EVALUATE_IF("pq_msort_error4", true, false))
    return true;

  /** set each worker's MQ_handle */
  for (uint i = 0; i < m_gather->m_dop; i++) {
    m_gather->m_workers[i]->m_handle =
        m_record_gather->m_exchange->get_mq_handle(i);
  }
  return false;
}

/**
 * launch worker threads
 *
 * @retval: false if success, and otherwise true
 */
bool ParallelScanIterator::pq_launch_worker() {
  THD *thd = m_join->thd;
  assert(thd == current_thd);

  Gather_operator *gather = m_tab->gather;
  PQ_worker_manager **workers = gather->m_workers;
  int launch_workers = 0;

  /** when workers encounter error during execution, directly abort the parallel
   * execution */
  for (uint i = 0; i < m_gather->m_dop; i++) {
    assert(!workers[i]->thd_worker &&
           (workers[i]->m_status == PQ_worker_state::INIT));
    if (thd->is_error() || thd->pq_error) goto err;
    my_thread_handle id;
    id.thread = 0;
    /**
     * pq_worker_error8: all workers are fialed to landuch
     * pq_worker_error9: worker's id in [0, 2, 4, ..] are failed to lanuch
     */
    if (DBUG_EVALUATE_IF("pq_worker_error8", false, true) &&
        DBUG_EVALUATE_IF("pq_worker_error9", (i % 2), true)) {
      mysql_thread_create(key_thread_parallel_query, &id, NULL, pq_worker_exec,
                          (void *)workers[i]);
    }
    workers[i]->thread_id = id;
    int expected_status = PQ_worker_state::READY | PQ_worker_state::COMPELET |
                          PQ_worker_state::PQERROR;
    if (id.thread != 0) {
      /** Record the thread id so that we can later determine whether the thread
       * started */
      workers[i]->m_active = workers[i]->wait_for_status(thd, expected_status);
      /** partial workers may fail before execution */
      if (!workers[i]->m_active ||
          DBUG_EVALUATE_IF("pq_worker_error7", (i >= m_gather->m_dop / 2),
                           false)) {
        goto err;
      }
      launch_workers++;
    } else {
      sql_print_warning("worker %d has failed to start up\n", i);
      MQueue_handle *mq_handler = m_record_gather->m_exchange->get_mq_handle(i);
      if (mq_handler) mq_handler->set_datched_status(MQ_HAVE_DETACHED);
    }
  }
  /** if all workers are not launched, then directly return false */
  if (!launch_workers) goto err;
  return false;

err:
  for (uint i = 0; i < m_gather->m_dop; i++) {
    if (workers[i]->thread_id.thread && workers[i]->thd_worker) {
      workers[i]->thd_worker->pq_error = true;
    }
  }
  return true;
}

/**
 * wait all workers finish their execution
 */
void ParallelScanIterator::pq_wait_workers_finished() {
  THD *leader_thd = m_join->thd;
  assert(leader_thd == current_thd);

  /**
   * leader first detached the message queue, and then wait workers finish
   * the execution. The reason for detach MQ is that leader has fetched the
   * satisfied #records (e.g., limit operation).
   */
  if (m_record_gather) {
    Exchange *exchange = m_record_gather->m_exchange;
    MQueue_handle *m_handle = nullptr;
    for (uint i = 0; i < m_gather->m_dop; i++) {
      if ((m_handle = exchange->get_mq_handle(i))) {
        m_handle->set_datched_status(MQ_HAVE_DETACHED);
      }
    }
  }

  /**
   * wait all such workers to finish execution, two conditions must meet:
   *  c1: the worker thread has been created
   *  c2: the worker has not yet finished
   */
  int expected_status = PQ_worker_state::COMPELET | PQ_worker_state::PQERROR;
  for (uint i = 0; i < m_gather->m_dop; i++) {
    if (m_gather->m_workers[i]->thread_id.thread != 0)  // c1
    {
      if (m_gather->m_workers[i]->m_active &&
          !(((unsigned int)m_gather->m_workers[i]->m_status) &
            PQ_worker_state::COMPELET)) {
        m_gather->m_workers[i]->wait_for_status(leader_thd, expected_status);
      }
      my_thread_join(&m_gather->m_workers[i]->thread_id, NULL);
    }
  }
}

int ParallelScanIterator::pq_error_code() {
  THD *thd = m_join->thd;

  if (m_gather->m_ha_err == HA_ERR_TABLE_DEF_CHANGED) {
    m_gather->m_ha_err = 0;
    return HA_ERR_TABLE_DEF_CHANGED;
  }

  if (thd->is_killed()) {
    thd->send_kill_message();
  }

  /** collect worker threads status from DA info */
  JOIN *tmplate_join = m_gather->m_template_join;
  THD *temp_thd = tmplate_join->thd;
  thd->pq_status_reset();
  thd->pq_merge_status(temp_thd);
  Diagnostics_area *da = temp_thd->get_stmt_da();
  if (temp_thd->is_error()) {
    temp_thd->raise_condition(da->mysql_errno(), da->returned_sqlstate(),
                              Sql_condition::SL_ERROR, da->message_text());
  }

  if (da->cond_count() > 0) {
    Diagnostics_area::Sql_condition_iterator it = da->sql_conditions();
    const Sql_condition *cond;
    while ((cond = it++)) {
      thd->raise_condition(cond->mysql_errno(), NULL, cond->severity(),
                           cond->message_text());
    }
  }
  /**  output parallel error code */
  if (!temp_thd->is_error() && !thd->is_error() && thd->pq_error &&
      !thd->running_explain_analyze) {
    my_error(ER_PARALLEL_EXEC_ERROR, MYF(0));
  }
  return 1;
}

bool ParallelScanIterator::Init() {
  assert(current_thd == m_join->thd);
  m_gather->waitReadEnd();
  if (m_gather->init() ||        /** cur innodb data,
                                     should be called first(will change dop based on
                                    split count) */
      pq_init_record_gather() || /** init mq_record_gather */
      pq_launch_worker() ||      /** launch worker threads */
      DBUG_EVALUATE_IF("pq_worker_error6", true, false)) {
    m_join->thd->pq_error = true;
    return true;
  }
  return false;
}

int ParallelScanIterator::Read() {
  /** kill query */
  if (m_join->thd->is_killed()) {
    m_join->thd->send_kill_message();
    return 1;
  }
  /** fetch message from MQ to table->record[0] */
  if (m_record_gather->mq_scan_next()) return 0;
  return -1;
}

int ParallelScanIterator::End() {
  m_gather->signalReadEnd();
  /** wait all workers to finish their execution */
  pq_wait_workers_finished();
  /** output error code */
  return pq_error_code();
}

ParallelScanIterator::~ParallelScanIterator() {
  table()->file->ha_index_or_rnd_end();
  /** cleanup m_record_gather */
  if (m_record_gather) {
    m_record_gather->mq_scan_end();
  }
}

PQblockScanIterator::PQblockScanIterator(THD *thd, TABLE *table, uchar *record,
                                         ha_rows *examined_rows,
                                         Gather_operator *gather,
                                         bool need_rowid)
    : TableRowIterator(thd, table),
      m_record(record),
      m_examined_rows(examined_rows),
      m_pq_ctx(gather->m_pq_ctx),
      keyno(gather->keyno),
      m_gather(gather),
      m_need_rowid(need_rowid) {
  thd->pq_iterator = this;
}

bool PQblockScanIterator::Init() {
  table()->file->pq_worker_scan_init(keyno, m_pq_ctx);
  return false;
}

int PQblockScanIterator::End() {
  assert(thd() && thd()->pq_leader);
  if (m_gather) m_gather->signalAll();
  return -1;
}

PQblockScanIterator::~PQblockScanIterator() {
}

int PQblockScanIterator::Read() {
  int tmp;
  while ((tmp = table()->file->ha_pq_next(m_record, m_pq_ctx))) {
    /*
      ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is
      reading and another deleting without locks.
    */
    if (tmp == HA_ERR_RECORD_DELETED && !thd()->killed) continue;
    return HandleError(tmp);
  }

  if (m_examined_rows != nullptr) {
    ++*m_examined_rows;
  }
  // write row_id into file
  if (m_need_rowid) {
    assert(table()->file->ht->db_type == DB_TYPE_INNODB);
    assert(table()->record[0] == m_record);
    table()->file->position(m_record);
  }

  return 0;
}