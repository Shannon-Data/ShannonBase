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
#include "sql/sql_parallel.h"
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
    Item_func::NOT_ALL_FUNC };

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
  Item_func *func = static_cast<Item_func *>(item);
  assert(func);

  // check func type
  if (pq_not_support_func(func)) {
    return false;
  }

  // the case of Item_func_make_set
  if (!strcmp(func->func_name(), "make_set")) {
    Item *arg_item = down_cast<Item_func_make_set *>(func)->get_item();
    if (arg_item && !check_pq_support_fieldtype(arg_item, having)) {
      return false;
    }
  }

  // check func args type
  for (uint i = 0; i < func->arg_count; i++) {
    // c: args contain unsupported fields
    Item *arg_item = func->arguments()[i];
    if (arg_item == nullptr ||
        !check_pq_support_fieldtype(arg_item, having)) {  // c
      return false;
    }
  }

  // the case of Item_equal
  if (func->functype() == Item_func::MULT_EQUAL_FUNC) {
    Item_equal *item_equal = down_cast<Item_equal *>(item);
    assert(item_equal);

    // check const_item
    Item *const_item = item_equal->const_arg();
    if (const_item &&
        (const_item->type() == Item::SUM_FUNC_ITEM ||         // c1
         !check_pq_support_fieldtype(const_item, having))) {  // c2
      return false;
    }

    // check fields
    /*
    Item *field_item = nullptr;
    List<Item_field> fields = item_equal->get_fields();
    List_iterator_fast<Item_field> it(fields);
    for (size_t i = 0; (field_item = it++); i++) {
      if (!check_pq_support_fieldtype(field_item, having)) {
        return false;
      }
    }
    */

    for (Item_field &field : item_equal->get_fields()) {
      if (!check_pq_support_fieldtype(&field, having)) {
        return false;
      }
    }
  }

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
  Item_ref *item_ref = down_cast<Item_ref *>(item);
  if (item_ref == nullptr || pq_not_support_ref(item_ref, having)) {
    return false;
  }

  if (!check_pq_support_fieldtype(item_ref->m_ref_item[0], having)) {
    return false;
  }

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
    {Item::VALUES_COLUMN_ITEM, nullptr}};

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
  const bool not_all_columns = !(select_options & TMP_TABLE_ALL_COLUMNS);
  long hidden_field_count = param->hidden_field_count;
  Field *from_field = nullptr;
  Field **tmp_from_field = &from_field;
  Field **default_field = &from_field;

  bool force_copy_fields = false;
  TABLE_SHARE s;
  TABLE table;
  table.s = &s;

  uint copy_func_count = param->func_count;
  if (param->precomputed_group_by) {
    copy_func_count += param->sum_func_count;
  }

  Func_ptr_array *copy_func = new (root) Func_ptr_array(root);
  if (copy_func == nullptr) {
    return true;
  }

  copy_func->reserve(copy_func_count);
  for (Item *item : fields) {
    Item::Type type = item->type();
    const bool is_sum_func =
        type == Item::SUM_FUNC_ITEM && !item->m_is_window_function;

    if (not_all_columns && item != nullptr) {
      if (item->has_aggregation() && type != Item::SUM_FUNC_ITEM) {
        if (item->is_outer_reference()) item->update_used_tables();
        if (type == Item::SUBSELECT_ITEM ||
            (item->used_tables() & ~OUTER_REF_TABLE_BIT)) {
          param->using_outer_summary_function = 1;
          goto update_hidden;
        }
      }

      if (item->m_is_window_function) {
        if (!param->m_window || param->m_window_frame_buffer) {
          goto update_hidden;
        }

        if (param->m_window != down_cast<Item_sum *>(item)->window()) {
          goto update_hidden;
        }
      } else if (item->has_wf()) {
        if (param->m_window == nullptr || !param->m_window->is_last()) {
          goto update_hidden;
        }
      }

      if (item->const_item()) continue;
    }

    if (is_sum_func && !save_sum_fields) {
      /* Can't calc group yet */
    } else {
      Field *new_field = nullptr;
      if (param->schema_table) {
        new_field =
            item ? create_tmp_field_for_schema(item, &table, root) : nullptr;
      } else {
        new_field =
            item ? create_tmp_field(thd, &table, item, type, copy_func,
                                    tmp_from_field, default_field, false,  //(1)
                                    !force_copy_fields && not_all_columns,
                                    item->marker == Item::MARKER_BIT ||
                                        param->bit_fields_as_long,  //(2)
                                    force_copy_fields)
                 : nullptr;
      }

      if (new_field == nullptr) {
        assert(thd->is_fatal_error());
        return true;
      }

      if (not_all_columns && type == Item::SUM_FUNC_ITEM) {
        ((Item_sum *)item)->set_result_field(new_field);
      }

      s.fields++;
    }

  update_hidden:
    if (!--hidden_field_count) {
      param->hidden_field_count = 0;
    }
  }  // end of while ((item=li++)).

  if (s.fields == 0) return true;

  Field *result_field = nullptr;

  for (Item *item : fields) {
    // c1: const_item will not produce field in the first rewritten table
    if (item->const_item() || item->basic_const_item()) {
      continue;
    }

    if (item->has_aggregation() && item->type() != Item::SUM_FUNC_ITEM) {
      if (item->type() == Item::SUBSELECT_ITEM ||
          (item->used_tables() & ~OUTER_REF_TABLE_BIT)) {
        continue;
      }
    }

    result_field = item->get_result_field();
    if (result_field) {
      enum_field_types field_type = result_field->type();
      // c3: result_field contains unsupported data type
      if (pq_not_support_datatype(field_type)) {
        return true;
      }
    } else {
      // c4: item is not FIELD_ITEM and it has no result_field
      if (item->type() != Item::FIELD_ITEM) {
        return true;
      }

      result_field = down_cast<Item_field *>(item)->result_field;
      if (result_field && pq_not_support_datatype(result_field->type())) {
        return true;
      }
    }
  }

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
  DBUG_ENTER("check result fields is suitable for parallel query or not");
  MEM_ROOT *pq_check_root = ::new MEM_ROOT();
  if (pq_check_root == nullptr) {
    DBUG_RETURN(false);
  }

  init_sql_alloc(key_memory_thd_main_mem_root, pq_check_root,
                 global_system_variables.query_alloc_block_size);

  bool suit_for_parallel = false;

  mem_root_deque<Item *> *tmp_all_fields = join->fields;

  join->tmp_table_param.pq_copy(join->saved_tmp_table_param);
  join->tmp_table_param.copy_fields.clear();

  Temp_table_param *tmp_param = new (pq_check_root)
      Temp_table_param(pq_check_root, join->tmp_table_param);

  if (tmp_param == nullptr) {
    // free the memory
    pq_check_root->Clear();
    if (pq_check_root) {
      ::delete pq_check_root;
    }
    DBUG_RETURN(suit_for_parallel);
  }

  tmp_param->m_window_frame_buffer = false;
  mem_root_deque<Item *> tmplist(*tmp_all_fields);
  tmp_param->hidden_field_count = CountHiddenFields(*tmp_all_fields);

  // create_tmp_table may change the original item's result_field, hence
  // we must save it before.
  std::vector<Field *> saved_result_field(tmplist.size(), nullptr);

  int i = 0;
  for (Item *tmp_item : *tmp_all_fields) {
    if (tmp_item->type() == Item::FIELD_ITEM ||
        tmp_item->type() == Item::DEFAULT_VALUE_ITEM) {
      saved_result_field[i] = down_cast<Item_field *>(tmp_item)->result_field;
    } else {
      saved_result_field[i] = tmp_item->get_result_field();
    }
    i++;
  }

  if (pq_create_result_fields(join->thd, tmp_param, tmplist, true,
                              join->query_block->active_options(),
                              pq_check_root)) {
    suit_for_parallel = false;
  } else {
    suit_for_parallel = true;
  }

  // restore result_field
  i = 0;
  for (Item *tmp_item : *tmp_all_fields) {
    if (tmp_item->type() == Item::FIELD_ITEM ||
        tmp_item->type() == Item::DEFAULT_VALUE_ITEM) {
      down_cast<Item_field *>(tmp_item)->result_field = saved_result_field[i];
    } else {
      tmp_item->set_result_field(saved_result_field[i]);
    }
    i++;
  }

  // free the memory
  pq_check_root->Clear();
  if (pq_check_root) {
    ::delete pq_check_root;
  }
  DBUG_RETURN(suit_for_parallel);
}

