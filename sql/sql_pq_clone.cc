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

#include "sql/sql_pq_clone.h"

#include "include/my_dbug.h"
#include "include/mysql/psi/mysql_thread.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/mysqld.h"
#include "sql/range_optimizer/range_optimizer.h"
#include "sql/sql_base.h"
#include "sql/sql_lex.h"
#include "sql/sql_opt_exec_shared.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_parallel.h"
#include "sql/sql_resolver.h"
#include "sql/system_variables.h"

class COND_CMP;
bool propagate_cond_constants(THD *thd, I_List<COND_CMP> *save_list, Item *and_father, Item *cond);

bool POSITION::pq_copy(THD *thd, POSITION *orig) {
  rows_fetched = orig->rows_fetched;
  read_cost = orig->read_cost;
  filter_effect = orig->filter_effect;
  prefix_rowcount = orig->prefix_rowcount;
  prefix_cost = orig->prefix_cost;
  table = orig->table;
  if (orig->key) {
    key = orig->key->pq_clone(thd);
    if (key == nullptr) {
      return true;
    }
  } else {
    key = nullptr;
  }
  ref_depend_map = orig->ref_depend_map;
  use_join_buffer = orig->use_join_buffer;
  sj_strategy = orig->sj_strategy;
  n_sj_tables = orig->n_sj_tables;
  dups_producing_tables = orig->dups_producing_tables;
  first_loosescan_table = orig->first_loosescan_table;
  loosescan_need_tables = orig->loosescan_need_tables;
  loosescan_key = orig->loosescan_key;
  loosescan_parts = orig->loosescan_parts;
  first_firstmatch_table = orig->first_firstmatch_table;
  first_firstmatch_rtbl = orig->first_firstmatch_rtbl;
  firstmatch_need_tables = orig->firstmatch_need_tables;
  first_dupsweedout_table = orig->first_dupsweedout_table;
  dupsweedout_tables = orig->dupsweedout_tables;
  sjm_scan_last_inner = orig->sjm_scan_last_inner;
  sjm_scan_need_tables = orig->sjm_scan_need_tables;

  return false;
}

bool QEP_TAB::pq_copy(THD *thd, QEP_TAB *orig) {
  set_type(orig->type());
  set_index(orig->index());
  set_first_inner(orig->first_inner());
  set_last_inner(orig->last_inner());
  set_first_sj_inner(orig->first_sj_inner());
  set_last_sj_inner(orig->last_sj_inner());
  keys().merge(orig->keys());
  m_reversed_access = orig->m_reversed_access;
  do_parallel_scan = orig->do_parallel_scan;
  firstmatch_return = orig->firstmatch_return;
  loosescan_key_len = orig->loosescan_key_len;
  POSITION *position = new (thd->pq_mem_root) POSITION;
  if (!position || position->pq_copy(thd, orig->position())) {
    return true;
  }

  set_position(position);
  if (orig->pq_cond) {
    JOIN *join = this->join();
    if (join == nullptr) {
      return true;
    }
    pq_cond = orig->pq_cond->pq_clone(join->thd, join->query_block);
    if (pq_cond == nullptr) {
      return true;
    }
  }

  return false;
}

bool TABLE::pq_copy(THD *thd, void *select_arg, TABLE *orig) {
  Query_block *select = static_cast<Query_block *>(select_arg);
  possible_quick_keys = orig->possible_quick_keys;
  covering_keys = orig->covering_keys;
  key_read = orig->key_read;
  const_table = orig->const_table;
  nullable = orig->nullable;
  null_row = orig->null_row;
  m_cost_model = orig->m_cost_model;
  memcpy(record[0], orig->record[0], orig->s->rec_buff_length);

  reginfo = orig->reginfo;

  file->pushed_idx_cond_keyno = orig->file->pushed_idx_cond_keyno;
  Item *index_pushdown = orig->file->pushed_idx_cond;
  // needs deep copy
  file->pushed_idx_cond = index_pushdown ? index_pushdown->pq_clone(thd, select) : nullptr;
  Item *copy_index_pushdown = file->pushed_idx_cond;
  if ((index_pushdown && copy_index_pushdown == nullptr) ||
      (copy_index_pushdown && copy_index_pushdown->fix_fields(thd, &copy_index_pushdown))) {
    return true;
  }

  return false;
}

/*
 * copy table_ref info.
 *
 * @retval:
 *    false if copy successfully, and otherwise true.
 */
