/* Copyright (c) 2013, 2020, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2022, Huawei Technologies Co., Ltd.

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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.*/

#include "sql/sql_parallel.h"
#include "include/my_alloc.h"
#include "include/my_dbug.h"
#include "include/mysql/psi/mysql_thread.h"
#include "sql/auth/auth_acls.h"

#include "sql/filesort.h"
#include "sql/handler.h"
#include "sql/item_sum.h"
#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/timing_iterator.h"
#include "sql/join_optimizer/explain_access_path.h"
#include "sql/log.h"

#include "sql/mysqld.h"
#include "sql/mysqld_thd_manager.h"  // Global_THD_manager
#include "sql/opt_trace.h"

#include "sql/query_result.h"
#include "sql/range_optimizer/path_helpers.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/sql_base.h"
#include "sql/sql_exchange_nosort.h"
#include "sql/sql_exchange_sort.h"
#include "sql/sql_message_queue.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_parallel_global.h"
#include "sql/sql_pq_clone.h"
#include "sql/sql_pq_condition.h"
#include "sql/sql_pq_range.h"
#include "sql/sql_tmp_table.h"
#include "sql/transaction.h"

ulonglong parallel_memory_limit = 0;
ulong parallel_max_threads = 0;
uint parallel_threads_running = 0;
uint parallel_threads_refused = 0;
uint parallel_memory_refused = 0;
uint pq_memory_used[PQ_MEMORY_USED_BUCKET] = {0};
uint pq_memory_total_used = 0;

mysql_mutex_t LOCK_pq_threads_running;
mysql_cond_t COND_pq_threads_running;

Item *make_cond_for_index(Item *cond, TABLE *table, uint keyno, bool other_tbls_ok);

Item *make_cond_remainder(Item *cond, bool exclude_index);

void thd_set_thread_stack(THD *thd, const char *stack_start);

static JOIN *make_pq_worker_plan(PQ_worker_manager *mngr);

void release_pq_running_threads(uint dop) {
  mysql_mutex_lock(&LOCK_pq_threads_running);
  parallel_threads_running -= dop;
  current_thd->pq_threads_running -= dop;
  mysql_cond_broadcast(&COND_pq_threads_running);
  mysql_mutex_unlock(&LOCK_pq_threads_running);
}

/**
 * Init record gather
 *
 * @retval: false if success, and otherwise true
 */
bool MQ_record_gather::mq_scan_init(Filesort *sort, int workers, uint ref_length, bool stab_output) {
  if (sort) {
    m_exchange = new (m_thd->pq_mem_root)
        Exchange_sort(m_thd, m_tab->table(), sort, m_tab->old_table()->file, workers, ref_length, stab_output);
  } else {
    m_exchange = new (m_thd->pq_mem_root) Exchange_nosort(m_thd, m_tab->table(), workers, ref_length, stab_output);
  }

  if (!m_exchange || m_exchange->init()) return true;

  return false;
}

/**
 * read table->record[0] from workers through message queue
 *
 * @retval: false if success, and otherwise true
 */

bool MQ_record_gather::mq_scan_next() {
  assert(m_exchange);
  return (m_exchange->read_mq_record());
}

void MQ_record_gather::mq_scan_end() {
  assert(m_exchange);
  m_exchange->cleanup();
}

PQ_worker_manager::PQ_worker_manager(uint id)
    : m_id(id),
      m_gather(nullptr),
      thd_leader(nullptr),
      thd_worker(nullptr),
      m_handle(nullptr),
      m_status(INIT),
      m_active(false) {
  mysql_mutex_init(0, &m_mutex, MY_MUTEX_INIT_FAST);
  mysql_cond_init(0, &m_cond);
}

PQ_worker_manager::~PQ_worker_manager() {
  mysql_mutex_destroy(&m_mutex);
  mysql_cond_destroy(&m_cond);
}

/*
 * PQ worker wait for an status
 *
 * @leader: PQ leader thread
 * @status:
 *
 * @retval:
 *    true if normally execute, and otherwise false (i.e., execution-error)
 */

bool PQ_worker_manager::wait_for_status(THD *leader MY_ATTRIBUTE((unused)), uint status) {
  assert(leader == current_thd);
  mysql_mutex_lock(&m_mutex);
  while (!(((unsigned int)this->m_status) & status)) {
    struct timespec abstime;
    Timeout_type wait_time = 5;
    set_timespec(&abstime, wait_time * TIME_MILLION);
    mysql_cond_timedwait(&m_cond, &m_mutex, &abstime);
  }
  mysql_mutex_unlock(&m_mutex);
  return !(((unsigned int)this->m_status) & PQ_worker_state::PQERROR);
}

void PQ_worker_manager::signal_status(THD *thd, PQ_worker_state status) {
  mysql_mutex_lock(&m_mutex);
  if (!((unsigned int)this->m_status & (unsigned int)status)) {
    this->m_status = status;
    this->thd_worker = thd;
  }
  mysql_cond_signal(&m_cond);
  mysql_mutex_unlock(&m_mutex);
}

Gather_operator::Gather_operator(uint dop)
    : m_dop(dop),
      m_template_join(nullptr),
      m_workers(nullptr),
      m_table(nullptr),
      m_pq_ctx(nullptr),
      m_tab(nullptr),
      keyno(0),
      m_ha_err(0),
      m_stmt_da(false),
      m_code_state(nullptr),
      table_scan(false) {
  mysql_mutex_init(0, &lock_stmt_da, MY_MUTEX_INIT_FAST);
}

