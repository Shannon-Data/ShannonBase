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

#include "mysqlrouter/rapid_server_component.h"

#include <memory>  // shared_ptr

#include "mysql_server_rapid.h"
#include "mysqlrouter/rapid_server_global_scope.h"

std::shared_ptr<RapidServerGlobalScope> RapidServerComponent::get_global_scope() {
  static std::shared_ptr<RapidServerGlobalScope> instance{
      std::make_shared<RapidServerGlobalScope>()};

  return instance;
}

void RapidServerComponent::register_server(
    const std::string &name,
    std::shared_ptr<server_rapid::MySQLServerRapid> srv) {
  srvs_([&](auto srvs) { srvs.emplace(name, srv); });
}

RapidServerComponent &RapidServerComponent::get_instance() {
  static RapidServerComponent instance;

  return instance;
}

void RapidServerComponent::close_all_connections() {
  srvs_([&](auto srvs) {
    for (auto &srv : srvs) {
      // if we have a rapid_server instance, call its close_all_connections()
      if (auto server = srv.second.lock()) {
        server->close_all_connections();
      }
    }
  });
}