bool Index_lookup::pq_copy(JOIN *join, Index_lookup *ref, QEP_TAB *qep_tab) {
  THD *thd = join->thd;
  key_parts = ref->key_parts;
  key_length = ref->key_length;
  key_err = ref->key_err;
  key = ref->key;
  null_rejecting = ref->null_rejecting;
  depend_map = ref->depend_map;
  use_count = ref->use_count;
  disable_cache = ref->disable_cache;

  if (!(key_buff = thd->pq_mem_root->ArrayAlloc<uchar>(ALIGN_SIZE(key_length))) ||
      !(key_buff2 = thd->pq_mem_root->ArrayAlloc<uchar>(ALIGN_SIZE(key_length))) ||
      !(key_copy = thd->pq_mem_root->ArrayAlloc<store_key *>(key_parts)) ||
      !(items = thd->pq_mem_root->ArrayAlloc<Item *>(key_parts)) ||
      !(cond_guards = thd->pq_mem_root->ArrayAlloc<bool *>(key_parts))) {
    return true;
  }

  if (ref->null_ref_key != nullptr) {
    null_ref_key = key_buff;
  }

  memcpy(key_buff, ref->key_buff, ALIGN_SIZE(key_length));
  memcpy(key_buff2, ref->key_buff2, ALIGN_SIZE(key_length));
  uchar *key_buff_tmp = key_buff;

  for (uint i = 0; i < key_parts; i++) {
    items[i] = ref->items[i]->pq_clone(thd, join->query_block);
    if (items[i] == nullptr) {
      return true;
    }
    assert(DBUG_EVALUATE_IF("skip_pq_clone_check", true, false) || items[i]);
    if (!items[i]->fixed) {
      items[i]->fix_fields(thd, &items[i]);
    }

    if (qep_tab->table()->key_info) {
      KEY *const keyinfo = qep_tab->table()->key_info + key;
      bool maybe_null = keyinfo->key_part[i].null_bit;
      qep_tab->position()->key->val = items[i];
      qep_tab->position()->key->used_tables = qep_tab->position()->key->val->used_tables();
      if (ref->key_copy[i] != nullptr) {
        key_copy[i] = get_store_key(thd, qep_tab->position()->key->val, qep_tab->position()->key->used_tables,
                                    join->const_table_map, &keyinfo->key_part[i], key_buff_tmp, maybe_null);
      }

      key_buff_tmp += keyinfo->key_part[i].store_length;
    }

    if (!key_copy[i]) {
      key_copy[i] = ref->key_copy[i];
    }
    cond_guards[i] = ref->cond_guards[i];
  }
  return false;
}
/*
 * get table index
 *
 * @retval:
 *    -1 means not found.
 */
int get_qep_tab_index(QEP_TAB *src, Table_ref *first_tbl) {
  int index = 0;
  for (Table_ref *tl = first_tbl; tl != nullptr; tl = tl->next_leaf) {
    if (src->table_ref == tl) {
      return index;
    }
    index++;
  }
  return -1;
}

Table_ref *get_next_table(Table_ref *start_table, table_list_type_enum list_type) {
  if (list_type == TABLE_LIST_TYPE_DEFAULT) {
    return start_table->next_local;
  } else if (list_type == TABLE_LIST_TYPE_LEAF) {
    return start_table->next_leaf;
  } else if (list_type == TABLE_LIST_TYPE_MERGE) {
    return start_table->merge_underlying_list;
  } else {
    return start_table->next_global;
  }
  return nullptr;
}

Table_ref *get_table_by_index(Table_ref *start_table, table_list_type_enum list_type, int index) {
  if (start_table == nullptr) {
    return nullptr;
  }
  if (list_type == TABLE_LIST_TYPE_MERGE) {
    start_table = start_table->merge_underlying_list;
  }
  int it = 0;
  for (Table_ref *tbl_list = start_table; tbl_list != nullptr; it++) {
    if (it == index) {
      return tbl_list;
    }
    tbl_list = get_next_table(tbl_list, list_type);
  }
  return nullptr;
}

int get_qep_tab_index(QEP_TAB *tab, JOIN *join) {
  for (uint i = 0; i < join->tables; i++) {
    if (&join->qep_tab0[i] == tab) {
      return i;
    }
  }
  return -1;
}

bool copy_flush(QEP_TAB *des, JOIN *orig, int index, JOIN *join) {
  QEP_TAB *src = &orig->qep_tab[index];
  SJ_TMP_TABLE_TAB sjtabs[MAX_TABLES];
  SJ_TMP_TABLE_TAB *last_tab = sjtabs;
  if (src->flush_weedout_table->tabs != nullptr) {
    for (SJ_TMP_TABLE_TAB *t = src->flush_weedout_table->tabs; t < src->flush_weedout_table->tabs_end; t++) {
      int n = get_qep_tab_index(t->qep_tab, orig);
      if (n == -1) {
        return false;
      }
      last_tab->qep_tab = &join->qep_tab[n];
      ++last_tab;
    }
  }

  SJ_TMP_TABLE *sjtbl = create_sj_tmp_table(join->thd, join, sjtabs, last_tab);
  des->flush_weedout_table = sjtbl;
  QEP_TAB *start = &orig->qep_tab[index];
  int dis = 0;
  for (uint i = index + 1; i < orig->tables; i++) {
    QEP_TAB *t = &orig->qep_tab[i];
    if (t->check_weed_out_table == start->flush_weedout_table) {
      dis = i - index;
      break;
    }
  }

  QEP_TAB *last_sj_tab = des + dis;
  last_sj_tab->check_weed_out_table = sjtbl;
  return true;
}
/**
 * duplicate qep_tabs in JOIN
 *
 * @join : target JOIN
 * @orig : origin JOIN
 * @setup : setup qep_tab object
 *
 */
