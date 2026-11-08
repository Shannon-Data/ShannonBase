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

   The fundmental code for imcs rapid.
*/
#ifndef __SHANNONBASE_RAPID_TELEMETRY_H__
#define __SHANNONBASE_RAPID_TELEMETRY_H__

namespace ShannonBase {
namespace RapidMonitor {
/**
 * Registers the "rapid" meter and its metrics, once, on first use.
 *
 * Registration cannot happen in the plugin's init callback: core storage
 * engines are initialised from plugin_register_builtin_and_init_core_se(),
 * which runs before initialize_performance_schema(), so the psi_metric_v1
 * implementation still holds uninitialised locks at that point. The first
 * statement that runs through the Rapid handler is the earliest safe moment,
 * and that is where this is called from.
 *
 * The metrics are asynchronous counters: performance_schema does not copy any
 * value out of the engine, it calls back into the engine whenever an exporter
 * scrapes, so the cost on the hot paths stays the atomic add they already do.
 *
 * Failing is not fatal. A server started with performance_schema=OFF has no
 * observability surface for the engine, which is a supported setup, so the
 * failure is logged once and the engine keeps running.
 */
void ensure_rapid_metrics();

/**
 * Unregisters the meter registered by ensure_rapid_metrics().
 *
 * Must run before the engine's counters go away, because performance_schema
 * keeps pointers into them for as long as the meter is registered. A no-op when
 * registration never happened.
 */
void unregister_rapid_metrics();

}  // namespace RapidMonitor
}  // namespace ShannonBase

#endif  // __SHANNONBASE_RAPID_TELEMETRY_H__