Gather_operator::~Gather_operator() { mysql_mutex_destroy(&lock_stmt_da); }

/*
 * replace the parameter item of Aggr. with the generated item in rewritten tab.
 *
 */
void pq_replace_avg_func(THD *thd, Query_block *select MY_ATTRIBUTE((unused)), mem_root_deque<Item *> *fields,
                         nesting_map select_nest_level MY_ATTRIBUTE((unused))) {
  size_t i = 0;
  for (Item *item : *fields) {
    if (item->real_item()->type() == Item::SUM_FUNC_ITEM) {
      Item_sum *item_old = (Item_sum *)(item->real_item());
      assert(item_old);
      if (item_old->sum_func() == Item_sum::AVG_FUNC) {
        Item_sum_avg *item_avg = dynamic_cast<Item_sum_avg *>(item_old);
        assert(item_avg);
        item_avg->pq_avg_type = ParallelAvgType::PQ_LEADER;
        item_avg->resolve_type(thd);
      }
    } else if (item->real_item()->type() == Item::FIELD_AVG_ITEM) {
      Item_avg_field *item_avg_field = dynamic_cast<Item_avg_field *>(item->real_item());
      Item_sum_avg *item_avg = item_avg_field->avg_item;
      item_avg->pq_avg_type = ParallelAvgType::PQ_LEADER;
      item_avg_field->pq_avg_type = ParallelAvgType::PQ_LEADER;
      item_avg->resolve_type(thd);
      (*fields)[i] = item_avg;

    } else if (item->real_item()->type() == Item::FIELD_ITEM) {
      Item_field *item_field = dynamic_cast<Item_field *>(item->real_item());
      Item_sum *item_sum = item_field->field->item_sum_ref;
      if (item_sum && item_sum->sum_func() == Item_sum::AVG_FUNC) {
        Item_sum_avg *item_avg = down_cast<Item_sum_avg *>(item_sum);
        item_avg->pq_avg_type = ParallelAvgType::PQ_LEADER;
        item_avg->resolve_type(thd);
        (*fields)[i] = item_avg;
      }
    }
    i++;
  }
}

/**
 * build sum funcs based on PQ leader temp table field when orig JOIN old
 * fields_list contain sum funcs. because origin sum item has been replaced by
 * Item_field in temp table fields list
 *
 * @fields_orig：orig fields list which could contain sum funs item
 * @fields_new: PQ leader temp table's fields list
 *
 */
bool pq_build_sum_funcs(THD *thd, Query_block *select, Ref_item_array &ref_ptr, mem_root_deque<Item *> &fields,
                        uint elements, nesting_map select_nest_level) {
  uint saved_allow_sum_funcs = thd->lex->allow_sum_func;
  thd->lex->allow_sum_func |= select_nest_level;
  uint border = fields.size() - elements;

  size_t i = 0;
  for (Item *item : fields) {
    if (item->real_item()->type() == Item::FIELD_ITEM) {
      Item_field *item_field = dynamic_cast<Item_field *>(item);

      if (item_field == nullptr || item_field->field == nullptr || item_field->field->item_sum_ref == nullptr) {
        i++;
        continue;
      }

      Item_sum *item_ref = item_field->field->item_sum_ref;

      if (item_ref->type() == Item::SUM_FUNC_ITEM) {
        Item_sum *sum_func = item_ref->pq_rebuild_sum_func(thd, select, item_field);
        assert(DBUG_EVALUATE_IF("skip_pq_clone_check", true, false) || sum_func);
        if (!sum_func) {
          thd->lex->allow_sum_func = saved_allow_sum_funcs;
          return true;
        }
        sum_func->fix_fields(thd, nullptr);
        item_field->field->item_sum_ref = sum_func;
        fields[i] = sum_func;
        ref_ptr[((i < border) ? fields.size() - i - 1 : i - border)] = sum_func;
      }
    }
    i++;
  }
  thd->lex->allow_sum_func = saved_allow_sum_funcs;
  return false;
}

THD *pq_new_thd(THD *thd) {
  DBUG_TRACE;

  THD *new_thd = new (thd->pq_mem_root) THD();
  if (!new_thd || DBUG_EVALUATE_IF("dup_thd_abort", (!(const_cast<NET *>(new_thd->get_net())->error = 0)), false)) {
    goto err;
  }

  new_thd->set_new_thread_id();
  thd_set_thread_stack(new_thd, (char *)&new_thd);
  new_thd->init_cost_model();
  new_thd->store_globals();
  new_thd->want_privilege = 0;
  const_cast<NET *>(new_thd->get_net())->error = 0;
  new_thd->set_db(thd->db());
  new_thd->pq_copy_from(thd);

  return new_thd;

err:
  if (new_thd) {
    end_connection(new_thd);
    close_connection(new_thd, 0, false, false);
    new_thd->release_resources();
    new_thd->get_stmt_da()->reset_diagnostics_area();
    destroy(new_thd);
  }
  return nullptr;
}

/**
 * make a parallel query gather operator from a serial query plan
 *
 * @join : is a pysics serial query plan
 * @table : table need to parallel scan
 * @dop is : the degree of parallel
 *
 */