bool pq_dup_tabs(JOIN *join, JOIN *orig, bool setup MY_ATTRIBUTE((unused))) {
  Item *m_having_cond = nullptr;

  join->const_tables = orig->const_tables;
  join->primary_tables = orig->primary_tables;
  Query_block *select = join->query_block;

  // phase 1. Create qep_tab and qep_tab->qs;
  QEP_shared *qs = new (join->thd->pq_mem_root) QEP_shared[join->tables + 1];
  if (qs == nullptr) {
    goto err;
  }
  join->qep_tab0 = new (join->thd->pq_mem_root) QEP_TAB[join->tables + 1];
  if (join->qep_tab0 == nullptr) {
    goto err;
  }
  join->qep_tab = join->qep_tab0;

  for (uint i = 0; i < join->tables; i++) {
    join->qep_tab[i].set_qs(&qs[i]);
    join->qep_tab[i].set_join(join);
    join->qep_tab[i].set_idx(i);
    join->qep_tab[i].match_tab = orig->qep_tab[i].match_tab;
    join->qep_tab[i].flush_weedout_table = orig->qep_tab[i].flush_weedout_table;
    join->qep_tab[i].check_weed_out_table = orig->qep_tab[i].check_weed_out_table;
    join->qep_tab[i].op_type = orig->qep_tab[i].op_type;
    join->qep_tab[i].table_ref = orig->qep_tab[i].table_ref;
    join->qep_tab[i].using_dynamic_range = orig->qep_tab[i].using_dynamic_range;
  }

  for (uint i = 0; i < join->primary_tables; i++) {
    QEP_TAB *tab = &join->qep_tab[i];
    QEP_TAB *orig_tab = &orig->qep_tab[i];

    // phase 3. Set tables to qep_tab according to db/table name
    if (tab->pq_copy(join->thd, orig_tab)) {
      goto err;
    }
    TABLE *tb = orig_tab->table();
    tab->table_name = new (join->thd->pq_mem_root) LEX_CSTRING{tb->s->table_name.str, tb->s->table_name.length};

    tab->db = new (join->thd->pq_mem_root) LEX_CSTRING{tb->s->db.str, tb->s->db.length};
    if (tab->table_name == nullptr || tab->db == nullptr) {
      goto err;
    }

    /*
     * note: currently, setup is true.
     * Because duplicate qep_tabs in JOIN need fix_field to convert item to
     * field.
     */
    assert(select->leaf_tables);
    /*
     * setup physic table object
     * Sometimes there are multiple tables with the same name in the
     * leaf_tables, such as select empnum from t1 where hours in (select hours
     * from t1); The leaf_tables has two t1's in it,at this point we need to
     * copy the corresponding table of the same name.
     */
    int index = get_qep_tab_index(orig_tab, orig->query_block->leaf_tables);
    if (index == -1) {
      goto err;
    }
    Table_ref *tl = get_table_by_index(select->leaf_tables, TABLE_LIST_TYPE_LEAF, index);
    if (tl == nullptr) {
      goto err;
    }
    bitmap_copy(tl->table->read_set, tab->table_ref->table->read_set);
    bitmap_copy(tl->table->write_set, tab->table_ref->table->write_set);
    tab->set_table(tl->table);
    tab->table_ref = tl;

    // phase 4. Copy table properties from leader
    if (tab->ref().pq_copy(join, &orig_tab->ref(), tab)) {
      goto err;
    }

    if (orig_tab->table()) {
      if (tab->table()->pq_copy(join->thd, (void *)select, orig_tab->table())) {
        goto err;
      }
      tab->set_keyread_optim();
    }

    // phase 2. clone conditions in qep_tab
    Item *condition = orig_tab->condition();
    if ((condition != nullptr) && i < orig->primary_tables) {
      Item *cond = condition->pq_clone(join->thd, select);
      assert(DBUG_EVALUATE_IF("skip_pq_clone_check", true, false) || cond);
      if (cond == nullptr) {
        goto err;
      }
      if (cond->fix_fields(join->thd, &cond)) {
        goto err;
      }
      tab->set_condition(cond);
      tab->set_condition_optim();
    }

    /**
        // phase 2. clone cache_idx_cond in qep_tab
        Item *cache_idx_cond = orig_tab->cache_idx_cond;
        if ((cache_idx_cond != nullptr) && i < orig->primary_tables) {
          Item *cond = cache_idx_cond->pq_clone(join->thd, select);
          assert(DBUG_EVALUATE_IF("skip_pq_clone_check", true, false) || cond);
          if (cond == nullptr) {
            goto err;
          }
          if (cond->fix_fields(join->thd, &cond)) {
            goto err;
          }
          tab->cache_idx_cond = cond;
        }
    */

    // phase 5. setup pq condition for index push down
    if ((tab->has_pq_cond && !tab->pq_cond) || (tab->pq_cond && tab->pq_cond->fix_fields(join->thd, &tab->pq_cond)) ||
        DBUG_EVALUATE_IF("pq_clone_error2", true, false)) {
      sql_print_warning("[Parallel query]: ICP condition pushdown failed");
      goto err;
    }

    // phase 6. copy quick select
    /*
        MEM_ROOT *saved_mem_root = join->thd->mem_root;
        if (orig_tab->quick()) {
          QUICK_SELECT_I *quick =
              orig_tab->quick()->pq_clone(join->thd, tab->table());
          assert(DBUG_EVALUATE_IF("pq_clone_error1", true, false) || quick);
          if (quick == nullptr) {
            goto err;
          }
          tab->set_quick(quick);
          tab->set_quick_optim();
        }
        join->thd->mem_root = saved_mem_root;
        */

    // phase 6. copy quick select

    MEM_ROOT *saved_mem_root = join->thd->mem_root;
    if (orig_tab->range_scan()) {
      AccessPath *range = pq_range_clone_from(join->thd, orig_tab->range_scan(), tab->table());
      assert(DBUG_EVALUATE_IF("pq_clone_error1", true, false) || range);
      if (range == nullptr) {
        goto err;
      }

      tab->set_range_scan(range);
      // tab->set_quick_optim();
    }
    join->thd->mem_root = saved_mem_root;
  }

  // phase 7. Copy having condition
  m_having_cond = select->having_cond();
  if (m_having_cond) {
    assert(m_having_cond->is_bool_func());
    join->thd->where = "having clause";
    select->having_fix_field = true;
    select->resolve_place = Query_block::RESOLVE_HAVING;
    if (!m_having_cond->fixed &&
        (m_having_cond->fix_fields(join->thd, &m_having_cond) || m_having_cond->check_cols(1))) {
      goto err;
    }

    select->having_fix_field = false;
    select->resolve_place = Query_block::RESOLVE_NONE;
  }

  for (uint i = 0; i < join->tables; i++) {
    QEP_TAB *t = &orig->qep_tab[i];
    if (t->flush_weedout_table != nullptr) {
      if (!copy_flush(&join->qep_tab[i], orig, i, join)) {
        goto err;
      }
    }
  }
  return false;

err:
  return true;
}

