#ifndef __SHANNONBASE_SQL_PQ_CLONE_ITEM_H__
#define __SHANNONBASE_SQL_PQ_CLONE_ITEM_H__

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "item_geofunc.h"
#include "item_inetfunc.h"
#include "item_pfs_func.h"
#include "mem_root_deque.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_regexp_func.h"
#include "sql/item_sum.h"
#include "sql/item_timefunc.h"
#include "sql/log.h"
#include "sql/parse_tree_helpers.h"
#include "sql/parse_tree_items.h"
#include "sql/parse_tree_nodes.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_pq_clone.h"
#define CHECK_TYPE(T)                                                                               \
  if (typeid(*this) != typeid(T) || DBUG_EVALUATE_IF("simulate_item_type_mismatch", true, false)) { \
    sql_print_warning(                                                                              \
        "Caller's type %s is not equals to this class type %s, "                                    \
        "will not use parallel query, SQL= %s",                                                     \
        typeid(*this).name(), typeid(T).name(), thd->query().str);                                  \
    assert(DBUG_EVALUATE_IF("simulate_item_type_mismatch", true, false) || false);                  \
    return nullptr;                                                                                 \
  }

#define COPY_FROM_SUPER(D, B)                                    \
  if (B::pq_copy_from(thd, select, item)) {                      \
    return true;                                                 \
  }                                                              \
  D *orig_item MY_ATTRIBUTE((unused)) = dynamic_cast<D *>(item); \
  assert(orig_item);

#define COPY_SELF_ATTR(OBJ)                           \
  if (!OBJ || OBJ->pq_copy_from(thd, select, this)) { \
    return nullptr;                                   \
  }

#define PQ_CLONE_DEF(T)                              \
  Item *T::pq_clone(THD *thd, Query_block *select) { \
    CHECK_TYPE(T)                                    \
    T *new_item = nullptr;

#define PQ_CLONE_RETURN    \
  COPY_SELF_ATTR(new_item) \
  return new_item;         \
  }

#define PQ_CLONE_ARGS                                 \
  mem_root_deque<Item *> item_list(thd->pq_mem_root); \
  for (uint i = 0; i < arg_count; i++) {              \
    Item *arg = args[i]->pq_clone(thd, select);       \
    if (arg == nullptr) return nullptr;               \
    item_list.push_back(arg);                         \
  }

#define PQ_COPY_FROM_DEF(D, B)                                      \
  bool D::pq_copy_from(THD *thd, Query_block *select, Item *item) { \
    COPY_FROM_SUPER(D, B)

#define PQ_COPY_FROM_RETURN \
  return false;             \
  }

#define PQ_REBUILD_SUM_DEF(T)                                                   \
  Item_sum *T::pq_rebuild_sum_func(THD *thd, Query_block *select, Item *item) { \
    CHECK_TYPE(T)                                                               \
    T *new_item = nullptr;

#define PQ_REBUILD_SUM_RETURN \
  COPY_SELF_ATTR(new_item)    \
  return new_item;            \
  }

#define ARG0 copy_args[0]
#define ARG1 copy_args[1]
#define ARG2 copy_args[2]
#define ARG3 copy_args[3]
#define ARG4 copy_args[4]
#define COPY_FUNC_ITEM(T, ...)                        \
  Item *T::pq_clone(THD *thd, Query_block *select) {  \
    CHECK_TYPE(T);                                    \
    Item *copy_args[5];                               \
    assert(arg_count < 5);                            \
    for (uint i = 0; i < arg_count; i++) {            \
      copy_args[i] = args[i]->pq_clone(thd, select);  \
      if (copy_args[i] == nullptr) {                  \
        return nullptr;                               \
      }                                               \
    }                                                 \
    Item *new_item = nullptr;                         \
    new_item = new (thd->pq_mem_root) T(__VA_ARGS__); \
    COPY_SELF_ATTR(new_item)                          \
    return new_item;                                  \
  }

Item *Item::pq_clone(THD *thd MY_ATTRIBUTE((unused)), Query_block *select MY_ATTRIBUTE((unused))) { return nullptr; }

bool Item::pq_copy_from(THD *thd MY_ATTRIBUTE((unused)), Query_block *select MY_ATTRIBUTE((unused)), Item *item) {
  cmp_context = item->cmp_context;
  marker = item->marker;

  collation = item->collation;
  item_name.copy(item->item_name.ptr(), item->item_name.length(), system_charset_info,
                 item->item_name.is_autogenerated());
  orig_name.copy(item->orig_name.ptr(), item->orig_name.length(), system_charset_info,
                 item->orig_name.is_autogenerated());
  decimals = item->decimals;
  is_expensive_cache = item->is_expensive_cache;
  m_accum_properties = item->m_accum_properties;
  m_data_type = item->m_data_type;
  m_is_window_function = item->m_is_window_function;
  max_length = item->max_length;
  m_nullable = item->is_nullable();
  null_value = item->null_value;
  str_value = item->str_value;
  hidden = item->hidden;

#ifndef NDEBUG
  contextualized = item->contextualized;
#endif
  unsigned_flag = item->unsigned_flag;

  if (!pq_alloc_item && item->pq_alloc_item) thd->add_item(this);

  return false;
}

/* Item_basic_constant start */
PQ_COPY_FROM_DEF(Item_basic_constant, Item) {
  if (orig_item != nullptr) {
    used_table_map = orig_item->used_table_map;
  }
}
PQ_COPY_FROM_RETURN

/* Item_cache start */
PQ_COPY_FROM_DEF(Item_cache, Item_basic_constant) {
  if (orig_item != nullptr) {
    used_table_map = orig_item->used_table_map;
    cached_field = orig_item->cached_field;
  }
  if (orig_item != nullptr && orig_item->example != nullptr) {
    Item *example_arg = orig_item->example->pq_clone(thd, select);
    if (example_arg == nullptr) return true;
    if (!example_arg->fixed) {
      example_arg->fix_fields(thd, &example_arg);
    }
    setup(example_arg);
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_cache_datetime) { new_item = new (thd->pq_mem_root) Item_cache_datetime(data_type()); }
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_cache_decimal) { new_item = new (thd->pq_mem_root) Item_cache_decimal(); }
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_cache_int) {
  new_item = new (thd->pq_mem_root) Item_cache_int();
  if (new_item == nullptr) {
    return nullptr;
  }
  if (origin_item) {
    new_item->example = origin_item->pq_clone(thd, select);
    if (new_item->example == nullptr) {
      return nullptr;
    }
  }
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_cache_real) { new_item = new (thd->pq_mem_root) Item_cache_real(); }
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_cache_row) { new_item = new (thd->pq_mem_root) Item_cache_row(); }
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_cache_str) {
  const Item *item = static_cast<const Item *>(this);
  new_item = new (thd->pq_mem_root) Item_cache_str(item);
}
PQ_CLONE_RETURN
/* Item_cache end */

/* Item_hex_string start */
PQ_CLONE_DEF(Item_hex_string) { new_item = new (thd->pq_mem_root) Item_hex_string(POS()); }
PQ_CLONE_RETURN

// TOOD str_value copyed twice
PQ_CLONE_DEF(Item_bin_string) {
  new_item = new (thd->pq_mem_root) Item_bin_string(str_value.ptr(), str_value.length());
}
PQ_CLONE_RETURN
/* Item_hex_string end */

/* Item_null start */
PQ_CLONE_DEF(Item_null) { new_item = new (thd->pq_mem_root) Item_null(POS()); }
PQ_CLONE_RETURN
/* Item_null end */