Gather_operator *make_pq_gather_operator(JOIN *join, QEP_TAB *tab, uint dop) {
  THD *thd = current_thd;
  assert(thd == join->thd && thd->parallel_exec);
  JOIN *template_join = nullptr;
  Gather_operator *gather_opr = nullptr;

  // duplicate a query plan template from join, which is used in PQ workers
  THD *new_thd = pq_new_thd(join->thd);
  if (!new_thd) goto err;
  new_thd->pq_leader = thd;
  new_thd->parallel_exec = true;
  template_join = pq_make_join(new_thd, join);

  if (!template_join || pq_dup_tabs(template_join, join, true)) {
    goto err;
  }

  if (template_join->setup_tmp_table_info(join) || DBUG_EVALUATE_IF("pq_gather_error1", true, false)) {
    sql_print_warning("[Parallel query] Setup gather tmp tables failed");
    goto err;
  }

  /** duplicate a new THD and set it as current_thd, so here should restore old
   * THD */
  thd->store_globals();
  gather_opr = new (thd->pq_mem_root) Gather_operator(dop);

  if (!gather_opr || DBUG_EVALUATE_IF("pq_gather_error2", true, false)) {
    goto err;
  }

  tab->gather = gather_opr;
  gather_opr->m_template_join = template_join;
  gather_opr->m_tab = tab;
  gather_opr->m_table = tab->table();
#ifndef NDEBUG
  gather_opr->m_code_state = my_thread_var_dbug();
  assert(gather_opr->m_code_state && *(gather_opr->m_code_state));
#endif
  template_join->thd->push_diagnostics_area(&gather_opr->m_stmt_da);

  gather_opr->m_workers = thd->pq_mem_root->ArrayAlloc<PQ_worker_manager *>(dop);

  if (!gather_opr->m_workers || DBUG_EVALUATE_IF("pq_gather_error3", (!(gather_opr->m_workers = nullptr)), false)) {
    goto err;
  }

  for (uint i = 0; i < dop; i++) {
    gather_opr->m_workers[i] = new (thd->pq_mem_root) PQ_worker_manager(i);
    if (!gather_opr->m_workers[i]) goto err;
    gather_opr->m_workers[i]->m_gather = gather_opr;
    gather_opr->m_workers[i]->thd_leader = thd;
    gather_opr->m_workers[i]->thd_worker = nullptr;
    gather_opr->m_workers[i]->thread_id.thread = 0;
  }
  return gather_opr;

err:
  if (new_thd) new_thd->store_globals();
  pq_free_join(template_join);
  pq_free_thd(new_thd);
  if (gather_opr && gather_opr->m_workers) {
    for (uint i = 0; i < dop; i++) {
      destroy(gather_opr->m_workers[i]);
    }
  }
  destroy(gather_opr);
  thd->store_globals();
  return nullptr;
}

bool Gather_operator::init() {
  int error = 0;
  THD *thd = current_thd;
  assert(thd == m_table->in_use);
  int tab_idx = m_template_join->pq_tab_idx;
  assert(tab_idx >= (int)m_template_join->const_tables && m_template_join->qep_tab[tab_idx].do_parallel_scan);

  QEP_TAB *tab = &m_template_join->qep_tab[tab_idx];
  m_table->file->pq_reverse_scan = tab->m_reversed_access;
  m_table->file->pq_range_type = PQ_RANGE_TYPE::PQ_QUICK_SELECT_NONE;
  join_type type = tab->type();
  switch (type) {
    case JT_ALL:
      keyno = m_table->s->primary_key;
      /*
       * Note that: order/group-by may be optimized in test_skip_sort(), and
       * correspondingly the order/group-by is finished with the generated
       * tab->quick().
       */
      if (m_tab->range_scan() &&
          m_template_join->m_ordered_index_usage != JOIN::ORDERED_INDEX_USAGE::ORDERED_INDEX_VOID) {
        keyno = used_index(m_tab->range_scan());
      }
      table_scan = true;
      break;
    case JT_RANGE:
      assert(m_tab->range_scan());
      keyno = used_index(m_tab->range_scan());
      break;
    case JT_REF:
      m_table->file->pq_ref_key.key = tab->ref().key_buff;
      m_table->file->pq_ref_key.keypart_map = make_prev_keypart_map(tab->ref().key_parts);
      m_table->file->pq_ref_key.length = tab->ref().key_length;
      m_table->file->pq_ref_key.flag = HA_READ_KEY_OR_NEXT;
      m_table->file->pq_ref = true;
      keyno = tab->ref().key;
      break;
    case JT_INDEX_SCAN:
      keyno = m_tab->index();
      break;
    default:
      assert(0);
      keyno = m_table->s->primary_key;
  }

  if (m_tab->range_scan() && type == JT_RANGE) {
    m_table->file->pq_range_type = m_tab->range_scan()->quick_select_type();
    AccessPath *range = m_tab->range_scan();

    unique_ptr_destroy_only<RowIterator> m_iterator;
    m_iterator = CreateIteratorFromAccessPath(m_tab->join()->thd, range, m_tab->join(),
                                              /*eligible_for_batch_mode=*/true);

    if (m_iterator == nullptr || m_iterator->Init()) return true;

    if (is_reverse_sorted_range(range)) m_table->file->pq_reverse_scan = true;
  }

  /** partition table into blocks for parallel scan by multiple workers,
   *  if blocks number is less than workers, m_dop will be changed to blocks
   * number
   */
  uint dop_orig = m_dop;
  error = m_table->file->ha_pq_init(keyno, m_dop);
  m_pq_ctx = thd->pq_ctx;
  if (error) {
    m_table->file->print_error(error, MYF(0));
    m_ha_err = error;
    return true;
  }

  if (dop_orig != m_dop) {
    release_pq_running_threads(dop_orig - m_dop);
  }

  return false;
}