/*
 * clone order structure
 */
ORDER *pq_dup_order(THD *thd, Query_block *select, ORDER *orig) {
  ORDER *order = new (thd->pq_mem_root) ORDER();
  if (order == nullptr) {
    return nullptr;
  }

  if (!orig->in_field_list) {
    order->item_initial = (*orig->item)->pq_clone(thd, select);
  } else {
    order->item_initial = orig->item_initial->pq_clone(thd, select);
  }

  assert(DBUG_EVALUATE_IF("skip_pq_clone_check", true, false) || order->item_initial);
  if (order->item_initial == nullptr) {
    return nullptr;
  }

  order->next = nullptr;
  order->item = &order->item_initial;
  order->direction = orig->direction;
  order->in_field_list = orig->in_field_list;
  order->used_alias = orig->used_alias;
  order->field_in_tmp_table = nullptr;
  order->buff = nullptr;
  order->used = 0;
  order->depend_map = 0;
  order->is_explicit = orig->is_explicit;

  return order;
}

int get_table_index(Table_ref *start_table, table_list_type_enum list_type, Table_ref *tl) {
  if (start_table == nullptr) {
    return -1;
  }
  int index = 0;
  for (Table_ref *tbl_list = start_table; tbl_list != nullptr; index++) {
    if (tbl_list == tl) {
      return index;
    }
    tbl_list = get_next_table(tbl_list, list_type);
  }
  return -1;
}

Table_ref *copy_table(THD *thd, Table_ref *src, Query_block *select, Query_block *orig) {
  Table_ref *ptr = new (thd->mem_root) Table_ref;
  if (ptr == nullptr) {
    return nullptr;
  }
  ptr->query_block = select;
  ptr->derived = src->derived;
  ptr->effective_algorithm = src->effective_algorithm;
  ptr->outer_join = src->outer_join;
  if (src->merge_underlying_list != nullptr) {
    Table_ref *foundtable = nullptr;
    int index = get_table_index(orig->leaf_tables, TABLE_LIST_TYPE_GLOBAL, src->merge_underlying_list);
    if (index != -1) {
      foundtable = get_table_by_index(select->leaf_tables, TABLE_LIST_TYPE_GLOBAL, index);
      if (foundtable == nullptr) {
        return nullptr;
      }
      ptr->merge_underlying_list = foundtable;
    } else {
      ptr->merge_underlying_list = copy_table(thd, src->merge_underlying_list, select, orig);
    }
  }
  ptr->field_translation = nullptr;
  ptr->table_name = src->table_name;
  ptr->table_name_length = src->table_name_length;
  ptr->alias = src->alias;
  ptr->is_alias = src->is_alias;
  ptr->table_function = src->table_function;
  if (src->table_function) {
    ptr->derived_key_list.clear();
  }
  ptr->is_fqtn = src->is_fqtn;
  ptr->db = src->db;
  ptr->db_length = src->db_length;
  ptr->set_tableno(src->tableno());
  ptr->set_lock({TL_UNLOCK, THR_DEFAULT});
  ptr->updating = false;
  ptr->ignore_leaves = false;
  ptr->is_system_view = src->is_system_view;

  if (!ptr->is_derived() && !ptr->is_table_function() && is_infoschema_db(ptr->db, ptr->db_length)) {
    dd::info_schema::convert_table_name_case(const_cast<char *>(ptr->db), const_cast<char *>(ptr->table_name));
    ST_SCHEMA_TABLE *schema_table = nullptr;
    if (!ptr->is_system_view) {
      schema_table = find_schema_table(thd, ptr->table_name);
      if (schema_table) {
        ptr->schema_table = schema_table;
      }
    }
  }
  ptr->cacheable_table = true;
  ptr->index_hints = nullptr;
  ptr->option = nullptr;
  ptr->next_name_resolution_table = nullptr;
  ptr->partition_names = nullptr;
  MDL_REQUEST_INIT(&ptr->mdl_request, MDL_key::TABLE, ptr->db, ptr->table_name, MDL_SHARED_READ, MDL_TRANSACTION);
  return ptr;
}

