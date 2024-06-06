#ifndef SQL_PARALLEL_H
#define SQL_PARALLEL_H

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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.   */

#include "my_alloc.h"
#include "sql/iterators/basic_row_iterators.h"
#include "sql/sql_base.h"
#include "sql/sql_lex.h"
#include "sql/sql_message_queue.h"

class QEP_TAB;
class Query_block;
class THD;
class Gather_operator;
class JOIN;
class ORDER_with_src;
class Exchange;
class Filesort;

enum PQ_exec_status { SEQ_EXEC = 0, PARL_EXEC, ABORT_EXEC };

enum PQ_worker_state { INIT = 1, READY = 2, COMPELET = 4, PQERROR = 8, OVER = 16 };

/** optimized variables */
struct PQ_optimized_var {
  bool pq_grouped;
  bool pq_implicit_grouping;
  bool pq_simple_group;
  bool pq_simple_order;
  bool pq_streaming_aggregation;
  bool pq_group_optimized_away;
  bool pq_need_tmp_before_win;
  bool pq_skip_sort_order;
  uint pq_m_ordered_index_usage;

  /*
   * optimized_*_flags record each element in select_lex->group_list (or
   * order_list) is optimized or not. Specifically, optimized_group_flags[i] = 1
   * means that group_list[i] is optimized in JOIN::optimize(); otherwise,
   * pq_group_optimized[i] = 0 and group_list[i] is remained in
   * JOIN::group_list.
   */
  std::vector<bool> optimized_group_flags;
  std::vector<bool> optimized_order_flags;
  uint pq_no_jbuf_after;  // for read_set and index push down
};

class MQ_record_gather {
 public:
  THD *m_thd;
  QEP_TAB *m_tab;
  Exchange *m_exchange;

 public:
  MQ_record_gather() : m_thd(nullptr), m_tab(nullptr), m_exchange(nullptr) {}

  MQ_record_gather(THD *thd, QEP_TAB *tab) : m_thd(thd), m_tab(tab), m_exchange(nullptr) {}
  ~MQ_record_gather() {}
  bool mq_scan_init(Filesort *sort, int workers, uint ref_length, bool stab_output = false);

  bool mq_scan_next();

  void mq_scan_end();
};

/**
 * Parallel scan worker's manager struct
 */
class PQ_worker_manager {
 public:
  uint m_id;  // worker number
  Gather_operator *m_gather;
  THD *thd_leader;             // pointer to leader thread
  THD *thd_worker;             // pointer to worker thread
  MQueue_handle *m_handle;     // worker's message queue handle
  PQ_worker_state m_status;    // worker status
  my_thread_handle thread_id;  // Thread id
  bool m_active{false};        // true if this worker is created, and false otherwise.

 private:
  mysql_mutex_t m_mutex;  // mutex protect previous members
  mysql_cond_t m_cond;

 public:
  PQ_worker_manager() = delete;

  PQ_worker_manager(uint id);

  ~PQ_worker_manager();

  bool wait_for_status(THD *thd, uint state);

  void signal_status(THD *thd, PQ_worker_state state);

  void kill();
};

struct CODE_STATE;
/**
 * Gather operator for parallel scan
 */
class Gather_operator {
 public:
  uint m_dop;                     // Degree of parallel execution;
                                  // (if m_pq_ctx->m_ctxs.size() is less than exepected m_dop,
                                  // m_dop will change to m_pq_ctx->m_ctxs.size() in init()
                                  // function)
  JOIN *m_template_join;          // physical query plan template
  PQ_worker_manager **m_workers;  // parallel workers manager info
  TABLE *m_table;                 // the table need parallel query
  void *m_pq_ctx;                 // innodb Parallel query context
  QEP_TAB *m_tab;                 // the rewrite qep_tab
  uint keyno;                     // the index for paralleling read
  int m_ha_err;
  Diagnostics_area m_stmt_da;
  mysql_mutex_t lock_stmt_da;
  CODE_STATE **m_code_state;
  bool table_scan;

 private:
  std::mutex m_read_end_mutex;

 public:
  Gather_operator() = delete;

  Gather_operator(uint dop);

  ~Gather_operator();

  bool init();

  void end();

  void signalAll();

  void signalReadEnd();

  void waitReadEnd();
};

Gather_operator *make_pq_gather_operator(JOIN *join, QEP_TAB *tab, uint dop);
PQ_exec_status make_pq_leader_plan(THD *thd);
void *pq_worker_exec(void *arg);
bool pq_build_sum_funcs(THD *thd, Query_block *select, Ref_item_array &ref_ptr, mem_root_deque<Item *> &fields,
                        uint elements, nesting_map select_nest_level);
void pq_replace_avg_func(THD *thd, Query_block *select, mem_root_deque<Item *> *fields, nesting_map select_nest_level);
extern bool get_table_key_fields(QEP_TAB *tab, std::vector<std::string> &res_fields);
extern bool setup_order(THD *thd, Ref_item_array ref_item_array, Table_ref *tables, List<Item> &fields,
                        List<Item> &all_fields, ORDER *order);
extern void release_pq_running_threads(uint dop);
extern void add_pq_memory(PSI_memory_key key, size_t length, uint id);
extern void sub_pq_memory(PSI_memory_key key, size_t length, uint id);
extern uint get_pq_memory_total();
extern void add_to_list(SQL_I_List<ORDER> &list, ORDER *order);

extern ulonglong parallel_memory_limit;
extern ulong parallel_max_threads;
extern uint pq_memory_used[PQ_MEMORY_USED_BUCKET];
extern uint pq_memory_total_used;
extern uint parallel_threads_running;

extern mysql_mutex_t LOCK_pq_threads_running;
extern mysql_cond_t COND_pq_threads_running;

bool pq_make_join_readinfo(JOIN *join, PQ_worker_manager *mngr, uint no_jbuf_after);

bool pq_check_stable_sort(JOIN *join, int idx);

bool set_key_order(QEP_TAB *tab, std::vector<std::string> &res_fields, ORDER **order_ptr, Ref_item_array *ref_ptrs);

void restore_list(PQ_Group_list_ptrs *ptr, SQL_I_List<ORDER> &orig_list);

void pq_free_thd(THD *thd);

void pq_free_join(JOIN *join);

void pq_free_gather(Gather_operator *gather);

#endif /* SQL_PARALLEL_H */
