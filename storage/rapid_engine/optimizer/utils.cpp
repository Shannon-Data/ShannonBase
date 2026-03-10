/**
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

   Copyright (c) 2023, 2026 Shannon Data AI and/or its affiliates.

   The fundmental code for imcs optimizer.
*/
#include "storage/rapid_engine/optimizer/utils.h"

#include "sql/join_optimizer/join_optimizer.h"
#include "sql/join_optimizer/make_join_hypergraph.h"
#include "sql/range_optimizer/range_optimizer.h"

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_config.h"

namespace ShannonBase {
namespace Optimizer {
namespace Utils {
RpdTableLookup rpd_lookup_func() {
  return [](TABLE *table) -> Imcs::RpdTable * {
    if (!table || !table->s) return nullptr;

    auto *share = ShannonBase::shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
    if (!share) return nullptr;

    return share->is_partitioned ? Imcs::Imcs::instance()->get_rpd_parttable(share->m_tableid)
                                 : Imcs::Imcs::instance()->get_rpd_table(share->m_tableid);
  };
}

table_map get_tablescovered(const AccessPath *path) {
  if (!path) return 0;

  switch (path->type) {
    case AccessPath::TABLE_SCAN:
      return path->table_scan().table->pos_in_table_list->map();
    case AccessPath::INDEX_SCAN:
      return path->index_scan().table->pos_in_table_list->map();
    case AccessPath::REF:
      return path->ref().table->pos_in_table_list->map();
    case AccessPath::EQ_REF:
      return path->eq_ref().table->pos_in_table_list->map();
    case AccessPath::INDEX_RANGE_SCAN:
      return path->index_range_scan().used_key_part[0].field->table->pos_in_table_list->map();
    case AccessPath::CONST_TABLE:
      return path->const_table().table->pos_in_table_list->map();
    case AccessPath::HASH_JOIN:
      return get_tablescovered(path->hash_join().outer) | get_tablescovered(path->hash_join().inner);
    case AccessPath::NESTED_LOOP_JOIN:
      return get_tablescovered(path->nested_loop_join().outer) | get_tablescovered(path->nested_loop_join().inner);
    case AccessPath::BKA_JOIN:
      return get_tablescovered(path->bka_join().outer) | get_tablescovered(path->bka_join().inner);
    case AccessPath::FILTER:
      return get_tablescovered(path->filter().child);
    case AccessPath::SORT:
      return get_tablescovered(path->sort().child);
    case AccessPath::AGGREGATE:
      return get_tablescovered(path->aggregate().child);
    case AccessPath::LIMIT_OFFSET:
      return get_tablescovered(path->limit_offset().child);
    case AccessPath::STREAM:
      return get_tablescovered(path->stream().child);
    case AccessPath::WINDOW:
      return get_tablescovered(path->window().child);
    case AccessPath::REMOVE_DUPLICATES:
      return get_tablescovered(path->remove_duplicates().child);
    case AccessPath::REMOVE_DUPLICATES_ON_INDEX:
      return get_tablescovered(path->remove_duplicates_on_index().child);
    case AccessPath::MATERIALIZE: {
      table_map result = 0;
      const auto &mat = path->materialize();
      if (mat.table_path) {
        result |= get_tablescovered(mat.table_path);
      }
      for (const auto &operand : mat.param->m_operands) {
        if (operand.subquery_path) {
          result |= get_tablescovered(operand.subquery_path);
        }
      }
      return result;
    } break;
    case AccessPath::APPEND: {
      table_map result = 0;
      for (const AppendPathParameters &child : *path->append().children) {
        result |= get_tablescovered(child.path);
      }
      return result;
    } break;
    case AccessPath::ZERO_ROWS:
    case AccessPath::ZERO_ROWS_AGGREGATED:
    case AccessPath::FAKE_SINGLE_ROW:
    case AccessPath::TABLE_VALUE_CONSTRUCTOR:
      return 0;
    case AccessPath::FOLLOW_TAIL:
      return path->follow_tail().table->pos_in_table_list->map();
    default:
      return 0;
  }
}

table_map get_tablescovered_from_hypergraph(const AccessPath *path, const JoinHypergraph &graph) {
  if (path->type == AccessPath::TABLE_SCAN) {
    TABLE *table = path->table_scan().table;
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
      if (graph.nodes[i].table() == table) {
        return table->pos_in_table_list->map();
      }
    }
  }
  return get_tablescovered(path);
}
}  // namespace Utils
}  // namespace Optimizer
}  // namespace ShannonBase