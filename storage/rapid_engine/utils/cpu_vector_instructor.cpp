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
   Detect the highest x86 SIMD level supported by the current CPU+OS.
   Output format:  SHANNON_VECT_LEVEL=<0|1|2|3>
     0 = scalar only
     1 = SSE4.2
     2 = AVX / AVX2
     3 = AVX-512F + AVX-512BW
*/
#ifndef __SHANNONBASE_CPU_VECTOR_INSTRUCTOR_H__
#define __SHANNONBASE_CPU_VECTOR_INSTRUCTOR_H__
#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <cstdio>

#if defined(_MSC_VER)
#include <intrin.h>
static void do_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]) {
  __cpuidex(reinterpret_cast<int *>(out), (int)leaf, (int)subleaf);
}
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
static void do_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]) {
  __cpuid_count(leaf, subleaf, out[0], out[1], out[2], out[3]);
}
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_MSC_VER)
static sigjmp_buf s_jmp;
static volatile bool s_xcr0_ok = false;
static volatile uint64_t s_xcr0_val = 0;

static void sigill_handler(int) { siglongjmp(s_jmp, 1); }

static uint64_t safe_read_xcr0() {
  struct sigaction sa_new {
  }, sa_old{};
  sa_new.sa_handler = sigill_handler;
  sigemptyset(&sa_new.sa_mask);
  sa_new.sa_flags = 0;
  sigaction(SIGILL, &sa_new, &sa_old);

  uint64_t val = 0;
  if (sigsetjmp(s_jmp, 1) == 0) {
#if defined(_MSC_VER)
    val = _xgetbv(0);
#else
    uint32_t lo, hi;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    val = ((uint64_t)hi << 32) | lo;
#endif
    s_xcr0_ok = true;
  }
  sigaction(SIGILL, &sa_old, nullptr);
  return val;
}

int main() {
  uint32_t regs[4] = {};
  // leaf 1: SSE4.2 / AVX / OSXSAVE
  do_cpuid(1, 0, regs);
  const uint32_t ecx1 = regs[2];
  const bool sse42 = (ecx1 >> 20) & 1u;
  const bool avx1 = (ecx1 >> 28) & 1u;
  const bool osxsave = (ecx1 >> 27) & 1u;
  // leaf 7 sub 0: AVX2 / AVX-512F / AVX-512BW
  do_cpuid(7, 0, regs);
  const uint32_t ebx7 = regs[1];
  const bool avx2 = (ebx7 >> 5) & 1u;
  const bool avx512f = (ebx7 >> 16) & 1u;
  const bool avx512bw = (ebx7 >> 30) & 1u;

  bool ymm_ok = false, zmm_ok = false;
  if (osxsave) {
    uint64_t xcr0 = safe_read_xcr0();
    if (s_xcr0_ok) {
      ymm_ok = (xcr0 & 0x06u) == 0x06u;
      zmm_ok = (xcr0 & 0xe6u) == 0xe6u;
    } else {
      ymm_ok = (avx1 || avx2);
      zmm_ok = false;
    }
  }

  int level = 0;
  if (sse42) level = 1;
  if ((avx1 || avx2) && ymm_ok) level = 2;
  if (avx512f && avx512bw && zmm_ok) level = 3;

  printf("SHANNON_VECT_LEVEL=%d\n", level);
  return 0;
}
#else
// Non-x86: ARM/Apple dealing in rapid_arch_inf.h.in.
int main() {
  printf("SHANNON_VECT_LEVEL=0\n");
  return 0;
}
#endif
#endif  //__SHANNONBASE_CPU_VECTOR_INSTRUCTOR_H__