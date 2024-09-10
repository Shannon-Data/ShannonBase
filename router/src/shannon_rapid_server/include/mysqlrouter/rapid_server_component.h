/*
  Copyright (c) 2018, 2023, Oracle and/or its affiliates.

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
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

  Shannon Data AI.
*/

#ifndef MYSQLROUTER_RAPID_SERVER_COMPONENT_INCLUDED
#define MYSQLROUTER_RAPID_SERVER_COMPONENT_INCLUDED

#include <memory>
#include <vector>

#include "mysql/harness/stdx/monitor.h"
#include "mysqlrouter/rapid_server_export.h"
#include "mysqlrouter/rapid_server_global_scope.h"

namespace server_rapid {
class MySQLServerRapid;
}

class RAPID_SERVER_EXPORT RapidServerComponent {
 public:
  // disable copy, as we are a single-instance
  RapidServerComponent(RapidServerComponent const &) = delete;
  void operator=(RapidServerComponent const &) = delete;

  static RapidServerComponent &get_instance();

  void register_server(const std::string &name,
                       std::shared_ptr<server_rapid::MySQLServerRapid> srv);

  std::shared_ptr<RapidServerGlobalScope> get_global_scope();
  void close_all_connections();

 private:
  Monitor<std::map<std::string, std::weak_ptr<server_rapid::MySQLServerRapid>>>
      srvs_{{}};

  RapidServerComponent() = default;
};

#endif
