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

#ifndef MYSQLROUTER_RAPID_SERVER_REST_CLIENT_INCLUDED
#define MYSQLROUTER_RAPID_SERVER_REST_CLIENT_INCLUDED

#include <chrono>
#include <string>

/** @brief URI for the rapid server globals */
static constexpr const char kRapidServerGlobalsRestUri[] =
    "/api/v1/rapid_server/globals/";

/** @class RapidServerHttpClient
 *
 * Allows communicating with mysql server rapid via
 * HTTP port
 *
 **/
class RapidServerRestClient {
 public:
  /** @brief Constructor
   *
   * @param http_port       port on which http server handles the http requests
   * @param http_hostname   hostname of the http server that handles the http
   * requests
   */
  RapidServerRestClient(const uint16_t http_port,
                       const std::string &http_hostname = "127.0.0.1");

  /** @brief Sets values of the all globals in the server rapid via
   *         http interface.
   *         Example:
   *             set_globals("{\"secondary_removed\": true}");
   *
   * @param globals_json    json string with the globals names and values to set
   */
  void set_globals(const std::string &globals_json);

  /** @brief Gets all the rapid server globals as a json string
   */
  std::string get_globals_as_json_string();

  /** @brief Gets a selected rapid server int global value
   *
   * @param global_name    name of the global
   */
  int get_int_global(const std::string &global_name);

  /** @brief Gets a selected rapid server bool global value
   *
   * @param global_name    name of the global
   */
  bool get_bool_global(const std::string &global_name);

  /** @brief Sends Delete request to the rapid server
   *         on the selected URI
   *
   * @param uri uri for the Delete request
   */
  void send_delete(const std::string &uri);

  /**
   * @brief Wait until a REST endpoint returns !404.
   *
   * at rapid startup the socket starts to listen before the REST endpoint gets
   * registered. As long as it returns 404 Not Found we should wait and retry.
   *
   * @param max_wait_time max time to wait for endpoint being ready
   * @returns true once endpoint doesn't return 404 anymore, fails otherwise
   */
  bool wait_for_rest_endpoint_ready(
      std::chrono::milliseconds max_wait_time =
          kRapidServerDefaultRestEndpointTimeout) const noexcept;

 private:
  static constexpr std::chrono::milliseconds kRapidServerMaxRestEndpointStepTime{
      100};
  static constexpr std::chrono::milliseconds
      kRapidServerDefaultRestEndpointTimeout{1000};
  const std::string http_hostname_;
  const uint16_t http_port_;
};

#endif  // MYSQLROUTER_RAPID_SERVER_REST_CLIENT_INCLUDED