/* Item_num start */
PQ_CLONE_DEF(Item_int_with_ref) {
  Item *pq_ref = ref->pq_clone(thd, select);
  if (pq_ref == nullptr) return nullptr;
  new_item = new (thd->pq_mem_root) Item_int_with_ref(pq_ref->data_type(), value, pq_ref, unsigned_flag);
}
PQ_CLONE_RETURN
/* Item_num end */

/* Item_string start */
PQ_CLONE_DEF(Item_string) {
  if (origin_item) return origin_item->pq_clone(thd, select);

  new_item =
      new (thd->pq_mem_root) Item_string(static_cast<Name_string>(item_name), str_value.ptr(), str_value.length(),
                                         collation.collation, collation.derivation, collation.repertoire);
  if (new_item) {
    new_item->set_cs_specified(m_cs_specified);
  }
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_static_string_func) {
  if (origin_item) return origin_item->pq_clone(thd, select);

  new_item = new (thd->pq_mem_root) Item_static_string_func(func_name, str_value.ptr(), str_value.length(),
                                                            collation.collation, collation.derivation);
}
PQ_CLONE_RETURN
/* Item_string end */
/* Item_basic_constant end */

/* Item_ident start */
PQ_COPY_FROM_DEF(Item_ident, Item) {
  DBUG_EXECUTE_IF("simulate_item_clone_attr_copy_error", return true;);

  context = &select->context;

  if (orig_item->cached_table == nullptr) {
    m_tableno = orig_item->m_tableno;
  } else {
    m_tableno = orig_item->cached_table->m_tableno;
  }
}
PQ_COPY_FROM_RETURN

PQ_COPY_FROM_DEF(Item_field, Item_ident) {
  DBUG_EXECUTE_IF("simulate_item_field_copy_error", return true;);

  if (orig_item->table_ref != nullptr) {
    m_tableno = orig_item->table_ref->m_tableno;
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_field) {
  DBUG_EXECUTE_IF("simulate_item_clone_error", return nullptr;);
  DBUG_EXECUTE_IF("simulate_no_item_copy_function", return Item::pq_clone(thd, select););

  new_item = new (thd->pq_mem_root) Item_field(POS(), db_name, table_name, m_orig_field_name);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_default_value) {
  Item *new_arg = nullptr;
  if (arg) {
    new_arg = arg->pq_clone(thd, select);
    if (new_arg == nullptr) return nullptr;
  }
  new_item = new (thd->pq_mem_root) Item_default_value(POS(), new_arg);
}
PQ_CLONE_RETURN

int find_ref_in_table(Table_ref *tl, Item **ref) {
  int count = tl->field_translation_end - tl->field_translation;
  if (count <= 0) {
    return -1;
  }
  for (int i = 0; i < count; i++) {
    if (*ref == tl->field_translation[i].item) {
      return i;
    }
  }
  return -1;
}

Table_ref *get_table_in_merge_tablelist(Query_block *select, Table_ref *tb) {
  int tableindex = 0;
  for (Table_ref *tbl_list = select->orig->get_table_list(); tbl_list != nullptr; tbl_list = tbl_list->next_local) {
    if (tbl_list->merge_underlying_list != nullptr) {
      int index = get_table_index(tbl_list->merge_underlying_list, TABLE_LIST_TYPE_MERGE, tb);
      if (index != -1) {
        Table_ref *tbl = get_table_by_index(select->get_table_list(), TABLE_LIST_TYPE_DEFAULT, tableindex);
        return get_table_by_index(tbl, TABLE_LIST_TYPE_MERGE, index);
      }
    }
    tableindex++;
  }
  return nullptr;
}

Item *Item_view_ref::pq_clone(class THD *thd, class Query_block *select) {
  Item_view_ref *item = nullptr;
  Item **item_ref = nullptr;

  if (select->orig != nullptr) {
    Table_ref *found_table = nullptr;
    int index = get_table_index(select->orig->get_table_list(), TABLE_LIST_TYPE_DEFAULT, cached_table);
    if (index != -1) {
      found_table = get_table_by_index(select->get_table_list(), TABLE_LIST_TYPE_DEFAULT, index);
    }

    if (found_table == nullptr) {
      index = get_table_index(select->orig->leaf_tables, TABLE_LIST_TYPE_GLOBAL, cached_table);
      if (index != -1) {
        found_table = get_table_by_index(select->leaf_tables, TABLE_LIST_TYPE_GLOBAL, index);
      }
    }

    if (found_table == nullptr) {
      found_table = get_table_in_merge_tablelist(select, cached_table);
    }

    if (found_table == nullptr) {
      return nullptr;
    }

    int field_index = find_ref_in_table(cached_table, m_ref_item);
    if (field_index == -1 || found_table->field_translation == nullptr ||
        found_table->field_translation_end - found_table->field_translation <= field_index) {
      return nullptr;
    }

    item_ref = &found_table->field_translation[field_index].item;

    const char *db_name;
    if (found_table->is_view()) {
      db_name = found_table->db;
    } else {
      db_name = nullptr;
    }

    item = new (thd->pq_mem_root)
        Item_view_ref(&select->context, item_ref, db_name, table_name, original_table_name(), field_name, found_table);
  } else {
    item_ref = new (thd->pq_mem_root) Item *();
    if (item_ref == nullptr) return nullptr;
    *item_ref = (*m_ref_item)->pq_clone(thd, select);
    if (*item_ref == nullptr) return nullptr;
    item = new (thd->pq_mem_root)
        Item_view_ref(&select->context, item_ref, db_name, table_name, original_table_name(), field_name, cached_table);
  }

  if (item == nullptr || item->pq_copy_from(thd, select, this)) {
    return nullptr;
  }

  return item;
}

/**
   Item_aggregate_ref wil be created from ref in setup_fields() afterwards,
   so clone ref for the item.
*/
Item *Item_aggregate_ref::pq_clone(class THD *thd, class Query_block *select) {
  Item *item_ref = (*m_ref_item)->pq_clone(thd, select);
  if (item_ref == nullptr) {
    return nullptr;
  }

  return item_ref;
}

Item *Item_ref::pq_clone(class THD *thd, class Query_block *select) {
  /*
   * c1: (Name_resolution_context, db_name, table_name, field_name)
   * c2: (pos, db_name, table_name, field_name)
   * c3: (context, ref, db_name, table_name, field_name)
   * c4: (thd, ref_item)
   */
  Item_ref *new_item = nullptr;
  Name_resolution_context *new_context = &select->context;

  if (copy_type == PQ_copy_type::WITH_CONTEXT)
    new_item = new (thd->pq_mem_root) Item_ref(new_context, db_name, table_name, field_name);
  else if (copy_type == PQ_copy_type::WITHOUT_CONTEXT)
    new_item = new (thd->pq_mem_root) Item_ref(POS(), db_name, table_name, field_name);
  else if (copy_type == PQ_copy_type::WITH_CONTEXT_REF) {
    return (*m_ref_item)->pq_clone(thd, select);
  } else {
    assert(copy_type == PQ_copy_type::WITH_REF_ONLY);
    new_item = new (thd->pq_mem_root) Item_ref(thd, this);
  }
  if (new_item == nullptr || new_item->pq_copy_from(thd, select, this)) return nullptr;

  new_item->context = &select->context;
  return new_item;
}

PQ_CLONE_DEF(Item_name_const) {
  Item *name_arg, *val_arg;
  if (name_item == nullptr) {
    name_arg = nullptr;
  } else {
    name_arg = name_item->pq_clone(thd, select);
    if (name_arg == nullptr) return nullptr;
  }
  if (value_item == nullptr) {
    val_arg = nullptr;
  } else {
    val_arg = value_item->pq_clone(thd, select);
    if (val_arg == nullptr) return nullptr;
  }
  new_item = new (thd->pq_mem_root) Item_name_const(POS(), name_arg, val_arg);
}
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_name_const, Item) {
  if (orig_item != nullptr) {
    valid_args = orig_item->valid_args;
  }
}
PQ_COPY_FROM_RETURN

