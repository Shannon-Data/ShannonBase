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

#ifndef __SHANNONBASE_CPU_MODEL_SELECTOR_H__
#define __SHANNONBASE_CPU_MODEL_SELECTOR_H__
#include <array>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <intrin.h>
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif

namespace ShannonBase {
namespace ML {
enum class Precision { FP32, FP16, INT8, QINT4, UNKNOWN };
enum class Device { CPU, GPU };

struct ModelSelection {
  std::string filename;        ///< Relative ONNX filename inside model_dir
  Precision precision;         ///< Resolved numeric precision
  Device device;               ///< Target execution device
  std::string variant;         ///< Human-readable tag, e.g. "int8_avx2"
  double estimated_memory_gb;  ///< Approximate RAM footprint
};
class CPUDetector {
 public:
  /// Convenience tag used by select_model_variant() to map ISA → model type.
  enum class ModelType {
    ARM64_QINT8,
    AVX512_VNNI_QINT8,
    AVX512F_QINT8,
    AVX2_QUINT8,
    BASELINE_FP32,
    FP16_OPTIMIZED,
    QINT4_OPTIMIZED,
    QINT4_FP16_HYBRID
  };

  CPUDetector() noexcept { detectFeatures(); }

  bool hasAVX2() const noexcept { return m_features.avx2; }
  bool hasAVX512F() const noexcept { return m_features.avx512f; }
  bool hasAVX512VNNI() const noexcept { return m_features.avx512vnni; }
  bool hasAVX512BW() const noexcept { return m_features.avx512bw; }
  bool hasAVX512DQ() const noexcept { return m_features.avx512dq; }
  bool isARM64() const noexcept { return m_features.arm64; }
  bool hasFMA() const noexcept { return m_features.fma; }
  bool hasFP16() const noexcept { return m_features.fp16; }

  std::string getCPUInfo() const;

 private:
  struct CPUFeatures {
    bool avx2 = false;
    bool avx512f = false;
    bool avx512vnni = false;
    bool avx512bw = false;
    bool avx512dq = false;
    bool arm64 = false;
    bool fma = false;
    bool fp16 = false;
  } m_features;

  void cpuid(std::array<int, 4> &regs, int funcId, int subFuncId = 0) const noexcept;
  void detectFeatures() noexcept;
};

/**
 *  select_model_variant
 *  Inspects the files present in @p model_dir and the host CPU's ISA support
 *  to choose the best ONNX model variant automatically.
 *  @param model_dir       Directory that contains the ONNX model files.
 *  @param user_precision  Optional override: "fp32", "fp16", "int8", "q4".
 *                         When non-empty and the requested file exists, it is
 *                         selected unconditionally.
 *  @param user_variant    Reserved for future use (currently ignored).
 *  @return                Populated ModelSelection; filename is empty when no
 *                         suitable file was found.
 */
ModelSelection select_model_variant(const std::string &model_dir, const std::string &user_precision = "",
                                    const std::string &user_variant = "");
}  // namespace ML
}  // namespace ShannonBase
#endif  // __SHANNONBASE_CPU_MODEL_SELECTOR_H__