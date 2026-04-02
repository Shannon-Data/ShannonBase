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

   Copyright (c) 2023 -, Shannon Data AI and/or its affiliates.
*/

#include "ml/infra_component/llm_model_detector.h"

namespace ShannonBase {
namespace ML {
std::string CPUDetector::getCPUInfo() const {
  std::string info = "CPU Features: ";
  if (m_features.arm64) info += "ARM64 ";
  if (m_features.avx2) info += "AVX2 ";
  if (m_features.avx512f) info += "AVX512F ";
  if (m_features.avx512vnni) info += "AVX512-VNNI ";
  if (m_features.avx512bw) info += "AVX512BW ";
  if (m_features.avx512dq) info += "AVX512DQ ";
  if (m_features.fma) info += "FMA ";
  if (m_features.fp16) info += "FP16 ";
  return (info == "CPU Features: ") ? "CPU Features: None detected" : info;
}

void CPUDetector::cpuid(std::array<int, 4> &regs, int funcId, int subFuncId) const noexcept {
#if defined(_MSC_VER)
  __cpuidex(regs.data(), funcId, subFuncId);
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
  unsigned int eax, ebx, ecx, edx;
  __cpuid_count(funcId, subFuncId, eax, ebx, ecx, edx);
  regs[0] = static_cast<int>(eax);
  regs[1] = static_cast<int>(ebx);
  regs[2] = static_cast<int>(ecx);
  regs[3] = static_cast<int>(edx);
#else
  regs.fill(0);
#endif
}

void CPUDetector::detectFeatures() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
  m_features.arm64 = true;
#if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC)
  m_features.fp16 = true;
#endif
  return;
#endif

  std::array<int, 4> regs{};

  cpuid(regs, 0, 0);
  const int maxLeaf = regs[0];
  if (maxLeaf < 1) return;

  cpuid(regs, 1, 0);
  const int ecx1 = regs[2];

  const bool avx = (ecx1 & (1 << 28)) != 0;
  m_features.fma = (ecx1 & (1 << 12)) != 0;

  if (maxLeaf >= 7) {
    cpuid(regs, 7, 0);
    const int ebx7 = regs[1];
    const int ecx7 = regs[2];

    m_features.avx2 = avx && ((ebx7 & (1 << 5)) != 0);
    m_features.avx512f = ((ebx7 & (1 << 16)) != 0);
    m_features.avx512bw = ((ebx7 & (1 << 30)) != 0);
    m_features.avx512dq = ((ebx7 & (1 << 17)) != 0);
    m_features.avx512vnni = ((ecx7 & (1 << 11)) != 0);
  }
}

ModelSelection select_model_variant(const std::string &model_dir, const std::string &user_precision,
                                    const std::string & /*user_variant*/) {
  namespace fs = std::filesystem;

  ModelSelection result;
  result.device = Device::CPU;
  result.precision = Precision::FP32;
  result.variant = "base";

  // Returns true when the ONNX file exists and is either self-contained
  // (< 2 GiB) or accompanied by an external data shard (*_data).
  auto exists = [&](const std::string &fname) -> bool {
    const fs::path p = fs::path(model_dir) / fname;
    return fs::exists(p) && (fs::exists(fs::path(model_dir) / (fname + "_data")) || fs::file_size(p) < 2147483648ULL);
  };

  // User-specified precision takes absolute priority
  if (!user_precision.empty()) {
    struct PrecisionEntry {
      const char *key;
      const char *file;
      Precision prec;
      double mem_gb;
    };
    static constexpr PrecisionEntry k_table[] = {
        {"fp32", "model.onnx", Precision::FP32, 12.9},
        {"fp16", "model_fp16.onnx", Precision::FP16, 6.43},
        {"int8", "model_int8.onnx", Precision::INT8, 3.21},
        {"q4", "model_q4.onnx", Precision::QINT4, 3.34},
    };
    for (const auto &e : k_table) {
      if (user_precision == e.key && exists(e.file)) {
        result.filename = e.file;
        result.precision = e.prec;
        result.variant = e.key;
        result.estimated_memory_gb = e.mem_gb;
        return result;
      }
    }
    // Requested precision not available → fall through to auto-selection.
  }

  // Auto-select based on CPU ISA capabilities
  CPUDetector cpu;

  if (cpu.isARM64()) {
    if (cpu.hasFP16() && exists("model_fp16.onnx")) {
      result = {"model_fp16.onnx", Precision::FP16, Device::CPU, "fp16_arm", 6.43};
    } else if (exists("model_int8.onnx")) {
      result = {"model_int8.onnx", Precision::INT8, Device::CPU, "int8_arm", 3.21};
    } else if (exists("model_q4.onnx")) {
      result = {"model_q4.onnx", Precision::QINT4, Device::CPU, "q4_arm", 3.34};
    }
  } else if (cpu.hasAVX512VNNI() && exists("model_int8.onnx")) {
    result = {"model_int8.onnx", Precision::INT8, Device::CPU, "int8_avx512_vnni", 3.21};
  } else if (cpu.hasAVX512F() && exists("model_fp16.onnx")) {
    result = {"model_fp16.onnx", Precision::FP16, Device::CPU, "fp16_avx512", 6.43};
  } else if (cpu.hasAVX2() && exists("model_int8.onnx")) {
    result = {"model_int8.onnx", Precision::INT8, Device::CPU, "int8_avx2", 3.21};
  }

  // Generic fallback when no ISA-specific file was resolved
  if (result.filename.empty()) {
    if (exists("model_fp16.onnx")) {
      result = {"model_fp16.onnx", Precision::FP16, Device::CPU, "fp16_fallback", 6.43};
    } else if (exists("model_int8.onnx")) {
      result = {"model_int8.onnx", Precision::INT8, Device::CPU, "int8_fallback", 3.21};
    } else if (exists("model.onnx")) {
      result = {"model.onnx", Precision::FP32, Device::CPU, "fp32_fallback", 12.9};
    }
    // filename remains empty → caller must handle "no model found" condition.
  }
  return result;
}
}  // namespace ML
}  // namespace ShannonBase