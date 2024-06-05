#ifndef __SHANNONBASE_SQL_PQ_CLONE_INCLUDE_H__
#define __SHANNONBASE_SQL_PQ_CLONE_INCLUDE_H__

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

#include "sql/sql_list.h"
class Item;
class Item_ident;
class THD;
class Query_block;
class JOIN;
struct ORDER;
class ORDER_with_src;
class Table_ref;

enum table_list_type_enum {
  TABLE_LIST_TYPE_DEFAULT,
  TABLE_LIST_TYPE_LEAF,
  TABLE_LIST_TYPE_GLOBAL,
  TABLE_LIST_TYPE_MERGE
};

bool pq_dup_tabs(JOIN *pq_join, JOIN *join, bool setup);

Table_ref *get_table_by_index(Table_ref *start_table, table_list_type_enum list_type, int index);

int get_table_index(Table_ref *start_table, table_list_type_enum list_type, Table_ref *tl);

extern Item **resolve_ref_in_select_and_group(THD *thd, Item_ident *ref, Query_block *select);

JOIN *pq_make_join(THD *thd, JOIN *join);

bool copy_all_table_list(THD *thd, Query_block *orig, Query_block *dest_select);

#endif  // __SHANNONBASE_SQL_PQ_CLONE_INCLUDE_H__