bool copy_table_field(Table_ref *src, Table_ref *des, THD *thd, Query_block *dest_select) {
  int count = src->field_translation_end - src->field_translation;
  if (count <= 0) {
    return false;
  }
  if (des->field_translation_end - des->field_translation != count) {
    return true;
  }
  if (des->field_translation[0].item != nullptr) {
    return false;
  }
  for (int i = 0; i < count; i++) {
    des->field_translation[i].name = src->field_translation[i].name;
    if (src->field_translation[i].item == nullptr) {
      return true;
    }
    des->field_translation[i].item = src->field_translation[i].item->pq_clone(thd, dest_select);
    if (des->field_translation[i].item == nullptr) {
      return true;
    }
  }
  return false;
}

bool copy_merge_table_field(THD *thd, Query_block *dest_select, int tableindex, int mergeindex, Table_ref *srctb) {
  Table_ref *tb = get_table_by_index(dest_select->get_table_list(), TABLE_LIST_TYPE_DEFAULT, tableindex);
  if (tb == nullptr) {
    return true;
  }
  Table_ref *mergetable = get_table_by_index(tb, TABLE_LIST_TYPE_MERGE, mergeindex);
  if (mergetable == nullptr) {
    return true;
  }
  if (copy_table_field(srctb, mergetable, thd, dest_select)) {
    return true;
  }
  return false;
}

bool copy_global_table_list_field(THD *thd, Query_block *orig, Query_block *dest_select) {
  int tableindex = 0;
  for (Table_ref *tbl_list = orig->leaf_tables; tbl_list != nullptr; tbl_list = tbl_list->next_global) {
    if (tbl_list->field_translation != nullptr) {
      Table_ref *src = get_table_by_index(dest_select->leaf_tables, TABLE_LIST_TYPE_GLOBAL, tableindex);
      if (src == nullptr) {
        return true;
      }
      if (copy_table_field(tbl_list, src, thd, dest_select)) {
        return true;
      }
    }
    tableindex++;
  }
  return false;
}

bool init_table_field_space(THD *thd, Table_ref *src, Table_ref *des) {
  int count = src->field_translation_end - src->field_translation;
  if (count > 0 && des->field_translation == nullptr) {
    Field_translator *transl = (Field_translator *)thd->stmt_arena->alloc(count * sizeof(Field_translator));
    if (transl == nullptr) {
      return true;
    }
    for (int i = 0; i < count; i++) {
      transl[i].name = nullptr;
      transl[i].item = nullptr;
    }
    des->field_translation = transl;
    des->field_translation_end = transl + count;
  }
  return false;
}

bool copy_leaf_tables(THD *thd, Query_block *orig, Query_block *dest_select) {
  Table_ref *last = nullptr;
  dest_select->leaf_tables = nullptr;
  for (Table_ref *tbl_list = orig->leaf_tables; tbl_list != nullptr; tbl_list = tbl_list->next_leaf) {
    Table_ref *tl = copy_table(thd, tbl_list, dest_select, orig);
    if (tl == nullptr) {
      return true;
    }
    if (dest_select->leaf_tables == nullptr) {
      dest_select->leaf_tables = tl;
      last = tl;
    } else {
      last->next_name_resolution_table = tl;
      last->next_leaf = tl;
      last = tl;
    }
  }
  last->next_leaf = nullptr;
  return false;
}

void set_up_leaf_tables(THD *thd, Query_block *select) {
  select->partitioned_table_count = 0;
  for (Table_ref *tr = select->leaf_tables; tr != nullptr; tr = tr->next_leaf) {
    TABLE *const table = tr->table;
    select->leaf_table_count++;
    if (select->first_execution && select->opt_hints_qb &&  // QB hints initialized
        !tr->opt_hints_table)                               // Table hints are not adjusted yet
    {
      tr->opt_hints_table = select->opt_hints_qb->adjust_table_hints(tr);
    }
    if (table == nullptr) {
      continue;
    }
    table->pos_in_table_list = tr;
  }
  if (select->opt_hints_qb) {
    select->opt_hints_qb->check_unresolved(thd);
  }
}

bool copy_global_tables(THD *thd, Query_block *orig, Query_block *dest_select) {
  for (Table_ref *tbl_list = orig->leaf_tables; tbl_list != nullptr; tbl_list = tbl_list->next_global) {
    int index = get_table_index(orig->leaf_tables, TABLE_LIST_TYPE_LEAF, tbl_list);
    Table_ref *tmp = nullptr;
    if (index != -1) {
      tmp = get_table_by_index(dest_select->leaf_tables, TABLE_LIST_TYPE_LEAF, index);
    } else {
      tmp = copy_table(thd, tbl_list, dest_select, orig);
    }
    if (tmp == nullptr) {
      return true;
    }
    thd->lex->add_to_query_tables(tmp);
  }
  return false;
}

bool copy_table_list(THD *thd, Query_block *orig, Query_block *dest_select) {
  for (Table_ref *tbl_list = orig->get_table_list(); tbl_list != nullptr; tbl_list = tbl_list->next_local) {
    int index = get_table_index(orig->leaf_tables, TABLE_LIST_TYPE_GLOBAL, tbl_list);
    Table_ref *tmp = nullptr;
    if (index != -1) {
      tmp = get_table_by_index(dest_select->leaf_tables, TABLE_LIST_TYPE_GLOBAL, index);
    } else {
      tmp = copy_table(thd, tbl_list, dest_select, orig);
    }
    if (tmp == nullptr) {
      return true;
    }
    dest_select->m_table_list.link_in_list(tmp, &tmp->next_local);
  }
  return false;
}