/* Item_result_field start */
/* Item_func start */
PQ_COPY_FROM_DEF(Item_func, Item_result_field) {
  if (orig_item != nullptr) {
    null_on_null = orig_item->null_on_null;
    used_tables_cache = orig_item->used_tables_cache;
    not_null_tables_cache = orig_item->not_null_tables_cache;
  }
}
PQ_COPY_FROM_RETURN

/* Item_func_bit start */
PQ_COPY_FROM_DEF(Item_func_bit, Item_func) {
  if (orig_item != nullptr) {
    hybrid_type = orig_item->hybrid_type;
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(PTI_literal_underscore_charset_hex_num) {
  LEX_STRING str = {const_cast<char *>(str_value.ptr()), str_value.length()};
  new_item = new (thd->pq_mem_root) PTI_literal_underscore_charset_hex_num(POS(), collation.collation, str);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_bit_neg, POS(), ARG0)

COPY_FUNC_ITEM(Item_func_bit_and, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_bit_or, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_bit_xor, POS(), ARG0, ARG1)

COPY_FUNC_ITEM(Item_func_shift_left, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_shift_right, POS(), ARG0, ARG1)
/* Item_func_bit end */

PQ_CLONE_DEF(Item_func_case) {
  PQ_CLONE_ARGS
  new_item = new (thd->pq_mem_root) Item_func_case(POS(), &item_list, nullptr, nullptr);
}
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_func_case, Item_func) {
  if (orig_item != nullptr) {
    first_expr_num = orig_item->first_expr_num;
    else_expr_num = orig_item->else_expr_num;
    cached_result_type = orig_item->cached_result_type;
    left_result_type = orig_item->left_result_type;
    ncases = orig_item->ncases;
    cmp_type = orig_item->cmp_type;
  }
}
PQ_COPY_FROM_RETURN

COPY_FUNC_ITEM(Item_func_if, ARG0, ARG1, ARG2)
COPY_FUNC_ITEM(Item_func_month, POS(), ARG0)

/* Item_func_coalesce start */
PQ_CLONE_DEF(Item_func_coalesce) {
  assert(arg_count < 3);
  Item *new_args[2] = {nullptr};
  for (uint i = 0; i < arg_count; i++) {
    new_args[i] = args[i]->pq_clone(thd, select);
    if (new_args[i] == nullptr) return nullptr;
  }
  if (arg_count == 1) {
    new_item = new (thd->pq_mem_root) Item_func_coalesce(POS(), new_args[0]);
  } else if (arg_count == 2) {
    new_item = new (thd->pq_mem_root) Item_func_coalesce(POS(), new_args[0], new_args[1]);
  }
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_any_value, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_ifnull, POS(), ARG0, ARG1)
/* Item_func_coalesce end */

/* Item_func_min_max start */
PQ_CLONE_DEF(Item_func_max) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_max(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_min) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_min(POS(), &pt_item_list);
}
PQ_CLONE_RETURN
/* Item_func_min_max end */

/* Item_func_num1 start */
COPY_FUNC_ITEM(Item_func_abs, POS(), ARG0)

COPY_FUNC_ITEM(Item_func_ceiling, ARG0)
COPY_FUNC_ITEM(Item_func_floor, ARG0)

COPY_FUNC_ITEM(Item_func_neg, ARG0)
COPY_FUNC_ITEM(Item_func_round, ARG0, ARG1, truncate)
/* Item_func_num1 end */

/* Item_num_op start */
COPY_FUNC_ITEM(Item_func_plus, ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_minus, ARG0, ARG1)

COPY_FUNC_ITEM(Item_func_div, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_mod, ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_mul, ARG0, ARG1)
/* Item_num_op end */

/* Item_func_regexp start */
PQ_CLONE_DEF(Item_func_regexp_instr) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;
  new_item = new (thd->pq_mem_root) Item_func_regexp_instr(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_regexp_like) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_regexp_like(POS(), &pt_item_list);
}
PQ_CLONE_RETURN
/* Item_func_regexp end */

/* Item_func_weekday start */
PQ_CLONE_DEF(Item_func_weekday) {
  PQ_CLONE_ARGS
  new_item = new (thd->pq_mem_root) Item_func_weekday(POS(), item_list[0], this->odbc_type);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_dayname, POS(), ARG0)
/* Item_func_weekday end */

/* Item_int_func start */
/* Item_bool_func2 start */
COPY_FUNC_ITEM(Item_func_eq, ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_equal, ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_ge, ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_gt, ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_le, ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_lt, ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_ne, ARG0, ARG1)

PQ_CLONE_DEF(Item_func_like) {
  Item *arg[3];
  for (uint i = 0; i < arg_count; i++) {
    arg[i] = args[i]->pq_clone(thd, select);
    if (arg[i] == nullptr) return nullptr;
  }

  if (arg_count == 2) {
    new_item = new (thd->pq_mem_root) Item_func_like(arg[0], arg[1]);
  } else if (arg_count == 3) {
    new_item = new (thd->pq_mem_root) Item_func_like(arg[0], arg[1], arg[2]);
  } else {
    sql_print_warning("arg_count is wrong!");
  }
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_nullif, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_mbrcontains, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_strcmp, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_xor, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_st_contains, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_mbrcoveredby, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_mbrcovers, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_st_crosses, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_st_disjoint, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_st_equals, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_st_intersects, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_st_overlaps, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_st_touches, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_st_within, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_mbrwithin, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_mbrtouches, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_mbroverlaps, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_mbrintersects, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_mbrequals, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_mbrdisjoint, POS(), ARG0, ARG1)
/* Item_bool_func2 end */

/* Item_cond start */
PQ_COPY_FROM_DEF(Item_cond, Item_bool_func) {
  Item *list_item;
  List_iterator_fast<Item> list_it(orig_item->list);
  while ((list_item = list_it++)) {
    Item *arg = list_item->pq_clone(thd, select);
    if (arg == nullptr) return true;
    list.push_back(arg);
  }
  if (orig_item != nullptr) {
    abort_on_null = orig_item->abort_on_null;
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_cond_and) { new_item = new (thd->pq_mem_root) Item_cond_and(); }
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_cond_and, Item_cond) {
  if (orig_item != nullptr) {
    cond_equal.max_members = orig_item->cond_equal.max_members;
  }
  Item_equal *item_equal;
  List_iterator_fast<Item_equal> it(orig_item->cond_equal.current_level);
  for (size_t i = 0; (item_equal = it++); i++) {
    Item_equal *new_item_equal = dynamic_cast<Item_equal *>(item_equal->pq_clone(thd, select));
    if (new_item_equal == nullptr) return true;
    cond_equal.current_level.push_back(new_item_equal);
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_cond_or) { new_item = new (thd->pq_mem_root) Item_cond_or(); }
PQ_CLONE_RETURN
/* Item_cond end */

PQ_CLONE_DEF(Item_equal) { new_item = new (thd->pq_mem_root) Item_equal(); }
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_equal, Item_bool_func) {
  Item_field *item_field;
  List_iterator_fast<Item_field> it(orig_item->fields);
  for (size_t i = 0; (item_field = it++); i++) {
    Item_field *new_field = dynamic_cast<Item_field *>(item_field->pq_clone(thd, select));
    if (new_field == nullptr) return true;
    fields.push_back(new_field);
  }
  if (orig_item != nullptr && orig_item->m_const_arg != nullptr) {
    m_const_arg = orig_item->m_const_arg->pq_clone(thd, select);
    if (m_const_arg == nullptr) {
      return true;
    }
  }
}
PQ_COPY_FROM_RETURN