/*
 * signal the threads waiting for data
 */
void Gather_operator::signalAll() { m_table->file->ha_pq_signal_all(); }

void Gather_operator::signalReadEnd() { m_read_end_mutex.unlock(); }

void Gather_operator::waitReadEnd() { m_read_end_mutex.lock(); }

void pq_free_gather(Gather_operator *gather) {
  THD *thd_temp = gather->m_template_join->thd;
  if (thd_temp == nullptr) return;

  THD *saved_thd = current_thd;
  gather->m_table->file->ha_index_or_rnd_end();

  // explain format=tree called make_pq_worker_plan, hence
  // need to free worker join/thd
  for (uint i = 0; i < gather->m_dop; i++) {
    if (gather->m_workers[i]->thd_worker) {
      gather->m_workers[i]->thd_worker->store_globals();
      gather->m_workers[i]->thd_worker->lex->unit->cleanup(true);
      pq_free_thd(gather->m_workers[i]->thd_worker);
    }
  }

  thd_set_thread_stack(thd_temp, (char *)thd_temp);
  thd_temp->store_globals();

  uint tables = gather->m_template_join->tables;
  for (uint i = 0; i < tables; i++) {
    if (gather->m_template_join->qep_tab[i].table()) {
      gather->m_template_join->qep_tab[i].table()->set_keyread(false);
      gather->m_template_join->qep_tab[i].set_keyread_optim();
    }
  }

  pq_free_join(gather->m_template_join);
  for (uint i = 0; i < gather->m_dop; i++) {
    destroy(gather->m_workers[i]);
  }
  destroy(gather);

  pq_free_thd(thd_temp);
  thd_set_thread_stack(saved_thd, (char *)&saved_thd);
  saved_thd->store_globals();
}

void Gather_operator::end() { pq_free_gather(this); }

static void restore_leader_plan(JOIN *join) {
  join->pq_stable_sort = false;
  join->qep_tab = join->qep_tab0;
  join->ref_items = join->ref_items0;
  join->tmp_fields = join->tmp_fields0;
}

/**
 * make parallel query leader's physical query plan
 *
 * @join : origin serial query plan
 * @dop : degree of parallel
 * @return
 *
 *    SEQ_EXEC:  can not run in parallel mode, due to RBO.
 *    PARL_EXEC: successfully run  in parallel mode
 *    ABORT_EXEC: run error in parallal mode and then drop it
 */
