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
   Copyright (c) 2023, Shannon Data AI and/or its affiliates. */

#include "sql/sql_pq_condition.h"
// #include <arm_neon.h>
#include "sql/item_strfunc.h"
#include "sql/item_sum.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/mysqld.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_tmp_table.h"

const enum_field_types NO_PQ_SUPPORTED_FIELD_TYPES[] = {
    MYSQL_TYPE_TINY_BLOB, MYSQL_TYPE_MEDIUM_BLOB, MYSQL_TYPE_BLOB,
    MYSQL_TYPE_LONG_BLOB, MYSQL_TYPE_JSON,        MYSQL_TYPE_GEOMETRY};

const Item_sum::Sumfunctype NO_PQ_SUPPORTED_AGG_FUNC_TYPES[] = {
    Item_sum::COUNT_DISTINCT_FUNC,
    Item_sum::SUM_DISTINCT_FUNC,
    Item_sum::AVG_DISTINCT_FUNC,
    Item_sum::GROUP_CONCAT_FUNC,
    Item_sum::JSON_AGG_FUNC,
    Item_sum::UDF_SUM_FUNC,
    Item_sum::STD_FUNC,
    Item_sum::VARIANCE_FUNC,
    Item_sum::SUM_BIT_FUNC};

const Item_func::Functype NO_PQ_SUPPORTED_FUNC_TYPES[] = {
    Item_func::FT_FUNC,      Item_func::MATCH_FUNC,    Item_func::SUSERVAR_FUNC,
    Item_func::FUNC_SP,      Item_func::SUSERVAR_FUNC, Item_func::UDF_FUNC,
    Item_func::NOT_ALL_FUNC
};

const char *NO_PQ_SUPPORTED_FUNC_ARGS[] = {
    "rand",
    "json_valid",
    "json_length",
    "json_type",
    "json_contains_path",
    "json_unquote",
    "st_distance",
    "get_lock",
    "is_free_lock",
    "is_used_lock",
    "release_lock",
    "sleep",
    "xml_str",
    "json_func",
    "weight_string",  // Data truncation (MySQL BUG)
    "des_decrypt",    // Data truncation
    "to_number",
    "trunc",
    "regexp_count",
    "nvl2",
    "initcap",
    "sign",
    "to_date",
    "chr",
    "dump",
    "instrb",
    "lengthb",
    "rpad_oracle",
    "substrb",
    "to_timestamp",
    "translate",
    "vsize",
    "lpad_oracle",
    "rownum",
    "nchr",
};

const char *NO_PQ_SUPPORTED_FUNC_NO_ARGS[] = {"release_all_locks", "sys_guid"};

/**
 * return true when type is a not_supported_field; return false otherwise.
 */
bool pq_not_support_datatype(enum_field_types type) {
  for (const enum_field_types &field_type : NO_PQ_SUPPORTED_FIELD_TYPES) {
    if (type == field_type) {
      return true;
    }
  }

  return false;
}

/**
 * check PQ supported function type
 */
bool pq_not_support_functype(Item_func::Functype type) {
  for (const Item_func::Functype &func_type : NO_PQ_SUPPORTED_FUNC_TYPES) {
    if (type == func_type) {
      return true;
    }
  }

  return false;
}

/**
 * check PQ supported function
 */
bool pq_not_support_func(Item_func *func) {
  if (pq_not_support_functype(func->functype())) {
    return true;
  }

  for (const char *funcname : NO_PQ_SUPPORTED_FUNC_ARGS) {
    if (!strcmp(func->func_name(), funcname) && func->arg_count != 0) {
      return true;
    }
  }

  for (const char *funcname : NO_PQ_SUPPORTED_FUNC_NO_ARGS) {
    if (!strcmp(func->func_name(), funcname)) {
      return true;
    }
  }

  return false;
}

/**
 * check PQ support aggregation function
 */
bool pq_not_support_aggr_functype(Item_sum::Sumfunctype type) {
  for (const Item_sum::Sumfunctype &sum_func_type :
       NO_PQ_SUPPORTED_AGG_FUNC_TYPES) {
    if (type == sum_func_type) {
      return true;
    }
  }

  return false;
}

/**
 * check PQ supported ref function
 */
bool pq_not_support_ref(Item_ref *ref, bool having) {
  Item_ref::Ref_Type type = ref->ref_type();
  if (type == Item_ref::OUTER_REF) {
    return true;
  }
  /**
   * Now, when the sql contains a aggregate function after the 'having',
   * we do not support parallel query. For example:
   * select t1.col1 from t1 group by t1.col1 having avg(t1.col1) > 0;
   * So, we disable the sql;
   */
  if (having && type == Item_ref::AGGREGATE_REF) {
    return true;
  }

  return false;
}