/**
 * check whether the select fields is suitable for parallel query
 *
 * @return:
 *    true, suitable
 *    false.
 */
bool check_pq_select_fields(JOIN *join) {
  for (Item *item : join->query_block->fields) {
    if (!check_pq_support_fieldtype(item, false)) {
      return false;
    }
  }

  // check whether contains blob, text, json and geometry field
  for (Item *item : *join->fields) {
    if (!check_pq_support_fieldtype(item, false)) {
      return false;
    }
  }

  Item *n_where_cond = join->query_block->where_cond();
  Item *n_having_cond = join->query_block->having_cond();

  if (n_where_cond && !check_pq_support_fieldtype(n_where_cond, false)) {
    return false;
  }

  /*
   * For Having Aggr. function, the having_item will be pushed
   * into all_fields in prepare phase. Currently, we have not support this
   * operation.
   */
  if (n_having_cond && !check_pq_support_fieldtype(n_having_cond, true)) {
    return false;
  }

  if (check_pq_sort_aggregation(join->order)) {
    return false;
  }

  if (!check_pq_select_result_fields(join)) {
    return false;
  }

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
  QEP_TAB *tab = &join->qep_tab[join->const_tables];
  if (tab->is_inner_table_of_outer_join() ||
      tab->get_qs()->first_sj_inner() >= 0) {
    return false;
  }

  tab->do_parallel_scan = true;
  return true;
}

