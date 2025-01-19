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

   Copyright (c) 2023,2024 Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. To get cache line size.
*/

#ifndef __SHANNON_RAPID_ARCH_INFO_H__
#define __SHANNON_RAPID_ARCH_INFO_H__
// cache line size
#define CACHE_LINE_SIZE 64

#define CACHE_L1_SIZE
#define CACHE_L2_SIZE
#define CACHE_L3_SIZE

// clang-format off
/* prefetch
 * can be disabled, by declaring NO_PREFETCH build macro */
#if defined(SHANNON_NO_PREFETCH)
#  define SHANNON_PREFETCH_L1(addr)  (void)(addr)  /* disabled */
#  define SHANNON_PREFETCH_L2(addr)  (void)(addr)  /* disabled */
#  define SHANNON_PREFETCH_L3(addr)  (void)(addr)  /* disabled */
#  define SHANNON_PREFETCH_R(addr)  (void)(addr)  /* disabled */
#  define SHANNON__PREFETCH_RW(addr)  (void)(addr)  /* disabled */
#else
#  if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_I86))  /* _mm_prefetch() is not defined outside of x86/x64 */
#    include <mmintrin.h>   /* https://msdn.microsoft.com/fr-fr/library/84szxsww(v=vs.90).aspx */
#    define SHANNON_PREFETCH_L1(addr)  _mm_prefetch((const char*)(addr), _MM_HINT_T0)
#    define SHANNON_PREFETCH_L2(addr)  _mm_prefetch((const char*)(addr), _MM_HINT_T1)
#    define SHANNON_PREFETCH_L3(addr)  _mm_prefetch((const char*)(addr), _MM_HINT_T2)
#    define SHANNON_PREFETCH_R(addr)   _mm_prefetch((const char*)(addr), _MM_HINT_T2)
#    define SHANNON__PREFETCH_RW(addr)   _mm_prefetch((const char*)(addr), _MM_HINT_T2)
#  elif defined(__GNUC__) && ( (__GNUC__ >= 4) || ( (__GNUC__ == 3) && (__GNUC_MINOR__ >= 1) ) )
#    define SHANNON_PREFETCH_L1(addr)  __builtin_prefetch((addr), 0 /* rw==read */, 3 /* locality */)
#    define SHANNON_PREFETCH_L2(addr)  __builtin_prefetch((addr), 0 /* rw==read */, 2 /* locality */)
#    define SHANNON_PREFETCH_L3(addr)  __builtin_prefetch((addr), 0 /* rw==read */, 1 /* locality */)
#    define SHANNON_PREFETCH_R(addr)   __builtin_prefetch((addr), 0 /* rw==read */, 1 /* locality */)
/*   Minimize cache-miss latency by moving data at addr into a cache before it is read or written. */
#    define SHANNON__PREFETCH_RW(addr) __builtin_prefetch((addr), 1 /* rw==write */, 1 /* locality */)
#  elif defined(__aarch64__)
#    define SHANNON_PREFETCH_L1(addr)  __asm__ __volatile__("prfm pldl1keep, %0" ::"Q"(*(addr)))
#    define SHANNON_PREFETCH_L2(addr)  __asm__ __volatile__("prfm pldl2keep, %0" ::"Q"(*(addr)))
#    define SHANNON_PREFETCH_L3(addr)  __asm__ __volatile__("prfm pldl3keep, %0" ::"Q"(*(addr)))
#    define SHANNON_PREFETCH_R(addr)   __asm__ __volatile__("prfm pldl3keep, %0" ::"Q"(*(addr)))
#    define SHANNON__PREFETCH_RW(addr) __asm__ __volatile__("prfm pldl3strm, %0" ::"Q"(*(addr)))
#  else
#    define SHANNON_PREFETCH_L1(addr) (void)(addr)  /* disabled */
#    define SHANNON_PREFETCH_L2(addr) (void)(addr)  /* disabled */
#    define SHANNON_PREFETCH_L3(addr) (void)(addr)  /* disabled */
#    define SHANNON_PREFETCH_R(addr)   (void)(addr)  /* disabled */
#    define SHANNON__PREFETCH_RW(addr) (void)(addr)  /* disabled */
#  endif
#endif  /* NO_PREFETCH */
// clang-format on

#endif  //__SHANNON_RAPID_ARCH_INFO_H__