COPY_FUNC_ITEM(Item_func_true, POS())

COPY_FUNC_ITEM(Item_func_isnotnull, ARG0)

PQ_CLONE_DEF(Item_func_isnull) {
  Item *arg = args[0]->pq_clone(thd, select);
  if (arg == nullptr) return nullptr;
  new_item = new (thd->pq_mem_root) Item_func_isnull(POS(), arg);
}
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_func_isnull, Item_bool_func) {
  if (orig_item != nullptr) {
    cached_value = orig_item->cached_value;
  }
}
PQ_COPY_FROM_RETURN

COPY_FUNC_ITEM(Item_func_json_schema_valid, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_not, ARG0)

PQ_CLONE_DEF(Item_func_truth) {
  PQ_CLONE_ARGS
  new_item = new (thd->pq_mem_root) Item_func_truth(POS(), item_list[0], truth_test);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_extract) {
  PQ_CLONE_ARGS
  new_item = new (thd->pq_mem_root) Item_extract(POS(), this->int_type, item_list[0]);
}
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_extract, Item_int_func) {
  if (orig_item != nullptr) {
    date_value = orig_item->date_value;
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_typecast_year) {
  PQ_CLONE_ARGS
  new_item = new (thd->pq_mem_root) Item_typecast_year(POS(), item_list[0]);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_ascii, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_bit_count, POS(), ARG0)

PQ_CLONE_DEF(Item_func_char_length) {
  assert(arg_count == 1);
  Item *arg = args[0]->pq_clone(thd, select);
  if (arg == nullptr) return nullptr;
  new_item = new (thd->pq_mem_root) Item_func_char_length(POS(), arg);
}
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_func_char_length, Item_int_func) {
  if (orig_item != nullptr) {
    value.copy(orig_item->value);
  }
}
PQ_COPY_FROM_RETURN

COPY_FUNC_ITEM(Item_func_coercibility, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_crc32, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_dayofmonth, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_dayofyear, POS(), ARG0)

PQ_CLONE_DEF(Item_func_field) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_field(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_find_in_set, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_get_lock, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_hour, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_inet_aton, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_div_int, POS(), ARG0, ARG1)

PQ_CLONE_DEF(Item_func_interval) {
  assert(arg_count == 1 && args[0]->type() == Item::ROW_ITEM);
  Item_row *row = down_cast<Item_row *>(args[0]->pq_clone(thd, select));
  if (nullptr == row) return nullptr;
  new_item = new (thd->pq_mem_root) Item_func_interval(POS(), row);
}
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_func_interval, Item_int_func) {
  if (orig_item != nullptr) {
    use_decimal_comparison = orig_item->use_decimal_comparison;
    intervals = orig_item->intervals;
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_func_json_contains) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_json_contains(thd, POS(), &pt_item_list);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_json_depth, POS(), ARG0)

PQ_CLONE_DEF(Item_func_last_insert_id) {
  Item *item_arg = nullptr;
  if (arg_count == 1) {
    item_arg = args[0]->pq_clone(thd, select);
    if (item_arg == nullptr) {
      return nullptr;
    }
  }

  if (arg_count == 0) {
    new_item = new (thd->pq_mem_root) Item_func_last_insert_id(POS());
  } else if (arg_count == 1) {
    new_item = new (thd->pq_mem_root) Item_func_last_insert_id(POS(), item_arg);
  }
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_length, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_bit_length, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_minute, POS(), ARG0)

PQ_CLONE_DEF(Item_func_locate) {
  assert(arg_count < 4);
  Item *new_args[4] = {nullptr};
  for (uint i = 0; i < arg_count; i++) {
    new_args[i] = args[i]->pq_clone(thd, select);
    if (new_args[i] == nullptr) return nullptr;
  }

  if (arg_count == 2) {
    new_item = new (thd->pq_mem_root) Item_func_locate(POS(), new_args[0], new_args[1]);
  } else if (arg_count == 3) {
    new_item = new (thd->pq_mem_root) Item_func_locate(POS(), new_args[0], new_args[1], new_args[2]);
  }
}
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_func_locate, Item_int_func) {
  if (orig_item != nullptr) {
    cmp_collation = orig_item->cmp_collation;
  }
}
PQ_COPY_FROM_RETURN

COPY_FUNC_ITEM(Item_func_instr, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_microsecond, POS(), ARG0)

PQ_COPY_FROM_DEF(Item_func_opt_neg, Item_int_func) {
  if (orig_item != nullptr) {
    negated = orig_item->negated;
    pred_level = orig_item->pred_level;
  }
}
PQ_COPY_FROM_RETURN

COPY_FUNC_ITEM(Item_func_between, POS(), ARG0, ARG1, ARG2, negated)

PQ_CLONE_DEF(Item_func_in) {
  PT_select_item_list pt_item;
  for (uint i = 0; i < arg_count; i++) {
    Item *arg = args[i]->pq_clone(thd, select);
    if (arg == nullptr) return nullptr;
    pt_item.value.push_back(arg);
  }
  new_item = new (thd->pq_mem_root) Item_func_in(POS(), &pt_item, negated);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_ord, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_period_add, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_period_diff, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_quarter, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_second, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_sleep, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_time_to_sec, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_timestamp_diff, POS(), ARG0, ARG1, int_type)
COPY_FUNC_ITEM(Item_func_to_days, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_to_seconds, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_uncompressed_length, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_week, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_year, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_yearweek, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_typecast_signed, POS(), ARG0)
COPY_FUNC_ITEM(Item_typecast_unsigned, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_can_access_table, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_is_visible_dd_object, POS(), ARG0)
/* Item_int_func end */

/* Item_real_func start */
/* Item_dec_func start*/
COPY_FUNC_ITEM(Item_func_sin, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_sqrt, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_cos, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_tan, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_cot, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_pow, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_ln, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_log2, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_log10, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_asin, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_acos, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_exp, POS(), ARG0)

PQ_CLONE_DEF(Item_func_atan) {
  Item *item_args[2];
  assert(arg_count < 3);
  for (uint i = 0; i < arg_count; i++) {
    item_args[i] = args[i]->pq_clone(thd, select);
    if (item_args[i] == nullptr) return nullptr;
  }

  if (arg_count == 1)
    new_item = new (thd->pq_mem_root) Item_func_atan(POS(), item_args[0]);
  else if (arg_count == 2)
    new_item = new (thd->pq_mem_root) Item_func_atan(POS(), item_args[0], item_args[1]);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_log) {
  Item *item_args[2];
  assert(arg_count < 3);
  for (uint i = 0; i < arg_count; i++) {
    item_args[i] = args[i]->pq_clone(thd, select);
    if (item_args[i] == nullptr) return nullptr;
  }

  if (arg_count == 1)
    new_item = new (thd->pq_mem_root) Item_func_log(POS(), item_args[0]);
  else if (arg_count == 2)
    new_item = new (thd->pq_mem_root) Item_func_log(POS(), item_args[0], item_args[1]);
}
PQ_CLONE_RETURN