typedef bool (*PQ_CHECK_ITEM_FUN)(Item *item, bool having);

struct PQ_CHECK_ITEM_TYPE {
  Item::Type item_type;
  PQ_CHECK_ITEM_FUN fun_ptr;
};

bool check_pq_support_fieldtype(Item *item, bool having);

bool check_pq_support_fieldtype_std_item(Item *item MY_ATTRIBUTE((unused)),
                                         bool having MY_ATTRIBUTE((unused))) {
  return false;
}

bool check_pq_support_fieldtype_of_rowtype_item(
    Item *item MY_ATTRIBUTE((unused)), bool having MY_ATTRIBUTE((unused))) {
  return false;
}

bool check_pq_support_fieldtype_of_rowtype_table(
    Item *item MY_ATTRIBUTE((unused)), bool having MY_ATTRIBUTE((unused))) {
  return false;
}

bool check_pq_support_fieldtype_of_connect_by(
    Item *item MY_ATTRIBUTE((unused)), bool having MY_ATTRIBUTE((unused))) {
  return false;
}

bool check_pq_support_fieldtype_variance_item(
    Item *item MY_ATTRIBUTE((unused)), bool having MY_ATTRIBUTE((unused))) {
  return false;
}

bool check_pq_support_fieldtype_of_field_item(Item *item,
                                              bool MY_ATTRIBUTE((unused))) {
  Field *field = static_cast<Item_field *>(item)->field;
  assert(field);
  // not supported for generated column
  if (field && (field->is_gcol() || pq_not_support_datatype(field->type()))) {
    return false;
  }

  return true;
}

bool check_pq_support_fieldtype_of_func_item(Item *item, bool having) {
  assert(item && having);
  assert(false);
  return true;
}

bool check_pq_support_fieldtype_of_cond_item(Item *item, bool having) {
  Item_cond *cond = static_cast<Item_cond *>(item);
  assert(cond);

  if (pq_not_support_functype(cond->functype())) {
    return false;
  }

  Item *arg_item = nullptr;
  List_iterator_fast<Item> it(*cond->argument_list());
  for (size_t i = 0; (arg_item = it++); i++) {
    if (arg_item->type() == Item::SUM_FUNC_ITEM ||        // c1
        !check_pq_support_fieldtype(arg_item, having)) {  // c2
      return false;
    }
  }

  return true;
}

bool check_pq_support_fieldtype_of_sum_func_item(Item *item, bool having) {
  /**
   * Now, when the sql contains a reference to the aggregate function after the
   * 'having', we do not support parallel query. For example: select t1.col1,
   * avg(t1.col1) as avg from t1 group by t1.col1 having avg > 0; So, we disable
   * the sql.
   */
  if (having) {
    return false;
  }
  Item_sum *sum = static_cast<Item_sum *>(item);
  if (!sum || pq_not_support_aggr_functype(sum->sum_func())) {
    return false;
  }

  for (uint i = 0; i < sum->argument_count(); i++) {
    if (!check_pq_support_fieldtype(sum->get_arg(i), having)) {
      return false;
    }
  }

  return true;
}

bool check_pq_support_fieldtype_of_ref_item(Item *item, bool having) {
  assert(item && having);
  return true;
}

bool check_pq_support_fieldtype_of_cache_item(Item *item, bool having) {
  Item_cache *item_cache = dynamic_cast<Item_cache *>(item);
  if (item_cache == nullptr) {
    return false;
  }

  Item *example_item = item_cache->get_example();
  if (example_item == nullptr ||
      example_item->type() == Item::SUM_FUNC_ITEM ||        // c1
      !check_pq_support_fieldtype(example_item, having)) {  // c2
    return false;
  }

  return true;
}

bool check_pq_support_fieldtype_of_row_item(Item *item, bool having) {
  // check each item in Item_row
  Item_row *row_item = down_cast<Item_row *>(item);
  for (uint i = 0; i < row_item->cols(); i++) {
    Item *n_item = row_item->element_index(i);
    if (n_item == nullptr || n_item->type() == Item::SUM_FUNC_ITEM ||  // c1
        !check_pq_support_fieldtype(n_item, having)) {                 // c2
      return false;
    }
  }

  return true;
}