PQ_exec_status make_pq_leader_plan(THD *thd) {
  if (!check_pq_conditions(thd)) {
    thd->m_suite_for_pq = PQ_ConditionStatus::NOT_SUPPORTED;
    return PQ_exec_status::SEQ_EXEC;
  } else {
    thd->m_suite_for_pq = PQ_ConditionStatus::SUPPORTED;
  }

  uint dop = thd->pq_dop;
  JOIN *join = thd->lex->unit->first_query_block()->join;
  mem_root_deque<Item *> *fields_old = join->fields;
  QEP_TAB *tab = nullptr;
  Gather_operator *gather = nullptr;
  char buff[64] = {0};
  ulong saved_thd_want_privilege = thd->want_privilege;
  thd->want_privilege = 0;

  Opt_trace_context *const trace = &thd->opt_trace;
  Opt_trace_object trace_wrapper(trace);
  Opt_trace_object trace_exec(trace, "make_parallel_query_plan");
  trace_exec.add_select_number(join->query_block->select_number);
  Opt_trace_array trace_detail(trace, "detail");

  MEM_ROOT *saved_mem_root = thd->mem_root;
  thd->mem_root = thd->pq_mem_root;
  thd->parallel_exec = true;  // pass the RBO

  uint tab_idx = 0;
  for (uint i = join->const_tables; i < join->primary_tables; i++) {
    if (join->qep_tab[i].do_parallel_scan) {
      tab_idx = i;
      join->pq_tab_idx = tab_idx;
      break;
    }
  }

  if (tab_idx < join->primary_tables) {
    Table_ref *table_ref = join->qep_tab[tab_idx].table_ref;
    Opt_trace_object trace_one_table(trace);
    trace_one_table.add_utf8_table(table_ref).add("degree of parallel", dop);

    join->alloc_qep1(join->tables);
    join->alloc_indirection_slices1();
    join->ref_items1[REF_SLICE_ACTIVE] = join->query_block->base_ref_items;

    join->pq_stable_sort = pq_check_stable_sort(join, tab_idx);
    gather = make_pq_gather_operator(join, &join->qep_tab[tab_idx], dop);
    if (!gather || DBUG_EVALUATE_IF("pq_leader_abort1", true, false)) {
      goto err;
    }
    assert(gather->m_template_join->thd->pq_leader);
    tab = &join->qep_tab[tab_idx];
    tab->set_old_table(tab->table());
    // replace parallel scan table with a tmp table
    join->primary_tables = tab_idx;
    join->need_tmp_pq_leader = true;
    join->restore_optimized_vars();
    if (join->make_leader_tables_info() || DBUG_EVALUATE_IF("pq_leader_abort2", true, false)) goto err1;
    join->old_tables = join->tables;
    join->tables = join->primary_tables + join->tmp_tables;

    assert(tab->table()->s->table_category == TABLE_CATEGORY_TEMPORARY);
    tab->set_type(JT_ALL);
    tab->gather = gather;
    tab->check_weed_out_table = nullptr;
    tab->flush_weedout_table = nullptr;

    // create TABLE_LIST object for explain
    Table_ref *tbl = new (thd->pq_mem_root) Table_ref;
    if (!tbl) goto err;

    tbl->query_block = join->query_block;
    tbl->table_name = (char *)thd->memdup(tab->table()->s->table_name.str, tab->table()->s->table_name.length);
    tbl->table_name_length = tab->table()->s->table_name.length;
    tbl->db = (char *)thd->memdup(tab->table()->s->db.str, tab->table()->s->db.length);
    tbl->db_length = tab->table()->s->db.length;
    snprintf(buff, 64, "<gather%d>", gather->m_template_join->query_block->select_number);
    tbl->alias = (char *)thd->memdup(buff, 64);
    if (!tbl->table_name || !tbl->db || !tbl->alias) goto err;

    tab->table_ref = tbl;
    tbl->set_tableno(tab_idx);
    tab->table()->pos_in_table_list = tbl;
    join->query_block->m_table_list.link_in_list(tbl, &tbl->next_local);
    Index_lookup *ref = new (thd->pq_mem_root) Index_lookup();
    if (!ref) goto err;

    tab->set_ref(ref);
    for (uint i = 0; i < join->tables; i++) {
      join->qep_tab[i].set_condition(nullptr);
      // we have shrinked primary_tables, so we update position here
      if (i > join->primary_tables) {
        join->qep_tab[i].set_position(nullptr);
      }
    }

    join->query_expression()->clear_root_access_path();

    // generate execution tree
    join->get_root_access_path() = nullptr;
    join->create_access_paths();
    join->query_expression()->create_access_paths(thd);
    if (join->query_expression()->force_create_iterators(thd)) goto err1;

    thd->mem_root = saved_mem_root;
    thd->want_privilege = saved_thd_want_privilege;

    if (thd->lex->is_explain()) {
      // Treat the first tmp_table which obtained info from worker as
      // primary_table, because explain format=json need `primary_tables >= 1`
      join->primary_tables += 1;
      join->tmp_tables -= 1;

      // explain format=[traditional/tree/json] need real dop, so we call
      // gather->init here, explain format=analyze will call gather->init, do
      // not need call here
      if (!thd->lex->is_explain_analyze) {
        gather->init();
      }

      // explain format=tree need generate worker plan first
      if (thd->lex->explain_format->is_iterator_based() && !thd->lex->is_explain_analyze) {
        JOIN *worker_join = make_pq_worker_plan(gather->m_workers[0]);
        gather->m_workers[0]->thd_worker = worker_join->thd;

        /** make_pq_worker_plan will duplicate a new THD and set it as
         * current_thd, so here should restore old THD
         */
        thd->store_globals();
      }
    }

    return PQ_exec_status::PARL_EXEC;
  }

err:
  thd->mem_root = saved_mem_root;
  if (gather) pq_free_gather(gather);

  join->fields = fields_old;
  thd->want_privilege = saved_thd_want_privilege;
  restore_leader_plan(join);
  thd->pq_iterator = nullptr;

  // my_error(ER_PARALLEL_FAIL_INIT, MYF(0));
  return PQ_exec_status::SEQ_EXEC;

err1:
  thd->mem_root = saved_mem_root;
  if (gather) pq_free_gather(gather);

  join->fields = fields_old;
  thd->want_privilege = saved_thd_want_privilege;
  restore_leader_plan(join);

  my_error(ER_PARALLEL_FAIL_INIT, MYF(0));
  return PQ_exec_status::ABORT_EXEC;
}

/**
 * make a parallel query worker's pysics query plan
 *
 * @template_join : template query plan
 */
static JOIN *make_pq_worker_plan(PQ_worker_manager *mngr) {
  JOIN *join = nullptr;
  Gather_operator *gather = mngr->m_gather;
  JOIN *template_join = gather->m_template_join;
  handler *file = nullptr;
  Query_result *mq_result = nullptr;
  MQueue_handle *msg_handler = mngr->m_handle;

  // duplicate a query plan from template join, which is used in PQ workers
  THD *new_thd = pq_new_thd(template_join->thd);
  if (!new_thd) goto err;
  new_thd->pq_leader = mngr->thd_leader;
  new_thd->mem_root = new_thd->pq_mem_root;

  /* save worker's THD in leader's THD */
  mysql_mutex_lock(&mngr->thd_leader->pq_lock_worker);
  if (mngr->thd_leader->killed) {
    mysql_mutex_unlock(&mngr->thd_leader->pq_lock_worker);
    goto err;
  }
  mngr->thd_leader->pq_workers.push_back(new_thd);
  mysql_mutex_unlock(&mngr->thd_leader->pq_lock_worker);

  join = pq_make_join(new_thd, template_join);
  if (!join || pq_dup_tabs(join, template_join, true) || DBUG_EVALUATE_IF("pq_worker_abort1", true, false)) {
    goto err;
  }

  join->having_cond = join->query_block->having_cond();
  join->need_tmp_pq = true;
  if (join->setup_tmp_table_info(template_join) || join->make_tmp_tables_info() ||
      pq_make_join_readinfo(join, mngr, join->saved_optimized_vars.pq_no_jbuf_after) ||
      DBUG_EVALUATE_IF("pq_worker_abort2", true, false)) {
    sql_print_warning(
        "[Parallel query] Create worker tmp tables or make join read info "
        "failed");
    goto err;
  }

  /** set query result */
  file = join->qep_tab[join->pq_tab_idx].table()->file;
  mq_result = new (join->thd->pq_mem_root) Query_result_mq(join, msg_handler, file, join->pq_stable_sort);
  if (!mq_result || DBUG_EVALUATE_IF("pq_worker_error2", true, false)) {
    sql_print_warning("[Parallel query] Create worker result mq failed");
    goto err;
  }

  join->query_expression()->set_query_result(mq_result);
  join->query_block->set_query_result(mq_result);

  return join;

err:
  if (new_thd) new_thd->store_globals();
  pq_free_join(join);
  pq_free_thd(new_thd);
  mngr->thd_leader->store_globals();
  return nullptr;
}

