# Copyright (c) 2019, 2025, Oracle and/or its affiliates.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is designed to work with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have either included with
# the program or referenced in the documentation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# run_genagentjsfile.cmake
#
# usage:
#   cmake -D JS_SRC_DIR=<dir>    -D ENTRY=<entry.js>
#         -D TEMPLATE=<file>     -D OUTPUT=<file>
#         -P run_genagentjsfile.cmake
#
# Behavior Specification:
#   1. Recursively expand all //@include xxx.js directives starting from ENTRY file
#   2. Deduplicate includes - each file is expanded only once to prevent circular inclusion loops
#   3. Replace the __AGENT_BODY__ placeholder in TEMPLATE with the fully expanded JavaScript code
#   4. Run a SECOND expansion pass over the resulting merged text (template + injected body).
#      This is required because //@include directives may also appear directly inside the
#      TEMPLATE file itself, outside of the __AGENT_BODY__ placeholder (for example, a separate
#      JS function body embedded elsewhere in the same SQL template). Step 3 alone only expands
#      includes that originate from ENTRY; it does not see or process include directives that
#      live in the template text. This second pass reuses the same dedup state (_seen_files) so
#      a file already expanded in pass 1 is not expanded again.
#   5. Write the final result to OUTPUT
#
# Implementation Note: Do NOT split processing by line boundaries. CMake lists use ';' as 
# delimiters, and JavaScript source code contains numerous semicolons throughout - splitting 
# by lines would cause truncation. Instead, perform regex matching and targeted replacement 
# on the entire text block as a whole.

cmake_minimum_required(VERSION 3.10)

if(NOT JS_SRC_DIR OR NOT ENTRY OR NOT TEMPLATE OR NOT OUTPUT)
  message(FATAL_ERROR
    "Usage: cmake -D JS_SRC_DIR=.. -D ENTRY=.. -D TEMPLATE=.. -D OUTPUT=.. -P run_genagentjsfile.cmake")
endif()

set(_seen_files "")

function(expand_one_file rel_path out_var)
  get_filename_component(_abs_path "${JS_SRC_DIR}/${rel_path}" ABSOLUTE)

  list(FIND _seen_files "${_abs_path}" _idx)
  if(NOT _idx EQUAL -1)
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()
  set(_seen_files "${_seen_files}" "${_abs_path}")
  set(_seen_files "${_seen_files}" PARENT_SCOPE)

  if(NOT EXISTS "${_abs_path}")
    message(FATAL_ERROR "Include not found: ${_abs_path} (referenced as '${rel_path}')")
  endif()

  file(READ "${_abs_path}" _content)
  string(REGEX REPLACE "\r\n" "\n" _content "${_content}")

  # Extract all complete lines that match the pattern //@include xxx.js
  string(REGEX MATCHALL "//@include[ \t]+[^ \t\r\n]+" _inc_lines "${_content}")

  list(LENGTH _inc_lines _n_inc)
  if(_n_inc GREATER 0)
    list(REMOVE_DUPLICATES _inc_lines)
    foreach(_inc_line IN LISTS _inc_lines)
      string(REGEX REPLACE "//@include[ \t]+([^ \t\r\n]+)" "\\1" _inc_name "${_inc_line}")
      expand_one_file("${_inc_name}" _inc_expanded)
      # It is unnecessary to escape backslashes or special characters that may appear in 
      # _inc_expanded for use with string(REPLACE), because REPLACE performs literal 
      # substitution, not regex substitution — there is no escaping issue.
      string(REPLACE "${_inc_line}" "${_inc_expanded}" _content "${_content}")
    endforeach()
  endif()

  set(${out_var} "${_content}" PARENT_SCOPE)
  set(_seen_files "${_seen_files}" PARENT_SCOPE)
endfunction()

# Expand a free-standing block of text (not tied to a specific source file path) for
# //@include directives. Used for the second pass over the merged template+body text,
# where matches no longer correspond 1:1 to a single file on disk.
function(expand_text_block text_var out_var)
  set(_content "${${text_var}}")

  string(REGEX MATCHALL "//@include[ \t]+[^ \t\r\n]+" _inc_lines "${_content}")
  list(LENGTH _inc_lines _n_inc)
  if(_n_inc GREATER 0)
    list(REMOVE_DUPLICATES _inc_lines)
    foreach(_inc_line IN LISTS _inc_lines)
      string(REGEX REPLACE "//@include[ \t]+([^ \t\r\n]+)" "\\1" _inc_name "${_inc_line}")
      expand_one_file("${_inc_name}" _inc_expanded)
      string(REPLACE "${_inc_line}" "${_inc_expanded}" _content "${_content}")
    endforeach()
  endif()

  set(${out_var} "${_content}" PARENT_SCOPE)
  set(_seen_files "${_seen_files}" PARENT_SCOPE)
endfunction()

# Pass 1: expand ENTRY and inject into the __AGENT_BODY__ placeholder.
expand_one_file("${ENTRY}" _agent_body)

if(NOT EXISTS "${TEMPLATE}")
  message(FATAL_ERROR "Template not found: ${TEMPLATE}")
endif()
file(READ "${TEMPLATE}" _template_content)

string(REPLACE "__AGENT_BODY__" "${_agent_body}" _merged_content "${_template_content}")

# Pass 2: scan the merged text again for any //@include directives that live directly in
# the template itself (outside __AGENT_BODY__), e.g. a second JS function body defined
# elsewhere in the same SQL file. _seen_files carries over from pass 1 so already-expanded
# files are not duplicated.
expand_text_block(_merged_content _final_content)

get_filename_component(_out_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")
file(WRITE "${OUTPUT}" "${_final_content}")

message(STATUS "Generated: ${OUTPUT}")