PQ_CHECK_ITEM_TYPE g_check_item_type[] = {
    {Item::INVALID_ITEM, nullptr},
    {Item::FIELD_ITEM, check_pq_support_fieldtype_of_field_item},
    {Item::FUNC_ITEM, check_pq_support_fieldtype_of_func_item},
    {Item::SUM_FUNC_ITEM, check_pq_support_fieldtype_of_sum_func_item},
    {Item::STRING_ITEM, nullptr},
    {Item::INT_ITEM, nullptr},
    {Item::REAL_ITEM, nullptr},
    {Item::NULL_ITEM, nullptr},
    {Item::VARBIN_ITEM, nullptr},
    {Item::METADATA_COPY_ITEM, nullptr},
    {Item::FIELD_AVG_ITEM, nullptr},
    {Item::DEFAULT_VALUE_ITEM, nullptr},
    {Item::PROC_ITEM, nullptr},
    {Item::COND_ITEM, check_pq_support_fieldtype_of_cond_item},
    {Item::REF_ITEM, check_pq_support_fieldtype_of_ref_item},
    {Item::FIELD_STD_ITEM, check_pq_support_fieldtype_std_item},
    {Item::FIELD_VARIANCE_ITEM, check_pq_support_fieldtype_variance_item},
    {Item::INSERT_VALUE_ITEM, nullptr},
    {Item::SUBSELECT_ITEM, nullptr},
    {Item::ROW_ITEM, check_pq_support_fieldtype_of_row_item},
    {Item::CACHE_ITEM, check_pq_support_fieldtype_of_cache_item},
    {Item::TYPE_HOLDER, nullptr},
    {Item::PARAM_ITEM, nullptr},
    {Item::TRIGGER_FIELD_ITEM, nullptr},
    {Item::DECIMAL_ITEM, nullptr},
    {Item::XPATH_NODESET, nullptr},
    {Item::XPATH_NODESET_CMP, nullptr},
    {Item::VIEW_FIXER_ITEM, nullptr},
    {Item::FIELD_BIT_ITEM, nullptr},
    {Item::VALUES_COLUMN_ITEM, nullptr}
  };

/**
 * check item is supported by Parallel Query or not
 *
 * @retval:
 *     true : supported
 *     false : not supported
 */
bool check_pq_support_fieldtype(Item *item, bool having) {
  if (item == nullptr || pq_not_support_datatype(item->data_type())) {
    return false;
  }

  if (g_check_item_type[item->type()].fun_ptr != nullptr) {
    return g_check_item_type[item->type()].fun_ptr(item, having);
  }

  return true;
}

/*
 * check if order_list contains aggregate function
 *
 * @retval:
 *    true: contained
 *    false:
 */
bool check_pq_sort_aggregation(const ORDER_with_src &order_list) {
  if (order_list.order == nullptr) {
    return false;
  } else{
    return true;
  }

  ORDER *tmp = nullptr;
  Item *order_item = nullptr;

  for (tmp = order_list.order; tmp; tmp = tmp->next) {
    order_item = *(tmp->item);
    if (!check_pq_support_fieldtype(order_item, false)) {
      return true;
    }
  }

  return false;
}

/*
 * generate item's result_field
 *
 * @retval:
 *    false: generate success
 *    ture: otherwise
 */
bool pq_create_result_fields(THD *thd, Temp_table_param *param,
                             mem_root_deque<Item *> &fields,
                             bool save_sum_fields, ulonglong select_options,
                             MEM_ROOT *root) {
  assert(thd && param && root && fields.size()
         && save_sum_fields && select_options);
  return false;
}

/**
 * check whether the select result fields is suitable for parallel query
 *
 * @return:
 *    true, suitable
 *    false.
 */
bool check_pq_select_result_fields(JOIN *join) {
  assert(join);
  return true;
}

/**
 * choose a table that do parallel query, currently only do parallel scan on
 * first no-const primary table.
 * Disallow splitting inner tables, such as select * from t1 left join t2 on 1
 * where t1.a = 't1'. We can't split t2 when t1 is const table.
 * Disallow splitting semijion inner tables,such as select * from t1 where
 * exists (select * from t2). We can't split t2.
 *
 * @return:
 *    true, found a parallel scan table
 *    false, cann't found a parallel scan table
 */
bool choose_parallel_scan_table(JOIN *join) {
  assert(join);
  return true;
}

void set_pq_dop(THD *thd) {
  assert(thd);
}

/**
 *  check whether  the parallel query is enabled and set the
 *  parallel query condition status
 *
 */
void set_pq_condition_status(THD *thd) {
  set_pq_dop(thd);

  if (thd->pq_dop > 0) {
    thd->m_suite_for_pq = PQ_ConditionStatus::ENABLED;
  } else {
    thd->m_suite_for_pq = PQ_ConditionStatus::NOT_SUPPORTED;
  }
}

bool suite_for_parallel_query(THD *thd) {
  assert(thd);
  return true;
}