/**
 * main function of parallel worker.
 *
 */
void *pq_worker_exec(void *arg) {
  if (my_thread_init()) {
    my_thread_exit(0);
    return nullptr;
  }

  Diagnostics_area *da;
  const Sql_condition *cond;
  /** only for single query block */
  Query_result *result = nullptr;
  THD *thd = nullptr, *leader_thd = nullptr;

  PQ_worker_manager *mngr = static_cast<PQ_worker_manager *>(arg);
  assert(mngr->m_gather);
#ifndef NDEBUG
  db_stack_copy(*mngr->m_gather->m_code_state);
#endif
  leader_thd = mngr->thd_leader;
  THD *temp_thd = mngr->m_gather->m_template_join->thd;
  MQueue_handle *msg_handler = mngr->m_handle;
  bool send_error_status = true;

  JOIN *join = make_pq_worker_plan(mngr);
  if (!join || DBUG_EVALUATE_IF("pq_worker_error1", true, false)) {
    sql_print_warning("[Parallel query] Make worker plan failed");
    goto err;
  }

  thd = join->thd;
  assert(current_thd == thd && thd->pq_leader == leader_thd);
  mngr->signal_status(thd, PQ_worker_state::READY);
  join->query_expression()->ExecuteIteratorQuery(thd);

  if (thd->lex->is_explain_analyze && mngr->m_id == 0) {
    msg_handler->set_datched_status(MQ_HAVE_DETACHED);
    mngr->m_gather->waitReadEnd();
    Query_expression *unit = leader_thd->lex->unit;
    leader_thd->pq_explain += PrintQueryPlan(
        0, unit->root_access_path(), unit->is_union() ? nullptr : unit->first_query_block()->join, !unit->is_union());
  }

  if (join->thd->is_error() || join->thd->pq_error || DBUG_EVALUATE_IF("pq_worker_error3", true, false)) {
    goto err;
  }
  send_error_status = false;

err:

  /* s1: send error msg to MQ */
  if (send_error_status) {
    assert(msg_handler && leader_thd);
    leader_thd->pq_error = true;
    msg_handler->send_exception_msg(ERROR_MSG);
  }
  msg_handler->set_datched_status(MQ_HAVE_DETACHED);
  /** during pq_make_join, join->thd may have been created */
  thd = (join && join->thd) ? join->thd : thd;

  /* s2: release resource */
  result = join ? join->query_block->query_result() : nullptr;
  if (result) {
    result->cleanup();
    destroy(result);
  }

  if (join) {
    join->join_free();
  }

  if (thd) {
    thd->lex->unit->cleanup(true);
  }

  /* s3: collect error message */
  if (thd) {
    mysql_mutex_t *stmt_lock = &mngr->m_gather->lock_stmt_da;
    mysql_mutex_lock(stmt_lock);
    temp_thd->pq_merge_status(thd);
    da = thd->get_stmt_da();
    if (thd->is_error()) {
      temp_thd->raise_condition(da->mysql_errno(), da->returned_sqlstate(), Sql_condition::SL_ERROR, da->message_text(),
                                false);
    }
    if (da->cond_count() > 0) {
      Diagnostics_area::Sql_condition_iterator it = da->sql_conditions();
      while ((cond = it++)) {
        temp_thd->raise_condition(cond->mysql_errno(), NULL, cond->severity(), cond->message_text(), false);
      }
    }
    mysql_mutex_unlock(stmt_lock);
    pq_free_thd(thd);
    thd = NULL;
  }

#ifndef NDEBUG
  db_stack_reset();
#endif

  /* Clean up openssl errors. */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
  ERR_remove_thread_state(0);
#endif

  my_thread_end();
  /* s4: send last status to leader */
  PQ_worker_state status = send_error_status ? PQ_worker_state::PQERROR : PQ_worker_state::COMPELET;
  mngr->signal_status(thd, status);
  my_thread_exit(0);
  return nullptr;
}

