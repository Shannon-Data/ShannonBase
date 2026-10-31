// Shared runtime ISA checks for the dispatch shim and server diagnostics.
#ifndef __SHANNON_ORT_CPU_TIERS_H__
#define __SHANNON_ORT_CPU_TIERS_H__

#include <string>
#include <vector>
#if defined(__x86_64__) || defined(__amd64__)
#include <cpuid.h>
#elif defined(__aarch64__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#ifndef HWCAP_ASIMDDP
#define HWCAP_ASIMDDP (1UL << 20)
#endif
#ifndef HWCAP2_I8MM
#define HWCAP2_I8MM (1UL << 13)
#endif
#ifndef HWCAP2_SVE2
#define HWCAP2_SVE2 (1UL << 1)
#endif
#endif

namespace ShannonOrt {
inline std::vector<std::string> cpu_tiers() {
  std::vector<std::string> tiers;
#if defined(__x86_64__) || defined(__amd64__)
  __builtin_cpu_init();
  unsigned int eax, ebx, ecx, edx;
  bool cx16 = __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & bit_CMPXCHG16B);
  bool lahf = __get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx) && (ecx & 1U);
  bool v2 = cx16 && lahf && __builtin_cpu_supports("sse3") &&
            __builtin_cpu_supports("ssse3") && __builtin_cpu_supports("sse4.1") &&
            __builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt");
  bool v3 = v2 && __builtin_cpu_supports("avx") && __builtin_cpu_supports("avx2") &&
            __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi") &&
            __builtin_cpu_supports("bmi2") && __builtin_cpu_supports("f16c") &&
            __builtin_cpu_supports("lzcnt") && __builtin_cpu_supports("movbe");
  bool v4 = v3 && __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
            __builtin_cpu_supports("avx512cd") && __builtin_cpu_supports("avx512dq") &&
            __builtin_cpu_supports("avx512vl");
  if (v4) tiers.push_back("x86-64-v4");
  if (v3) tiers.push_back("x86-64-v3");
  if (v2) tiers.push_back("x86-64-v2");
  tiers.push_back("x86-64-v1");
#elif defined(__aarch64__)
  unsigned long hw = getauxval(AT_HWCAP);
  unsigned long hw2 = getauxval(AT_HWCAP2);
  if (hw2 & HWCAP2_SVE2) tiers.push_back("v9");
  if (hw2 & HWCAP2_I8MM) tiers.push_back("v8.6");
  if (hw & HWCAP_ASIMDDP) tiers.push_back("v8.2");
  tiers.push_back("v8.0");
#endif
  return tiers;
}

}  // namespace ShannonOrt
#endif
