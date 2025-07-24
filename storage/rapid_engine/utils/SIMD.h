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

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_UTILS_SIMD_H__
#define __SHANNONBASE_UTILS_SIMD_H__

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace ShannonBase {
namespace Utils {
namespace SIMD {
// ============================================================================
// SIMD Capability Detection
// ============================================================================
#ifdef __x86_64__
[[maybe_unused]] static bool hasSSE2() { return __builtin_cpu_supports("sse2"); }
[[maybe_unused]] static bool hasSSE41() { return __builtin_cpu_supports("sse4.1"); }
[[maybe_unused]] static bool hasAVX() { return __builtin_cpu_supports("avx"); }
[[maybe_unused]] static bool hasAVX2() { return __builtin_cpu_supports("avx2"); }
[[maybe_unused]] static bool hasFMA() { return __builtin_cpu_supports("fma"); }
[[maybe_unused]] static bool hasAVX512F() { return __builtin_cpu_supports("avx512f"); }

[[maybe_unused]] static void initialize() { __builtin_cpu_init(); }
#elif defined(__aarch64__) || defined(__arm__)
// ARM implementations
#include <sys/auxv.h>

[[maybe_unused]] static bool hasNEON() {
#if defined(__aarch64__)
  return true;  // NEON is mandatory on AArch64
#else
  return (getauxval(AT_HWCAP) & HWCAP_NEON) != 0;
#endif
}

[[maybe_unused]] static bool hasCRC() { return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0; }

// ARM versions of the functions - return false or implement ARM equivalents
[[maybe_unused]] static bool hasSSE2() { return false; }
[[maybe_unused]] static bool hasSSE41() { return false; }
[[maybe_unused]] static bool hasAVX() { return false; }
[[maybe_unused]] static bool hasAVX2() { return false; }
[[maybe_unused]] static bool hasFMA() { return (getauxval(AT_HWCAP) & HWCAP_FPHP) != 0; }
[[maybe_unused]] static bool hasAVX512F() { return false; }

[[maybe_unused]] static void initialize() {
  // No initialization needed on ARM or can parse /proc/cpuinfo
}
#else
// Unknown architecture
[[maybe_unused]] static bool hasSSE2() { return false; }
[[maybe_unused]] static bool hasSSE41() { return false; }
[[maybe_unused]] static bool hasAVX() { return false; }
[[maybe_unused]] static bool hasAVX2() { return false; }
[[maybe_unused]] static bool hasFMA() { return false; }
[[maybe_unused]] static bool hasAVX512F() { return false; }

[[maybe_unused]] static void initialize() {}
#endif
}  // namespace SIMD
}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_UTILS_SIMD_H__