void set_pq_dop(THD *thd) {
  if (!thd->no_pq && thd->variables.force_parallel_execute &&
      thd->pq_dop == 0) {
    thd->pq_dop = thd->variables.parallel_default_dop;
    if (thd->pq_dop > std::thread::hardware_concurrency())
      thd->pq_dop = std::thread::hardware_concurrency();
  }
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
  if (thd->in_sp_trigger != 0 ||  // store procedure or trigger
      thd->get_attachable_trx() ||    // attachable transaction
                                  // serializable without snapshot read
      thd->tx_isolation == ISO_SERIALIZABLE ) {
    return false;
  }

  return true;
}

bool suite_for_parallel_query(LEX *lex) {
  if (lex->in_execute_ps || lex->has_sp || lex->has_notsupported_func) {
    return false;
  }

  return true;
}

bool suite_for_parallel_query(Query_expression *unit) {
  return unit->is_simple() ? true : false;
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
  if (select->first_inner_query_expression() !=
          nullptr ||  // nesting subquery, including view〝derived
                      // table〝subquery condition and so on.
      select->outer_query_block() != nullptr ||  // nested subquery
      select->is_distinct() ||                   // select distinct
      select->has_foj() ||                       // full out join
      select->saved_windows_elements) {  // windows function
    return false;
  }
  if (select->has_limit()) {
    return false;
  }

  for (Table_ref *tbl_list = select->get_table_list(); tbl_list != nullptr;
       tbl_list = tbl_list->next_local) {
    if (!suite_for_parallel_query(tbl_list)) {
      return false;
    }
  }

  for (Table_ref *tbl_list = select->get_table_list(); tbl_list != nullptr;
       tbl_list = tbl_list->next_global) {
    if (!suite_for_parallel_query(tbl_list)) {
      return false;
    }
  }

  for (Table_ref *tbl_list = select->leaf_tables; tbl_list != nullptr;
       tbl_list = tbl_list->next_leaf) {
    if (!suite_for_parallel_query(tbl_list)) {
      return false;
    }
  }
  return true;
}