/* Item_dec_func end*/

COPY_FUNC_ITEM(Item_func_longfromgeohash, POS(), ARG0)

PQ_CLONE_DEF(Item_func_rand) { new_item = new (thd->pq_mem_root) Item_func_rand(POS()); }
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_latfromgeohash, POS(), ARG0)
/* Item_real_func end */

/* Item_str_func start */
PQ_CLONE_DEF(Item_func_aes_decrypt) {
  assert(arg_count < 4);
  Item *new_args[4] = {nullptr};
  for (uint i = 0; i < arg_count; i++) {
    new_args[i] = args[i]->pq_clone(thd, select);
    if (new_args[i] == nullptr) return nullptr;
  }

  if (arg_count == 2) {
    new_item = new (thd->pq_mem_root) Item_func_aes_decrypt(POS(), new_args[0], new_args[1]);
  } else if (arg_count == 3) {
    new_item = new (thd->pq_mem_root) Item_func_aes_decrypt(POS(), new_args[0], new_args[1], new_args[2]);
  }
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_aes_encrypt) {
  assert(arg_count < 4);
  Item *new_args[4] = {nullptr};
  for (uint i = 0; i < arg_count; i++) {
    new_args[i] = args[i]->pq_clone(thd, select);
    if (new_args[i] == nullptr) return nullptr;
  }

  if (arg_count == 2) {
    new_item = new (thd->pq_mem_root) Item_func_aes_encrypt(POS(), new_args[0], new_args[1]);
  } else if (arg_count == 3) {
    new_item = new (thd->pq_mem_root) Item_func_aes_encrypt(POS(), new_args[0], new_args[1], new_args[2]);
  }
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_char) {
  PQ_CLONE_ARGS
  PT_item_list pt_item_list;
  pt_item_list.value = item_list;
  new_item = new (thd->pq_mem_root) Item_func_char(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_charset, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_collation, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_compress, POS(), ARG0)

PQ_CLONE_DEF(Item_func_concat) {
  PQ_CLONE_ARGS
  PT_item_list pt_item_list;
  pt_item_list.value = item_list;
  new_item = new (thd->pq_mem_root) Item_func_concat(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_concat_ws) {
  PQ_CLONE_ARGS
  PT_item_list pt_item_list;
  pt_item_list.value = item_list;
  new_item = new (thd->pq_mem_root) Item_func_concat_ws(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_conv, POS(), ARG0, ARG1, ARG2)
COPY_FUNC_ITEM(Item_func_conv_charset, POS(), ARG0, m_cast_cs)

PQ_CLONE_DEF(Item_func_date_format) {
  PQ_CLONE_ARGS
  new_item = new (thd->pq_mem_root) Item_func_date_format(POS(), item_list[0], item_list[1], this->is_time_format);
}
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_func_date_format, Item_str_func) {
  if (orig_item != nullptr) {
    value.copy(orig_item->value);
    fixed_length = orig_item->fixed_length;
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_func_elt) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_elt(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_export_set) {
  PQ_CLONE_ARGS

  if (arg_count == 3) {
    new_item = new (thd->pq_mem_root) Item_func_export_set(POS(), item_list[0], item_list[1], item_list[2]);
  } else if (arg_count == 4) {
    new_item =
        new (thd->pq_mem_root) Item_func_export_set(POS(), item_list[0], item_list[1], item_list[2], item_list[3]);
  } else if (arg_count == 5) {
    new_item = new (thd->pq_mem_root)
        Item_func_export_set(POS(), item_list[0], item_list[1], item_list[2], item_list[3], item_list[4]);
  }
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_from_base64, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_inet_ntoa, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_insert, POS(), ARG0, ARG1, ARG2, ARG3)

PQ_CLONE_DEF(Item_func_json_quote) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_json_quote(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_left, POS(), ARG0, ARG1)

COPY_FUNC_ITEM(Item_func_lpad, POS(), ARG0, ARG1, ARG2)

PQ_COPY_FROM_DEF(Item_func_lpad, Item_str_func) {
  if (orig_item != nullptr) lpad_str.copy(orig_item->lpad_str);
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_func_make_set) {
  Item *arg_a = item->pq_clone(thd, select);
  if (arg_a == nullptr) return nullptr;

  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_make_set(POS(), arg_a, &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_internal_table_rows) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_internal_table_rows(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_internal_avg_row_length) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_internal_avg_row_length(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_internal_data_length) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_internal_data_length(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_internal_max_data_length) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_internal_max_data_length(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_internal_index_length) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_internal_index_length(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_internal_data_free) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_internal_data_free(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_internal_auto_increment) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_internal_auto_increment(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_internal_update_time) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_internal_update_time(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_internal_check_time) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_internal_check_time(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_internal_checksum) {
  PQ_CLONE_ARGS

  PT_item_list pt_item_list;
  pt_item_list.value = item_list;

  new_item = new (thd->pq_mem_root) Item_func_internal_checksum(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_internal_get_comment_or_error) {
  PQ_CLONE_ARGS
  PT_item_list pt_item_list;
  pt_item_list.value = item_list;
  new_item = new (thd->pq_mem_root) Item_func_internal_get_comment_or_error(POS(), &pt_item_list);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_monthname, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_pfs_format_bytes, POS(), ARG0)

PQ_COPY_FROM_DEF(Item_func_pfs_format_bytes, Item_str_func) {
  if (orig_item != nullptr) {
    m_value = orig_item->m_value;
    memcpy(orig_item->m_value_buffer, m_value_buffer, 20);
  }
}
PQ_COPY_FROM_RETURN

COPY_FUNC_ITEM(Item_func_pfs_format_pico_time, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_quote, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_repeat, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_replace, POS(), ARG0, ARG1, ARG2)
COPY_FUNC_ITEM(Item_func_reverse, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_random_bytes, POS(), ARG0)

PQ_CLONE_DEF(Item_func_right) {
  PQ_CLONE_ARGS
  new_item = new (thd->pq_mem_root) Item_func_right(POS(), item_list[0], item_list[1]);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_rpad, POS(), ARG0, ARG1, ARG2)

PQ_COPY_FROM_DEF(Item_func_rpad, Item_str_func) {
  if (orig_item != nullptr) {
    rpad_str.copy(orig_item->rpad_str);
  }
}
PQ_COPY_FROM_RETURN

COPY_FUNC_ITEM(Item_func_set_collation, POS(), ARG0, collation_string)

PQ_COPY_FROM_DEF(Item_func_set_collation, Item_str_func) {
  if (orig_item != nullptr && orig_item->args[1] != nullptr) {
    args[1] = orig_item->args[1]->pq_clone(thd, select);
    if (args[1] == nullptr) return true;
  }
}
PQ_COPY_FROM_RETURN

COPY_FUNC_ITEM(Item_func_soundex, ARG0)
COPY_FUNC_ITEM(Item_func_space, POS(), ARG0)

PQ_CLONE_DEF(Item_func_substr) {
  assert(arg_count < 4);
  Item *new_args[4] = {nullptr};
  for (uint i = 0; i < arg_count; i++) {
    new_args[i] = args[i]->pq_clone(thd, select);
    if (new_args[i] == nullptr) return nullptr;
  }

  if (arg_count == 2) {
    new_item = new (thd->pq_mem_root) Item_func_substr(POS(), new_args[0], new_args[1]);
  } else if (arg_count == 3) {
    new_item = new (thd->pq_mem_root) Item_func_substr(POS(), new_args[0], new_args[1], new_args[2]);
  }
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_substr_index, POS(), ARG0, ARG1, ARG2)
COPY_FUNC_ITEM(Item_func_database, POS())
COPY_FUNC_ITEM(Item_func_user, POS())

PQ_CLONE_DEF(Item_func_trim) {
  PQ_CLONE_ARGS

  if (arg_count > 1)
    new_item = new (thd->pq_mem_root) Item_func_trim(POS(), item_list[0], item_list[1], m_trim_mode);
  else
    new_item = new (thd->pq_mem_root) Item_func_trim(POS(), item_list[0], m_trim_mode);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_ltrim, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_rtrim, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_uncompress, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_unhex, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_uuid, POS())
COPY_FUNC_ITEM(Item_func_get_dd_create_options, POS(), ARG0, ARG1, ARG2)
PQ_CLONE_DEF(Item_func_uuid_to_bin) {
  assert(arg_count < 3);
  Item *new_args[4] = {nullptr};

  for (uint i = 0; i < arg_count; i++) {
    new_args[i] = args[i]->pq_clone(thd, select);
    if (new_args[i] == nullptr) return nullptr;
  }

  if (arg_count == 1) {
    new_item = new (thd->pq_mem_root) Item_func_uuid_to_bin(POS(), new_args[0]);
  } else if (arg_count == 2) {
    new_item = new (thd->pq_mem_root) Item_func_uuid_to_bin(POS(), new_args[0], new_args[1]);
  }
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_weight_string, POS(), ARG0, result_length, num_codepoints, flags, as_binary)
COPY_FUNC_ITEM(Item_func_st_srid_mutator, POS(), ARG0, ARG1)

PQ_CLONE_DEF(Item_func_bin_to_uuid) {
  assert(arg_count < 3);
  Item *new_args[4] = {nullptr};

  for (uint i = 0; i < arg_count; i++) {
    new_args[i] = args[i]->pq_clone(thd, select);
    if (new_args[i] == nullptr) return nullptr;
  }

  if (arg_count == 1) {
    new_item = new (thd->pq_mem_root) Item_func_bin_to_uuid(POS(), new_args[0]);
  } else if (arg_count == 2) {
    new_item = new (thd->pq_mem_root) Item_func_bin_to_uuid(POS(), new_args[0], new_args[1]);
  }
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_format) {
  assert(arg_count < 4);
  Item *new_args[4] = {nullptr};
  for (uint i = 0; i < arg_count; i++) {
    new_args[i] = args[i]->pq_clone(thd, select);
    if (new_args[i] == nullptr) return nullptr;
  }

  if (arg_count == 2)
    new_item = new (thd->pq_mem_root) Item_func_format(POS(), new_args[0], new_args[1]);
  else if (arg_count == 3)
    new_item = new (thd->pq_mem_root) Item_func_format(POS(), new_args[0], new_args[1], new_args[2]);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_get_format) {
  assert(arg_count == 1);
  Item *arg = args[0]->pq_clone(thd, select);
  if (arg == nullptr) return nullptr;

  new_item = new (thd->pq_mem_root) Item_func_get_format(POS(), type, arg);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_hex, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_inet6_aton, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_inet6_ntoa, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_md5, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_sha, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_sha2, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_to_base64, POS(), ARG0)

PQ_COPY_FROM_DEF(Item_str_conv, Item_str_func) {
  if (orig_item != nullptr) {
    multiply = orig_item->multiply;
    converter = orig_item->converter;
  }
}
PQ_COPY_FROM_RETURN

COPY_FUNC_ITEM(Item_func_upper, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_lower, POS(), ARG0)

PQ_COPY_FROM_DEF(Item_temporal_hybrid_func, Item_str_func) {
  if (orig_item != nullptr) {
    sql_mode = orig_item->sql_mode;
    ascii_buf.copy(orig_item->ascii_buf);
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_date_add_interval) {
  Item *arg_a = args[0]->pq_clone(thd, select);
  Item *arg_b = args[1]->pq_clone(thd, select);
  if (arg_a == nullptr || arg_b == nullptr) return nullptr;
  new_item = new (thd->pq_mem_root) Item_date_add_interval(arg_a, arg_b, m_interval_type, m_subtract);
  if (new_item) {
    new_item->set_data_type(data_type());
  }
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_add_time, POS(), ARG0, ARG1, m_datetime, m_sign == -1 ? true : false)

COPY_FUNC_ITEM(Item_func_str_to_date, POS(), ARG0, ARG1)

PQ_COPY_FROM_DEF(Item_func_str_to_date, Item_temporal_hybrid_func) {
  if (orig_item != nullptr) {
    cached_timestamp_type = orig_item->cached_timestamp_type;
  }
}
PQ_COPY_FROM_RETURN

COPY_FUNC_ITEM(Item_typecast_char, thd, ARG0, m_cast_length, m_cast_cs)

PQ_CLONE_DEF(Item_date_literal) {
  MYSQL_TIME ltime;
  cached_time.get_time(&ltime);
  new_item = new (thd->pq_mem_root) Item_date_literal(&ltime);
}
PQ_CLONE_RETURN
/* Item_str_func end */

COPY_FUNC_ITEM(Item_func_curdate_utc, POS())
COPY_FUNC_ITEM(Item_func_curdate_local, POS())
COPY_FUNC_ITEM(Item_func_from_days, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_makedate, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_typecast_date, POS(), ARG0)

PQ_COPY_FROM_DEF(Item_typecast_date, Item_date_func) {
  if (orig_item != nullptr) {
    m_explicit_cast = orig_item->m_explicit_cast;
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_datetime_literal) {
  MYSQL_TIME *ltime = new (thd->pq_mem_root) MYSQL_TIME();
  if (ltime != nullptr) {
    this->get_date(ltime, 0);
    new_item =
        new (thd->pq_mem_root) Item_datetime_literal(ltime, this->cached_time.decimals(), thd->variables.time_zone);
  }
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_convert_tz, POS(), ARG0, ARG1, ARG2)
COPY_FUNC_ITEM(Item_func_from_unixtime, POS(), ARG0)

PQ_CLONE_DEF(Item_func_sysdate_local) { new_item = new (thd->pq_mem_root) Item_func_sysdate_local(decimals); }
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_typecast_datetime) {
  if (origin_item) {
    return origin_item->pq_clone(thd, select);
  }
  Item *arg_item = args[0]->pq_clone(thd, select);
  if (arg_item == nullptr) return nullptr;

  new_item = new (thd->pq_mem_root) Item_typecast_datetime(POS(), arg_item);
}
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_typecast_datetime, Item_datetime_func) {
  if (orig_item != nullptr) {
    detect_precision_from_arg = orig_item->detect_precision_from_arg;
    decimals = orig_item->decimals;
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_func_at_time_zone) {
  Item *arg_item = args[0]->pq_clone(thd, select);
  if (arg_item == nullptr) return nullptr;

  new_item = new (thd->pq_mem_root) Item_func_at_time_zone(POS(), arg_item, m_specifier_string, m_is_interval);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_curtime_local) {
  PQ_CLONE_ARGS
  new_item = new (thd->pq_mem_root) Item_func_curtime_local(POS(), this->decimals);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_curtime_utc) { new_item = new (thd->pq_mem_root) Item_func_curtime_utc(POS(), decimals); }
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_func_maketime, POS(), ARG0, ARG1, ARG2)
COPY_FUNC_ITEM(Item_func_sec_to_time, POS(), ARG0)
COPY_FUNC_ITEM(Item_func_timediff, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_typecast_time, POS(), ARG0)

PQ_COPY_FROM_DEF(Item_typecast_time, Item_time_func) {
  if (orig_item != nullptr) {
    detect_precision_from_arg = orig_item->detect_precision_from_arg;
    decimals = orig_item->decimals;
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_time_literal) {
  MYSQL_TIME *ltime = new (thd->pq_mem_root) MYSQL_TIME();
  if (ltime == nullptr) return nullptr;
  cached_time.get_time(ltime);
  new_item = new (thd->pq_mem_root) Item_time_literal(ltime, pq_dec_arg);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_typecast_decimal) {
  Item *item_arg = args[0]->pq_clone(thd, select);
  if (item_arg == nullptr) return nullptr;

  new_item = new (thd->pq_mem_root) Item_typecast_decimal(POS(), item_arg, pq_precision, decimals);
}
PQ_CLONE_RETURN

COPY_FUNC_ITEM(Item_typecast_real, ARG0)

PQ_CLONE_DEF(Item_func_get_system_var) {
  new_item = new (thd->pq_mem_root) Item_func_get_system_var(var_tracker, var_scope);
}
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_func_get_system_var, Item_var_func) {
  if (orig_item != nullptr) {
    cached_llval = orig_item->cached_llval;
    cached_dval = orig_item->cached_dval;
    cached_strval.copy(orig_item->cached_strval);
    cached_null_value = orig_item->cached_null_value;
    used_query_id = orig_item->used_query_id;
    cache_present = orig_item->cache_present;
  }
}
PQ_COPY_FROM_RETURN
/* Item_func end */

/* Item sum start */
PQ_COPY_FROM_DEF(Item_sum, Item_result_field) {
  if (orig_item != nullptr) {
    force_copy_fields = orig_item->force_copy_fields;
    with_distinct = orig_item->with_distinct;
    max_aggr_level = orig_item->max_aggr_level;
    max_sum_func_level = orig_item->max_sum_func_level;
    allow_group_via_temp_table = orig_item->allow_group_via_temp_table;
    save_deny_window_func = orig_item->save_deny_window_func;
    used_tables_cache = orig_item->used_tables_cache;
    forced_const = orig_item->forced_const;
  }
}
PQ_COPY_FROM_RETURN

Item_sum *Item_sum::pq_rebuild_sum_func(THD *thd MY_ATTRIBUTE((unused)), Query_block *select MY_ATTRIBUTE((unused)),
                                        Item *item MY_ATTRIBUTE((unused))) {
  sql_print_warning(
      "Item type %s's rebuild sum method is not implemented, "
      "will not use parallel query, SQL= %s",
      typeid(*this).name(), thd->query().str);
  assert(DBUG_EVALUATE_IF("simulate_no_item_rebuild_function", true, false) || false);
  return nullptr;
}

PQ_COPY_FROM_DEF(Item_sum_bit, Item_sum) {
  if (orig_item != nullptr) {
    reset_bits = orig_item->reset_bits;
    bits = orig_item->bits;
    hybrid_type = orig_item->hybrid_type;
    m_count = orig_item->m_count;
    m_frame_null_count = orig_item->m_frame_null_count;
  }
  m_digit_cnt = nullptr;
  m_digit_cnt_card = 0;
  if (orig_item != nullptr) {
    m_is_xor = orig_item->m_is_xor;
  }
}
PQ_COPY_FROM_RETURN

PQ_REBUILD_SUM_DEF(Item_sum_and) { new_item = new (thd->pq_mem_root) Item_sum_and(POS(), item, nullptr); }
PQ_REBUILD_SUM_RETURN

PQ_CLONE_DEF(Item_sum_and) {
  Item *arg = args[0]->pq_clone(thd, select);
  if (arg == nullptr) return nullptr;

  new_item = new (thd->pq_mem_root) Item_sum_and(POS(), arg, nullptr);
}
PQ_CLONE_RETURN

PQ_REBUILD_SUM_DEF(Item_sum_or) { new_item = new (thd->pq_mem_root) Item_sum_or(POS(), item, nullptr); }
PQ_REBUILD_SUM_RETURN

PQ_CLONE_DEF(Item_sum_or) {
  Item *arg = args[0]->pq_clone(thd, select);
  if (arg == nullptr) return nullptr;

  new_item = new (thd->pq_mem_root) Item_sum_or(POS(), arg, nullptr);
}
PQ_CLONE_RETURN

PQ_REBUILD_SUM_DEF(Item_sum_xor) { new_item = new (thd->pq_mem_root) Item_sum_xor(POS(), item, nullptr); }
PQ_REBUILD_SUM_RETURN

PQ_CLONE_DEF(Item_sum_xor) {
  Item *arg = args[0]->pq_clone(thd, select);
  if (nullptr == arg) return nullptr;

  new_item = new (thd->pq_mem_root) Item_sum_xor(POS(), arg, nullptr);
}
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_sum_hybrid, Item_sum) {
  if (orig_item == nullptr) {
    return true;
  }

  hybrid_type = orig_item->hybrid_type;
  m_has_values = orig_item->m_has_values;
  m_nulls_first = orig_item->m_nulls_first;
  m_optimize = orig_item->m_optimize;
  m_want_first = orig_item->m_want_first;
  m_cnt = orig_item->m_cnt;
  m_saved_last_value_at = orig_item->m_saved_last_value_at;
}
PQ_COPY_FROM_RETURN

COPY_FUNC_ITEM(Item_sum_max, ARG0)

PQ_REBUILD_SUM_DEF(Item_sum_max) { new_item = new (thd->pq_mem_root) Item_sum_max(POS(), item, nullptr); }
PQ_REBUILD_SUM_RETURN

COPY_FUNC_ITEM(Item_sum_min, ARG0)

PQ_REBUILD_SUM_DEF(Item_sum_min) { new_item = new (thd->pq_mem_root) Item_sum_min(POS(), item, nullptr); }
PQ_REBUILD_SUM_RETURN

PQ_COPY_FROM_DEF(Item_sum_num, Item_sum) {
  DBUG_EXECUTE_IF("simulate_item_rebuild_attr_copy_error", return true;);
  if (orig_item != nullptr) {
    is_evaluated = orig_item->is_evaluated;
  }
}
PQ_COPY_FROM_RETURN

COPY_FUNC_ITEM(Item_sum_count, POS(), ARG0, nullptr)

Item_sum *Item_sum_count::pq_rebuild_sum_func(THD *thd, Query_block *select, Item *item) {
  DBUG_EXECUTE_IF("simulate_item_rebuild_error", return nullptr;);
  DBUG_EXECUTE_IF("simulate_no_item_rebuild_function", return Item_sum::pq_rebuild_sum_func(thd, select, item););

  Item_sum_count *new_item_sum = new (thd->pq_mem_root) Item_sum_count(POS(), item, nullptr, true);
  if (new_item_sum == nullptr || new_item_sum->Item_sum_num::pq_copy_from(thd, select, this)) return nullptr;
  return new_item_sum;
}

Item *PTI_count_sym::pq_clone(THD *thd, Query_block *select) {
  CHECK_TYPE(PTI_count_sym)
  Item *arg = args[0]->pq_clone(thd, select);
  if (arg == nullptr) return nullptr;
  Item_sum_count *new_count = new (thd->pq_mem_root) Item_sum_count(POS(), arg, nullptr);
  if (new_count == nullptr || new_count->pq_copy_from(thd, select, this)) return nullptr;
  return new_count;
}

COPY_FUNC_ITEM(Item_sum_sum, POS(), ARG0, has_with_distinct(), nullptr)

PQ_REBUILD_SUM_DEF(Item_sum_sum) {
  new_item = new (thd->pq_mem_root) Item_sum_sum(POS(), item, has_with_distinct(), nullptr);
}
PQ_REBUILD_SUM_RETURN

PQ_CLONE_DEF(Item_sum_avg) {
  assert(arg_count == 1);
  Item *arg = args[0]->pq_clone(thd, select);
  if (arg == nullptr) return nullptr;
  new_item = new (thd->pq_mem_root) Item_sum_avg(POS(), arg, has_with_distinct(), nullptr);
  if (new_item) {
    new_item->pq_avg_type = ParallelAvgType::PQ_WORKER;
  }
}
PQ_CLONE_RETURN

PQ_REBUILD_SUM_DEF(Item_sum_avg) {
  new_item = new (thd->pq_mem_root) Item_sum_avg(POS(), item, has_with_distinct(), nullptr);
  if (new_item) {
    new_item->pq_avg_type = ParallelAvgType::PQ_REBUILD;
  }
}
PQ_REBUILD_SUM_RETURN
/* Item sum end */
/* Item_result_field end */

PQ_CLONE_DEF(Item_row) {
  assert(arg_count > 0);
  Item *arg_head = items[0]->pq_clone(thd, select);
  if (arg_head == nullptr) return nullptr;
  mem_root_deque<Item *> tail(thd->pq_mem_root);
  for (uint i = 1; i < arg_count; i++) {
    Item *arg_tail = items[i]->pq_clone(thd, select);
    if (arg_tail == nullptr) return nullptr;
    tail.push_back(arg_tail);
  }
  new_item = new (thd->pq_mem_root) Item_row(arg_head, tail);
}
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_row, Item) {
  // generated a random item_name for item_row
  if (orig_item != nullptr) {
    if (orig_item->item_name.length() == 0) {
      assert(orig_item->item_name.ptr() == nullptr);
      uint32 addr_mid_8 = ((uint64)this >> 32) << 24;
      std::string std_addr = "ITEM_ROW" + std::to_string(addr_mid_8);
      item_name.copy(std_addr.c_str(), std_addr.length(), system_charset_info, true);
    }
    used_tables_cache = orig_item->used_tables_cache;
    not_null_tables_cache = orig_item->not_null_tables_cache;
    with_null = orig_item->with_null;
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_float) { new_item = new (thd->pq_mem_root) Item_float(item_name, value, decimals, max_length); }
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_int) { new_item = new (thd->pq_mem_root) Item_int(this); }
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_int, Item_num) {
  if (orig_item != nullptr) {
    value = orig_item->value;
  }
}
PQ_COPY_FROM_RETURN

PQ_CLONE_DEF(Item_uint) { new_item = new (thd->pq_mem_root) Item_uint(item_name, value, max_length); }
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_decimal) {
  new_item = new (thd->pq_mem_root) Item_decimal(item_name, &decimal_value, decimals, max_length);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_version) { new_item = new (thd->pq_mem_root) Item_func_version(POS()); }
PQ_CLONE_RETURN

PQ_CLONE_DEF(PTI_function_call_nonkeyword_now) {
  new_item = new (thd->pq_mem_root) PTI_function_call_nonkeyword_now(POS(), decimals);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(PTI_text_literal_text_string) {
  new_item = new (thd->pq_mem_root) PTI_text_literal_text_string(POS(), is_7bit, literal);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(PTI_text_literal_nchar_string) {
  new_item = new (thd->pq_mem_root) PTI_text_literal_nchar_string(POS(), is_7bit, literal);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(PTI_text_literal_underscore_charset) {
  new_item = new (thd->pq_mem_root) PTI_text_literal_underscore_charset(POS(), is_7bit, cs, literal);
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(PTI_user_variable) {
  new_item = new (thd->pq_mem_root) PTI_user_variable(POS(), {const_cast<char *>(name.ptr()), name.length()});
}
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_get_user_var) { new_item = new (thd->pq_mem_root) Item_func_get_user_var(POS(), name); }
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_connection_id) { new_item = new (thd->pq_mem_root) Item_func_connection_id(POS()); }
PQ_CLONE_RETURN

PQ_CLONE_DEF(Item_func_trig_cond) {
  Item *arg = nullptr;
  if (arg_count > 0) arg = args[0]->pq_clone(thd, select);
  if (arg == nullptr) return nullptr;
  new_item = new (thd->pq_mem_root)
      Item_func_trig_cond(arg, trig_var, thd->lex->unit->first_query_block()->join, m_idx, trig_type);
}
PQ_CLONE_RETURN

PQ_COPY_FROM_DEF(Item_func_connection_id, Item_int_func) {
  if (orig_item != nullptr) {
    // value = orig_item->value;
  }
}
PQ_COPY_FROM_RETURN

Item *Item_func_unix_timestamp::pq_clone(THD *thd, Query_block *select) {
  Item *arg_item = nullptr;
  if (arg_count > 0) {
    arg_item = args[0]->pq_clone(thd, select);
    if (arg_item == nullptr) return nullptr;
  }

  Item_func_unix_timestamp *new_item = nullptr;
  if (arg_count) {
    new_item = new (thd->pq_mem_root) Item_func_unix_timestamp(POS(), arg_item);
  } else {
    new_item = new (thd->pq_mem_root) Item_func_unix_timestamp(POS());
  }

  if (!new_item || new_item->pq_copy_from(thd, select, this)) return nullptr;

  return new_item;
}

Item *Item_func_current_user::pq_clone(THD *thd, Query_block *select) {
  Item_func_current_user *new_item = new (thd->pq_mem_root) Item_func_current_user(POS());
  if (!new_item || new_item->pq_copy_from(thd, select, this)) return nullptr;

  new_item->context = &select->context;
  return new_item;
}

COPY_FUNC_ITEM(Item_func_benchmark, POS(), ARG0, ARG1)
COPY_FUNC_ITEM(Item_func_found_rows, POS())

Item *Item_func_false::pq_clone(THD *thd, Query_block *select) {
  CHECK_TYPE(Item_func_false);

  Item *new_item = new (thd->pq_mem_root) Item_func_false(POS());
  COPY_SELF_ATTR(new_item)

  if (item_name.ptr() == antijoin_null_cond) {
    new_item->item_name.set(antijoin_null_cond);
  }

  return new_item;
}

Item *Item_datetime_with_ref::pq_clone(THD *thd, Query_block *select) {
  CHECK_TYPE(Item_datetime_with_ref);
  if (origin_item) {
    return origin_item->pq_clone(thd, select);
  }

  return ref->pq_clone(thd, select);
}

Item *Item_time_with_ref::pq_clone(THD *thd, Query_block *select) {
  CHECK_TYPE(Item_time_with_ref);
  if (origin_item) {
    return origin_item->pq_clone(thd, select);
  }

  return ref->pq_clone(thd, select);
}

#endif  // __SHANNONBASE_SQL_PQ_CLONE_ITEM_H__