bool init_table_list_field_space(THD *thd, Query_block *select, table_list_type_enum list_type) {
  Table_ref *start_src = nullptr;
  Table_ref *start_des = nullptr;
  if (list_type == TABLE_LIST_TYPE_DEFAULT) {
    start_src = select->orig->get_table_list();
    start_des = select->get_table_list();
  } else {
    start_src = select->orig->leaf_tables;
    start_des = select->leaf_tables;
  }
  int tableindex = 0;
  for (Table_ref *tbl_list = start_src; tbl_list != nullptr; tableindex++) {
    if (tbl_list->field_translation != nullptr) {
      Table_ref *des = get_table_by_index(start_des, list_type, tableindex);
      if (des == nullptr) {
        return true;
      }
      if (init_table_field_space(thd, tbl_list, des)) {
        return true;
      }
    }
    tbl_list = get_next_table(tbl_list, list_type);
  }
  return false;
}
bool init_field_space(THD *thd, Query_block *orig, Query_block *select) {
  if (init_table_list_field_space(thd, select, TABLE_LIST_TYPE_DEFAULT) ||
      init_table_list_field_space(thd, select, TABLE_LIST_TYPE_GLOBAL)) {
    return true;
  }

  int tableindex = 0;
  for (Table_ref *tbl_list = orig->get_table_list(); tbl_list != nullptr; tbl_list = tbl_list->next_local) {
    if (tbl_list->merge_underlying_list != nullptr) {
      int mergeindex = 0;
      for (Table_ref *tb = tbl_list->merge_underlying_list; tb != nullptr; tb = tb->merge_underlying_list) {
        if (tb->field_translation != nullptr) {
          Table_ref *ta = get_table_by_index(select->get_table_list(), TABLE_LIST_TYPE_DEFAULT, tableindex);
          if (ta == nullptr) {
            return true;
          }
          Table_ref *mergetable = get_table_by_index(ta, TABLE_LIST_TYPE_MERGE, mergeindex);
          if (mergetable == nullptr) {
            return true;
          }
          if (init_table_field_space(thd, tb, mergetable)) {
            return true;
          }
        }
        mergeindex++;
      }
    }
    tableindex++;
  }
  return false;
}

bool copy_merge_table_list_field(THD *thd, Query_block *orig, Query_block *dest_select) {
  int tableindex = 0;
  int mergeindex = 0;
  for (Table_ref *tbl_list = orig->get_table_list(); tbl_list != nullptr; tbl_list = tbl_list->next_local) {
    if (tbl_list->merge_underlying_list != nullptr) {
      mergeindex = 0;
      for (Table_ref *tb = tbl_list->merge_underlying_list; tb != nullptr; tb = tb->merge_underlying_list) {
        if (tb->field_translation != nullptr && copy_merge_table_field(thd, dest_select, tableindex, mergeindex, tb)) {
          return true;
        }
        mergeindex++;
      }
    }
    tableindex++;
  }
  return false;
}

bool copy_table_list_field(THD *thd, Query_block *orig, Query_block *dest_select) {
  int tableindex = 0;
  for (Table_ref *tbl_list = orig->get_table_list(); tbl_list != nullptr; tbl_list = tbl_list->next_local) {
    if (tbl_list->field_translation != nullptr) {
      Table_ref *src = get_table_by_index(dest_select->get_table_list(), TABLE_LIST_TYPE_DEFAULT, tableindex);
      if (src == nullptr) {
        return true;
      }
      if (copy_table_field(tbl_list, src, thd, dest_select)) {
        return true;
      }
    }
    tableindex++;
  }
  return false;
}

bool copy_all_table_list(THD *thd, Query_block *orig, Query_block *dest_select) {
  if (copy_leaf_tables(thd, orig, dest_select) || copy_global_tables(thd, orig, dest_select) ||
      copy_table_list(thd, orig, dest_select)) {
    return true;
  }
  if (init_field_space(thd, orig, dest_select) || copy_merge_table_list_field(thd, orig, dest_select) ||
      copy_global_table_list_field(thd, orig, dest_select) || copy_table_list_field(thd, orig, dest_select)) {
    return true;
  }
  return false;
}

