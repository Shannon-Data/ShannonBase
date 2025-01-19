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

   The fundmental code for imcs optimizer.
*/
#ifndef __SHANNONBASE_COST_H__
#define __SHANNONBASE_COST_H__
#include "storage/rapid_engine/include/rapid_object.h"

#include "sql/handler.h"  //Cost_estimator
#include "sql/opt_costconstants.h"
#include "sql/opt_costmodel.h"

namespace ShannonBase {
namespace Optimizer {

class Rapid_SE_cost_constants : public SE_cost_constants {
 public:
  Rapid_SE_cost_constants() : SE_cost_constants(::Optimizer::kHypergraph) {}
  virtual ~Rapid_SE_cost_constants() = default;

  cost_constant_error rapid_update_func(const LEX_CSTRING &name, const double value) { return update(name, value); }

  cost_constant_error rapid_update_default_func(const LEX_CSTRING &name, const double value) {
    return update_default(name, value);
  }

  /// Default cost for reading a random block from an in-memory buffer
  static const double MEMORY_BLOCK_READ_COST;

  /// Default cost for reading a random disk block
  static const double IO_BLOCK_READ_COST;
};

class Rapid_Cost_estimate : public Cost_estimate {};

class Rapid_Cost_model_server : public Cost_model_server {
 public:
  Rapid_Cost_model_server() {
    // Create default values for server cost constants
    m_server_cost_constants = new Server_cost_constants(::Optimizer::kHypergraph);
#if !defined(NDEBUG)
    m_initialized = true;
#endif
  }

  ~Rapid_Cost_model_server() override {
    delete m_server_cost_constants;
    m_server_cost_constants = nullptr;
  }
};

class Rapid_Cost_model_table : public Cost_model_table {
 public:
  Rapid_Cost_model_table() {
    // Create a rapid cost model server object that will provide
    // cost constants for server operations
    m_cost_model_server = new Rapid_Cost_model_server();

    // Allocate cost constants for operations on tables
    m_se_cost_constants = new Rapid_SE_cost_constants();

#if !defined(NDEBUG)
    m_initialized = true;
#endif
  }

  ~Rapid_Cost_model_table() {
    delete m_cost_model_server;
    m_cost_model_server = nullptr;
    delete m_se_cost_constants;
    m_se_cost_constants = nullptr;
  }
};

}  // namespace Optimizer
}  // namespace ShannonBase

#endif  //__SHANNONBASE_COST_H__