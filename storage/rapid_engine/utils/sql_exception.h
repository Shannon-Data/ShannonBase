/* Copyright (c) 2026, Shannon Data AI and/or its affiliates.
   SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHANNONBASE_SQL_EXCEPTION_H
#define SHANNONBASE_SQL_EXCEPTION_H

#include <exception>
#include <new>

#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/sql_class.h"

namespace ShannonBase::Utils {
// Call only from a catch handler at a SQL execution boundary. Keep an already
// reported SQL error, and never let a C++ exception cross MySQL's error API.
inline bool ReportCurrentSqlException(THD *thd) noexcept {
  if (thd != nullptr && thd->is_error()) return true;
  try {
    throw;
  } catch (const std::bad_alloc &) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
  } catch (const std::exception &error) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), error.what());
  } catch (...) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Unexpected exception in Rapid execution");
  }
  return true;
}
}  // namespace ShannonBase::Utils
#endif