bool suite_for_parallel_query(LEX *lex) {
  assert(lex);
  return true;
}

bool suite_for_parallel_query(Query_expression *unit) {
  if (!unit->is_simple()) {
    return false;
  }

  return true;
}

bool suite_for_parallel_query(Table_ref *tbl_list) {
  if (tbl_list->is_view() ||                         // view
      tbl_list->lock_descriptor().type > TL_READ ||  // explicit table lock
      tbl_list->is_fulltext_searched() ||            // fulltext match search
      current_thd->locking_clause) {
    return false;
  }

  TABLE *tb = tbl_list->table;
  if (tb != nullptr &&
      (tb->s->tmp_table != NO_TMP_TABLE ||         // template table
       tb->file->ht->db_type != DB_TYPE_INNODB ||  // Non-InnoDB table
       tb->part_info)) {                           // partition table
    return false;
  }

  return true;
}

bool suite_for_parallel_query(Query_block *select) {
  assert(select);
  return true;
}

bool suite_for_parallel_query(JOIN *join) {
  assert(join);
  return true;
}

bool check_pq_running_threads(uint dop, ulong timeout_ms) {
  assert(dop && timeout_ms);
  return true;
}

class PQCheck {
 public:
  explicit PQCheck(Query_block *select_lex_arg) : select_lex(select_lex_arg) {}

  virtual ~PQCheck() {}

  virtual bool suite_for_parallel_query();

 protected:
  virtual void set_select_id();
  virtual void set_select_type();

 protected:
  uint select_id{};
  enum_explain_type select_type{};

 private:
  Query_block *select_lex;
};

class PlanReadyPQCheck : public PQCheck {
 public:
  explicit PlanReadyPQCheck(Query_block *select_lex_arg)
      : PQCheck(select_lex_arg), join(select_lex_arg->join) {}

  ~PlanReadyPQCheck() override {}

  bool suite_for_parallel_query() override;

 private:
  void set_select_id() override;
  void set_select_type() override;

 private:
  JOIN *join;
  QEP_TAB *tab{nullptr};
};

void PQCheck::set_select_id() { select_id = select_lex->select_number; }

void PQCheck::set_select_type() { select_type = select_lex->type(); }

bool PQCheck::suite_for_parallel_query() {
  set_select_id();
  set_select_type();

  if (select_id > 1 || select_type != enum_explain_type::EXPLAIN_SIMPLE) {
    return false;
  }

  return true;
}

void PlanReadyPQCheck::set_select_id() {
  if (tab && sj_is_materialize_strategy(tab->get_sj_strategy())) {
    select_id = tab->sjm_query_block_id();
  } else {
    PQCheck::set_select_id();
  }
}

void PlanReadyPQCheck::set_select_type() {
  if (tab && sj_is_materialize_strategy(tab->get_sj_strategy())) {
    select_type = enum_explain_type::EXPLAIN_MATERIALIZED;
  } else {
    PQCheck::set_select_type();
  }
}

bool PlanReadyPQCheck::suite_for_parallel_query() {
  for (uint t = 0; t < join->tables; t++) {
    tab = join->qep_tab + t;
    if (!tab->position()) {
      continue;
    }

    if (!PQCheck::suite_for_parallel_query()) {
      return false;
    }
  }

  return true;
}

bool check_select_id_and_type(Query_block *select_lex) {
  JOIN *join = select_lex->join;
  std::unique_ptr<PQCheck> check;
  bool ret = false;

  if (join == nullptr) {
    check.reset(new PQCheck(select_lex));
    goto END;
  }

  switch (join->get_plan_state()) {
    case JOIN::NO_PLAN:
    case JOIN::ZERO_RESULT:
    case JOIN::NO_TABLES: {
      check.reset(new PQCheck(select_lex));
      break;
    }

    case JOIN::PLAN_READY: {
      check.reset(new PlanReadyPQCheck(select_lex));
      break;
    }

    default:
      assert(0);
  }

END:
  if (check != nullptr) {
    ret = check->suite_for_parallel_query();
  }

  return ret;
}

bool check_select_group_and_order_by(Query_block *select_lex) {
  for (ORDER *group = select_lex->group_list.first; group;
       group = group->next) {
    Item *item = *group->item;
    if (item && !check_pq_support_fieldtype(item, false)) {
      return false;
    }
  }

  for (ORDER *order = select_lex->order_list.first; order;
       order = order->next) {
    Item *item = *order->item;
    if (item && !check_pq_support_fieldtype(item, false)) {
      return false;
    }
  }

  return true;
}

bool check_pq_conditions(THD *thd) {
  assert(thd);
  return true;
}