bool suite_for_parallel_query(JOIN *join) {
  if ((join->best_read < join->thd->variables.parallel_cost_threshold) ||
      (join->primary_tables == join->const_tables) ||
      (join->select_distinct || join->select_count) ||
      (join->fields->size() > MAX_FIELDS) ||
      (join->rollup_state != JOIN::RollupState::NONE) ||
      (join->zero_result_cause != nullptr)) {
    return false;
  }
  QEP_TAB *tab = &join->qep_tab[join->const_tables];
  // only support table/index full/range scan
  join_type scan_type = tab->type();
  if (scan_type != JT_ALL && scan_type != JT_INDEX_SCAN &&
      scan_type != JT_REF &&
      (scan_type != JT_RANGE || !tab->range_scan() ||
       tab->range_scan()->quick_select_type() != PQ_RANGE_TYPE::PQ_INDEX_RANGE_SCAN)) {
    return false;
  }
  if (tab->range_scan() &&
      tab->range_scan()->quick_select_type() != PQ_RANGE_TYPE::PQ_INDEX_RANGE_SCAN) {
    return false;
  }

  if (!check_pq_select_fields(join)) {
    return false;
  }

  return true;
}

bool check_pq_running_threads(uint dop, ulong timeout_ms) {
  bool success = false;
  mysql_mutex_lock(&LOCK_pq_threads_running);
  if (dop > parallel_max_threads) {
    success = false;
  } else if (parallel_threads_running + dop > parallel_max_threads) {
    if (timeout_ms > 0) {
      struct timespec start_ts;
      struct timespec end_ts;
      struct timespec abstime;
      ulong wait_timeout = timeout_ms;
      int wait_result;

    start:
      set_timespec(&start_ts, 0);
      /* Calcuate the waiting period. */
      abstime.tv_sec = start_ts.tv_sec + wait_timeout / TIME_THOUSAND;
      abstime.tv_nsec =
          start_ts.tv_nsec + (wait_timeout % TIME_THOUSAND) * TIME_MILLION;
      if (abstime.tv_nsec >= TIME_BILLION) {
        abstime.tv_sec++;
        abstime.tv_nsec -= TIME_BILLION;
      }
      wait_result = mysql_cond_timedwait(&COND_pq_threads_running,
                                         &LOCK_pq_threads_running, &abstime);
      if (parallel_threads_running + dop <= parallel_max_threads) {
        success = true;
      } else {
        success = false;
        if (!wait_result) {  // wait isn't timeout
          set_timespec(&end_ts, 0);
          ulong difftime = (end_ts.tv_sec - start_ts.tv_sec) * TIME_THOUSAND +
                           (end_ts.tv_nsec - start_ts.tv_nsec) / TIME_MILLION;
          wait_timeout -= difftime;
          goto start;
        }
      }
    }
  } else {
    success = true;
  }

  if (success) {
    // uint32x2_t v_a = {parallel_threads_running,
    //                   current_thd->pq_threads_running};
    // uint32x2_t v_b = {dop, dop};
    // v_a = vadd_u32(v_a, v_b);
    // parallel_threads_running = vget_lane_u32(v_a, 0);
    // current_thd->pq_threads_running = vget_lane_u32(v_a, 1);

    parallel_threads_running += dop;
    current_thd->pq_threads_running += dop;
  }

  mysql_mutex_unlock(&LOCK_pq_threads_running);
  return success;
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
  // max PQ memory size limit
  if (get_pq_memory_total() >= parallel_memory_limit) {
    atomic_add<uint>(parallel_memory_refused, 1);
    return false;
  }

  // max PQ threads limit
  if (!check_pq_running_threads(thd->pq_dop,
                                thd->variables.parallel_queue_timeout)) {
    atomic_add<uint>(parallel_threads_refused, 1);
    return false;
  }

  Query_block *select = thd->lex->unit->first_query_block();
  if (!check_select_group_and_order_by(select)) {
    return false;
  }

  // RBO limit
  if (!suite_for_parallel_query(thd)) {
    return false;
  }

  if (!suite_for_parallel_query(thd->lex)) {
    return false;
  }

  if (!suite_for_parallel_query(thd->lex->unit)) {
    return false;
  }

  if (!suite_for_parallel_query(select)) {
    return false;
  }

  if (!suite_for_parallel_query(select->join)) {
    return false;
  }

  if (!check_select_id_and_type(select)) {
    return false;
  }

  if (!choose_parallel_scan_table(select->join)) {
    return false;
  }

  return true;
}