/**
Plan refinement stage: do various setup things for the executor, including
  - setup join buffering use
  - push index conditions
  - increment relevant counters
  - etc

@return false if successful, true if error (Out of memory)
*/
bool pq_make_join_readinfo(JOIN *join, PQ_worker_manager *mngr, uint no_jbuf_after MY_ATTRIBUTE((unused))) {
  const bool prep_for_pos = join->need_tmp_before_win || join->select_distinct || join->grouped ||
                            !join->order.empty() || join->m_windows.elements > 0;

  for (uint i = join->const_tables; i < join->primary_tables; i++) {
    QEP_TAB *const qep_tab = &join->qep_tab[i];
    TABLE *const table = qep_tab->table();
    if (prep_for_pos || (qep_tab->do_parallel_scan && join->pq_stable_sort)) table->prepare_for_position();
  }
  std::vector<Item *> predicates_below_join;
  std::vector<PendingCondition> predicates_above_join;
  join->get_root_access_path() = nullptr;
  Gather_operator *gather = mngr->m_gather;

  for (uint i = 0; i < join->tables; i++) {
    QEP_TAB *qep_tab = &join->qep_tab[i];
    if (qep_tab->do_parallel_scan) {
      qep_tab->table()->file->pq_table_scan = gather->table_scan;
      qep_tab->gather = mngr->m_gather;

      /* index push down */
      uint keyno = gather->keyno;
      if (!(keyno == qep_tab->table()->s->primary_key && qep_tab->table()->file->primary_key_is_clustered()) &&
          qep_tab->pq_cond) {
        TABLE *tbl = qep_tab->table();
        Item *cond = qep_tab->pq_cond;
        Item *idx_cond = make_cond_for_index(cond, tbl, keyno, false);
        if (idx_cond) {
          Item *idx_remainder_cond = tbl->file->idx_cond_push(keyno, idx_cond);
          if (idx_remainder_cond != idx_cond) qep_tab->ref().disable_cache = true;

          Item *row_cond = make_cond_remainder(qep_tab->pq_cond, true);
          if (row_cond) {
            if (idx_remainder_cond) and_conditions(&row_cond, idx_remainder_cond);
            idx_remainder_cond = row_cond;
          }
          qep_tab->set_condition(idx_remainder_cond);
        }
      }

      /** optimize order by */
      if (join->pq_last_sort_idx == int(i) && i >= join->primary_tables) {
        assert(qep_tab->filesort);
        /** if there is limit on tmp table, we cannot remove sort */
        if (join->m_select_limit == HA_POS_ERROR) {
          destroy(qep_tab->filesort);
          qep_tab->filesort = nullptr;
        }

        if (join->pq_rebuilt_group) {
          assert(join->query_block->saved_group_list_ptrs);
          assert(join->m_select_limit == HA_POS_ERROR);
          restore_list(join->query_block->saved_group_list_ptrs, join->query_block->group_list);
          ORDER *order = restore_optimized_group_order(join->query_block->group_list,
                                                       join->saved_optimized_vars.optimized_group_flags);
          if (order) {
            ORDER_with_src group_list = ORDER_with_src(order, ESC_GROUP_BY);
            join->add_sorting_to_table(i, &group_list, true);
          }
        }
      }
    }
  }

  /** generate execution tree */
  join->set_optimized();
  join->query_expression()->set_optimized();
  join->create_access_paths();
  join->query_expression()->create_access_paths(join->thd);
  join->query_expression()->force_create_iterators(join->thd);

  return false;
}

bool pq_check_stable_sort(JOIN *join, int idx) {
  if ((join->pq_last_sort_idx >= 0 && join->pq_last_sort_idx != idx) || join->need_tmp_before_win) {
    return false;
  }
  return true;
}

/*
 * record the mapping:
 *      L = join->query_block->group_list -------> join->group_list = R
 *
 * @result:
 *    if L[i] \in R, then optimized_flags[i] = 0; otherwise, optimized_flags[i]
 * = 1 (i.e., L[i] is optimized in JOIN::optimized()). Correspondingly, we can
 * use L and optimized_flags to retrieve R.
 *
 */
void record_optimized_group_order(PQ_Group_list_ptrs *ptr, ORDER_with_src &new_list,
                                  std::vector<bool> &optimized_flags) {
  optimized_flags.clear();
  // group_list (or order) is optimized to NULL.
  if (new_list.order == nullptr || ptr == nullptr) {
    return;
  }

  optimized_flags.resize(ptr->size(), true);
  int i = 0;
  auto iterator = ptr->begin();
  auto order = new_list.order;

  while (iterator != ptr->end() && order) {
    // find this item, i.e., this item is not optimized
    if ((*iterator)->item[0] && order->item[0] && ((*iterator)->item[0]->eq(order->item[0], false))) {
      optimized_flags[i] = false;
      order = order->next;
    } else {
      optimized_flags[i] = true;
    }
    i++;
    iterator++;
  }
}

/*
 * restore the optimized group/order list, using original and optimized_flags
 */
ORDER *restore_optimized_group_order(SQL_I_List<ORDER> &orig_list, std::vector<bool> &optimized_flags) {
  int size = optimized_flags.size();
  if (0 == size) return nullptr;

  ORDER *header = orig_list.first;
  ORDER **prev_ptr = &header;
  ORDER *iterator;

  int idx = 0;

  for (iterator = orig_list.first; iterator; iterator = iterator->next) {
    if (!optimized_flags[idx]) {
      *prev_ptr = iterator;
      prev_ptr = &iterator->next;
    }
    idx++;
  }
  *prev_ptr = nullptr;

  return header;
}

void restore_list(PQ_Group_list_ptrs *ptr, SQL_I_List<ORDER> &orig_list) {
  orig_list.clear();

  ORDER *order = nullptr;
  ORDER **iterator = ptr->begin();
  for (; iterator != ptr->end(); iterator++) {
    order = *iterator;
    orig_list.link_in_list(order, &order->next);
  }
}

