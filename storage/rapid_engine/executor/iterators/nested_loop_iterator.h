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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#ifndef __SHANNONBASE_NESTED_LOOP_ITERATOR_H__
#define __SHANNONBASE_NESTED_LOOP_ITERATOR_H__

#include "sql/iterators/composite_iterators.h"
#include "storage/rapid_engine/executor/iterators/record_iterator.h"

class AccessPath;
namespace ShannonBase {
namespace Executor {
bool SelectVectorizedNestedLoop(THD *thd, AccessPath *path, bool batch_consumer);

// REF probes retain MySQL's scalar state machine. This operator owns the
// columnar output and the record image needed to resume that state machine
// after a batch consumer has overwritten shared TABLE buffers.
class VectorizedNestedLoopIterator final : public VectorizedRecordIterator {
 public:
  static bool CanVectorize(AccessPath *path);
  VectorizedNestedLoopIterator(THD *thd, unique_ptr_destroy_only<RowIterator> outer,
                               unique_ptr_destroy_only<RowIterator> inner, JoinType type, bool pfs_batch_mode,
                               const Prealloced_array<TABLE *, 4> &tables, size_t budget, double estimate);
};
}  // namespace Executor
}  // namespace ShannonBase
#endif
