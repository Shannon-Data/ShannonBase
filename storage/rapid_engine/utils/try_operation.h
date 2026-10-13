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

   Copyright (c) 2026, Shannon Data AI and/or its affiliates.
*/
#ifndef SHANNONBASE_TRY_OPERATION_H
#define SHANNONBASE_TRY_OPERATION_H

#include <type_traits>
#include <utility>

namespace ShannonBase::Utils {
// Adapt throwing STL/provider APIs to a completion status. The operation must
// return void; semantic failure belongs in its own output value. Failure does
// not roll back side effects: callers must preserve ownership until completion
// and explicitly fence partially applied work.
template <typename Operation>
bool try_operation(Operation &&operation) noexcept {
  static_assert(std::is_void_v<std::invoke_result_t<Operation>>);
  try {
    std::forward<Operation>(operation)();
    return true;
  } catch (...) {
    return false;
  }
}

// Preserve an operation's semantic failure as well as allocation/provider
// failure. Use at the lowest API boundary that owns the throwing operation.
template <typename Operation>
bool try_bool_operation(Operation &&operation) noexcept {
  static_assert(std::is_same_v<std::invoke_result_t<Operation>, bool>);
  bool succeeded = false;
  return try_operation([&] { succeeded = std::forward<Operation>(operation)(); }) && succeeded;
}
}  // namespace ShannonBase::Utils

#endif