Query_block *pq_dup_select(THD *thd, Query_block *orig) {
  Item *new_item = nullptr;
  Item *where = nullptr;
  Item *having = nullptr;
  ORDER *group = nullptr;
  ORDER *group_new = nullptr;
  ORDER *order = nullptr;
  ORDER *order_new = nullptr;
  Query_block *select = nullptr;
  SQL_I_List<ORDER> orig_list;

  LEX *lex = new (thd->pq_mem_root) LEX();
  if (lex == nullptr) {
    goto err;
  }
  lex->reset();
  lex->result = orig->parent_lex->result;
  lex->sql_command = orig->parent_lex->sql_command;
  lex->explain_format = orig->parent_lex->explain_format;
  lex->is_explain_analyze = orig->parent_lex->is_explain_analyze;
  thd->lex = lex;
  lex->thd = thd;
  thd->query_plan.set_query_plan(SQLCOM_SELECT, lex, false);

  select = lex->new_query(nullptr);
  if (!select || DBUG_EVALUATE_IF("dup_select_abort1", true, false)) {
    goto err;
  }
  select->orig = orig;
  select->renumber(thd->lex);
  select->with_sum_func = orig->with_sum_func;
  select->n_child_sum_items = orig->n_child_sum_items;
  select->n_sum_items = orig->n_sum_items;
  select->select_n_having_items = orig->select_n_having_items;
  select->select_n_where_fields = orig->select_n_where_fields;
  select->m_active_options = orig->m_active_options;
  lex->set_current_query_block(select);
  lex->unit = select->master_query_expression();
  thd->lex->query_block = select;

  // phase 1. clone tables and open/lock them
  if (copy_all_table_list(thd, orig, select)) {
    goto err;
  }

  assert(select->context.query_block == select);
  select->context.table_list = select->context.first_name_resolution_table = select->leaf_tables;

  // phase 1. open tables and lock them
  if (open_tables_for_query(thd, thd->lex->query_tables, 0) ||
      lock_tables(thd, thd->lex->query_tables, thd->lex->table_count, 0)) {
    goto err;
  }
  set_up_leaf_tables(thd, select);
  // phase 1. copy table->nullable
  // before setup_fields, propagate_nullability will change table->nullable,
  // which may affect item->maybe_null, so we copy it here.
  // see in Query_block:: prepare
  for (Table_ref *tl = orig->leaf_tables; tl != nullptr; tl = tl->next_leaf) {
    for (Table_ref *tbl_list = select->leaf_tables; tbl_list != nullptr; tbl_list = tbl_list->next_leaf) {
      const char *db = tbl_list->db;
      const char *table_name = tbl_list->table_name;
      const char *alias = tbl_list->alias;

      if (!strncmp(db, tl->db, strlen(db)) && strlen(tl->db) == strlen(db) &&
          !strncmp(table_name, tl->table_name, strlen(table_name)) && strlen(tl->table_name) == strlen(table_name) &&
          !strncmp(alias, tl->alias, strlen(alias)) && strlen(tl->alias) == strlen(alias) &&
          tl->m_tableno == tbl_list->m_tableno) {
        if (tl->table != nullptr && tl->table->is_nullable()) {
          tbl_list->table->set_nullable();
        }
        break;
      }
    }
  }

  // phase 2. clone select fields list
  for (Item *item : orig->fields) {
    if (item->hidden) {
      continue;
    }
    new_item = item->pq_clone(thd, select);
    // assert(DBUG_EVALUATE_IF("skip_pq_clone_check", true, false) || new_item);
    if (new_item == nullptr) {
      goto err;
    }

    select->fields.push_back(new_item);
  }

  // phase 3. duplicate group list
  /*
   * for template select_lex, we use leader's saved_group_list_ptrs to
   * restore original group_list, and then copy it to template. For
   * worker's select_lex, we directly use template's info to generate
   * its group_list.
   */
  if (orig->saved_group_list_ptrs) {
    restore_list(orig->saved_group_list_ptrs, orig_list);
    assert(orig_list.elements == orig->group_list.elements);
  } else {  // the case of template select_lex
    orig_list = orig->group_list;
  }

  // duplicate group list
  if (orig_list.elements) {
    for (group = orig_list.first; group; group = group->next) {
      group_new = pq_dup_order(thd, select, group);
      if (group_new == nullptr) {
        goto err;
      }

      select->group_list.link_in_list(group_new, &group_new->next);
    }
  }

  if (orig->saved_order_list_ptrs) {
    restore_list(orig->saved_order_list_ptrs, orig_list);
    assert(orig_list.elements == orig->order_list.elements);
  } else {  // the case of template select_lex
    orig_list = orig->order_list;
  }

  // duplicate order list
  if (orig_list.elements) {
    for (order = orig_list.first; order; order = order->next) {
      order_new = pq_dup_order(thd, select, order);
      if (order_new == nullptr) {
        goto err;
      }

      select->order_list.link_in_list(order_new, &order_new->next);
    }
  }

  /** mianly used for optimized_group_by */
  if (select->group_list.elements) {
    select->fix_prepare_information_for_order(thd, &select->group_list, &select->saved_group_list_ptrs);
  }
  if (select->order_list.elements) {
    select->fix_prepare_information_for_order(thd, &select->order_list, &select->saved_order_list_ptrs);
  }

  if (select->setup_base_ref_items(thd) || DBUG_EVALUATE_IF("dup_select_abort2", true, false)) {
    goto err;
  }

  thd->mark_used_columns = MARK_COLUMNS_READ;

  // phase 5. duplicate where cond
  if (orig->where_cond()) {
    where = orig->where_cond()->pq_clone(thd, select);
    assert(DBUG_EVALUATE_IF("skip_pq_clone_check", true, false) || where);
    if (where == nullptr) {
      goto err;
    }
    select->set_where_cond(where);
  } else {
    select->set_where_cond(nullptr);
  }

  // phase 6. duplicate having cond
  if (orig->having_cond()) {
    having = orig->having_cond()->pq_clone(thd, select);
    assert(DBUG_EVALUATE_IF("skip_pq_clone_check", true, false) || having);
    if (having == nullptr) {
      goto err;
    }
    select->set_having_cond(having);
  } else {
    select->set_having_cond(nullptr);
  }

  // phase 7: allow local set functions in HAVING and ORDER BY
  lex->allow_sum_func |= (nesting_map)1 << (nesting_map)select->nest_level;
  select->set_query_result(lex->result);
  return select;
err:
  return nullptr;
}