void pq_free_thd(THD *thd) {
  if (!thd) return;
  close_thread_tables(thd);
  thd->mdl_context.release_transactional_locks();
  trans_commit_stmt(thd);
  end_connection(thd);
  close_connection(thd, 0, false, false);
  Diagnostics_area *da = thd->get_stmt_da();
  if (thd->is_error() && thd->pq_leader) {
    thd->pq_leader->raise_condition(da->mysql_errno(), da->returned_sqlstate(), Sql_condition::SL_ERROR,
                                    da->message_text(), false);
  }
  thd->get_stmt_da()->reset_diagnostics_area();
  thd->release_resources();
  thd->free_items();
  destroy(thd);
}

void pq_free_join(JOIN *join) {
  if (!join) return;
  join->join_free();
  join->destroy();
  destroy(join);
}

/**
 * fetch the used key
 * @table: the first non-const table
 * @key: the index of the key in table->key_info
 * @key_parts: #fields in this key
 * @res_fields: the set of all fields in this key
 */
static void get_key_fields(TABLE *table, int key, uint key_parts, std::vector<std::string> &key_fields) {
  assert(table && table->key_info);

  KEY_PART_INFO *kp = table->key_info[key].key_part;
  for (uint i = 0; i < key_parts; i++, kp++) {
    key_fields.emplace_back(kp->field->field_name);
  }
}

/**
 * fetch the key fields used in the tab
 * @tab
 * @res_fields
 *
 * @retval:
 *  false for success, and otherwise true
 */
bool get_table_key_fields(QEP_TAB *tab, std::vector<std::string> &key_fields) {
  key_fields.clear();
  assert(tab);
  auto type = tab->old_type();

  // the original table in leader_join's qep_tab
  TABLE *table = tab->old_table();
  Index_lookup *ref = &tab->old_ref();
  if (!table || !ref) return true;

  // consider the following cases to obtain the sort filed:
  // (1) the case of explicitly using primary key
  if (tab->old_ref().key_parts) {
    get_key_fields(table, ref->key, ref->key_parts, key_fields);
  }
  // (2) the case of index scan
  else if (type == JT_INDEX_SCAN) {
    int key = tab->index();
    uint key_parts = table->key_info[key].user_defined_key_parts;
    get_key_fields(table, key, key_parts, key_fields);
  }

  /*
    // (3) the case of index range.
    else if (type == JT_RANGE || (type == JT_REF && tab->old_quick_optim())) {
      // Note: please confirm whether 'quick' belongs to old qep_tab;
      AccessPath *quick = tab->old_quick_optim();
      if (!quick) return true;

      if (used_index(quick) != MAX_KEY)
        get_key_fields(table, used_index(quick), get_used_key_parts(quick),
                       key_fields);

    }
    */
  // (3) the case of index range.
  else if (type == JT_RANGE) {
    // Note: please confirm whether 'quick' belongs to old qep_tab;
    AccessPath *quick = tab->range_scan();
    if (!quick) return true;

    if (used_index(quick) != MAX_KEY) get_key_fields(table, used_index(quick), get_used_key_parts(quick), key_fields);

  }
  // (4) the case of implicitly using primary key
  else {
    if (table->s->primary_key != MAX_KEY) {
      int key = table->s->primary_key;
      int key_parts = table->key_info[key].user_defined_key_parts;
      get_key_fields(table, key, key_parts, key_fields);
    }
    // (5) other cases
  }
  return false;
}

bool set_key_order(QEP_TAB *tab, std::vector<std::string> &key_fields, ORDER **order_ptr, Ref_item_array *ref_ptrs) {
  JOIN *join = tab->join();
  assert(join && join->order.empty());
  if (!key_fields.size()) {
    *order_ptr = NULL;
    return false;
  }

  std::map<std::string, Item **> fields_map;  // map[field] = item
  std::map<std::string, Item **>::iterator iter;
  std::vector<Item **> order_items;

  Ref_item_array &ref_items = *ref_ptrs;
  /** (1) build the map: {name} -> {item} */
  for (uint i = 0; i < join->fields->size(); i++) {
    Item *item = ref_items[i];
    if (item && item->type() == Item::FIELD_ITEM) {
      std::string field_name = static_cast<Item_field *>(item)->field->field_name;
      fields_map[field_name] = &ref_items[i];
    }
  }

  /** (2) find the item whose name in res_fields */
  for (std::string key : key_fields) {
    iter = fields_map.find(key);
    if (iter != fields_map.end()) {
      order_items.push_back(iter->second);
    }
  }

  /** (3) generate sort order */
  THD *thd = join->thd;
  SQL_I_List<ORDER> order_list;

  for (Item **item : order_items) {
    ORDER *order = new (thd->pq_mem_root) ORDER();
    if (!order) {
      *order_ptr = NULL;
      return true;
    }

    order->item_initial = *item;
    order->item = item;
    order->in_field_list = 1;
    order->is_explicit = 0;
    add_to_list(order_list, order);
  }

  *order_ptr = order_list.first;
  return false;
}

uint get_pq_memory_total() {
  uint sum_memory = 0;
  for (uint i = 0; i < PQ_MEMORY_USED_BUCKET; i++) sum_memory += atomic_add<uint>(pq_memory_used[i], 0);
  return sum_memory;
}

void add_pq_memory(PSI_memory_key key, size_t length, uint id) {
  if (key == key_memory_pq_mem_root) atomic_add<uint>(pq_memory_used[id], length);
}

void sub_pq_memory(PSI_memory_key key, size_t length, uint id) {
  if (key == key_memory_pq_mem_root) atomic_sub<uint>(pq_memory_used[id], length);
}