/**resolve query block, setup tables list, fields list, group list\order list
 * @select: query block **/
static bool pq_select_prepare(THD *thd, Query_block *select, mem_root_deque<Item *> &orig_all_fields) {
  // Setup.1 setup all fields
  int all_fields_count = select->fields.size();
  thd->mark_used_columns = MARK_COLUMNS_READ;
  ulong want_privilege = 0;
  if (setup_fields(thd, want_privilege, true, true, false, nullptr, &select->fields, select->base_ref_items)) {
    return true;
  }

  // Setup.2 setup GROUP BY clause
  if (select->group_list.elements && select->setup_group(thd)) {
    return true;
  }
  select->hidden_group_field_count = select->fields.size() - all_fields_count;

  // Setup.3 setup ORDER BY clause
  if (select->order_list.elements &&
      setup_order(thd, select->base_ref_items, select->get_table_list(), &select->fields, select->order_list.first)) {
    return true;
  }

  select->hidden_order_field_count = select->fields.size() - all_fields_count;

  if (select->order_list.elements && select->setup_order_final(thd)) {
    return true;
  }

  // Setup.4: check item's property */
  if (select->fields.size() != orig_all_fields.size()) {
    return true;
  }

  Item *orig_item = nullptr;
  uint i = 0;
  for (Item *item : select->fields) {
    orig_item = orig_all_fields[i];
    if (item == nullptr || (item->type() != orig_item->type())) return true;
    i++;
  }

  return false;
}

JOIN *pq_make_join(THD *thd, JOIN *join) {
  JOIN *pq_join = nullptr;
  Query_block *select = pq_dup_select(thd, join->query_block);
  if (!select || pq_select_prepare(thd, select, join->query_block->fields)) {
    goto err;
  }

  thd->lex->unit->set_prepared();

  pq_join = new (thd->pq_mem_root) JOIN(thd, select);
  if (!pq_join || DBUG_EVALUATE_IF("dup_join_abort", true, false)) {
    goto err;
  }
  pq_join->pq_copy_from(join);
  /**
   * limit cannot push down to worker, for the cases:
   *   (1) with aggregation
   *   (2) with sorting after optimized-group-by
   */
  if (join->query_expression()->select_limit_cnt) {
    if (join->query_block->with_sum_func ||  // c1
        (join->pq_rebuilt_group &&           // c2
         join->pq_last_sort_idx >= (int)join->primary_tables)) {
      pq_join->m_select_limit = HA_POS_ERROR;  // no limit
      pq_join->query_expression()->select_limit_cnt = HA_POS_ERROR;
    }
  }
  return pq_join;

err:
  return nullptr;
}

bool System_variables::pq_copy_from(struct System_variables orig) {
  pseudo_thread_id = orig.pseudo_thread_id;
  sql_mode = orig.sql_mode;
  collation_connection = orig.collation_connection;
  div_precincrement = orig.div_precincrement;
  time_zone = orig.time_zone;
  big_tables = orig.big_tables;
  lc_time_names = orig.lc_time_names;
  my_aes_mode = orig.my_aes_mode;
  transaction_isolation = orig.transaction_isolation;
  option_bits = orig.option_bits;
  explicit_defaults_for_timestamp = orig.explicit_defaults_for_timestamp;
  sortbuff_size = orig.sortbuff_size;
  join_buff_size = orig.join_buff_size;
  return false;
}

bool System_status_var::pq_merge_status(struct System_status_var worker) {
  filesort_range_count += worker.filesort_range_count;
  filesort_rows += worker.filesort_rows;
  filesort_scan_count += worker.filesort_scan_count;

  ha_read_first_count += worker.ha_read_first_count;
  ha_read_last_count += worker.ha_read_last_count;
  ha_read_key_count += worker.ha_read_key_count;
  ha_read_next_count += worker.ha_read_next_count;
  ha_read_prev_count += worker.ha_read_prev_count;
  ha_read_rnd_count += worker.ha_read_rnd_count;
  ha_read_rnd_next_count += worker.ha_read_rnd_next_count;
  return false;
}

bool THD::pq_copy_from(THD *thd) {
  variables.pq_copy_from(thd->variables);
  start_time = thd->start_time;
  user_time = thd->user_time;
  m_query_string = thd->m_query_string;
  tx_isolation = thd->tx_isolation;
  tx_read_only = thd->tx_read_only;
  parallel_exec = thd->parallel_exec;
  pq_dop = thd->pq_dop;
  arg_of_last_insert_id_function = thd->arg_of_last_insert_id_function;
  first_successful_insert_id_in_prev_stmt = thd->first_successful_insert_id_in_prev_stmt;
  first_successful_insert_id_in_prev_stmt_for_binlog = thd->first_successful_insert_id_in_prev_stmt_for_binlog;
  first_successful_insert_id_in_cur_stmt = thd->first_successful_insert_id_in_cur_stmt;
  stmt_depends_on_first_successful_insert_id_in_prev_stmt =
      thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt;
  return false;
}

bool THD::pq_merge_status(THD *thd) {
  status_var.pq_merge_status(thd->status_var);
  current_found_rows += thd->current_found_rows;
  pq_current_found_rows = thd->current_found_rows;
  m_examined_row_count += thd->m_examined_row_count;
  return false;
}

bool THD::pq_status_reset() {
  current_found_rows = 0;
  m_examined_row_count = 0;
  return false;
}