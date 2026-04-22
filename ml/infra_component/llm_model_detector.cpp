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

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

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
  m_features.fp16 = (ecx1 & (1 << 29)) != 0;
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

namespace {
struct PrecisionInfo {
  Precision prec;
  const char *tag;  // user‑facing tag: "fp16", "int8", "q4", etc.
  double mem_gb;    // estimated memory footprint (overridden by actual size)
  int priority;     // lower = more preferred
};

PrecisionInfo detect_precision(const std::string &stem) {
  std::string lower = stem;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

  struct Rule {
    const char *pat;
    PrecisionInfo info;
  };
  static const Rule rules[] = {
      {"fp16", {Precision::FP16, "fp16", 0.0, 10}},    {"float16", {Precision::FP16, "fp16", 0.0, 10}},
      {"int8", {Precision::INT8, "int8", 0.0, 20}},    {"quantized", {Precision::INT8, "int8", 0.0, 20}},
      {"uint8", {Precision::INT8, "int8", 0.0, 20}},   {"q4f16", {Precision::QINT4, "q4f16", 0.0, 25}},
      {"q4", {Precision::QINT4, "q4", 0.0, 30}},       {"int4", {Precision::QINT4, "q4", 0.0, 30}},
      {"bnb4", {Precision::QINT4, "q4", 0.0, 30}},     {"fp32", {Precision::FP32, "fp32", 0.0, 40}},
      {"float32", {Precision::FP32, "fp32", 0.0, 40}},
  };
  for (const auto &r : rules) {
    if (lower.find(r.pat) != std::string::npos) return r.info;
  }
  return {Precision::FP32, "fp32", 0.0, 100};  // default
}

bool is_companion(const std::string &stem) {
  static const std::vector<std::string> companions = {"embed_tokens", "lm_head", "shared"};
  for (const auto &c : companions) {
    if (stem == c) return true;
  }
  return false;
}

struct ScannedModel {
  std::string main_file;
  std::string stem;
  PrecisionInfo prec;
  double total_size_gb;
};

double get_external_data_size(const std::filesystem::path &dir, const std::string &stem) {
  namespace fs = std::filesystem;
  double total = 0.0;
  static const char *suffixes[] = {".onnx_data", ".onnx.data", ".data", "_data"};
  for (const char *suf : suffixes) {
    fs::path data_path = dir / (stem + suf);
    if (fs::exists(data_path) && fs::is_regular_file(data_path)) {
      total += static_cast<double>(fs::file_size(data_path)) / (1024.0 * 1024.0 * 1024.0);
      break;
    }
  }

  for (int i = 1;; ++i) {
    fs::path shard = dir / (stem + ".onnx_data_" + std::to_string(i));
    if (!fs::exists(shard)) break;
    total += static_cast<double>(fs::file_size(shard)) / (1024.0 * 1024.0 * 1024.0);
  }
  for (int i = 1;; ++i) {
    fs::path shard = dir / (stem + ".data_" + std::to_string(i));
    if (!fs::exists(shard)) break;
    total += static_cast<double>(fs::file_size(shard)) / (1024.0 * 1024.0 * 1024.0);
  }

  return total;
}

std::vector<ScannedModel> scan_models(const std::string &model_dir) {
  namespace fs = std::filesystem;

  if (!fs::exists(model_dir) || !fs::is_directory(model_dir)) return {};

  std::vector<ScannedModel> result;
  for (const auto &entry : fs::directory_iterator(model_dir)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name.size() < 5 || name.compare(name.size() - 5, 5, ".onnx") != 0) continue;
    const std::string stem = name.substr(0, name.size() - 5);
    if (is_companion(stem)) continue;

    ScannedModel m;
    m.main_file = name;
    m.stem = stem;
    m.prec = detect_precision(stem);

    double main_size_gb = static_cast<double>(fs::file_size(entry.path())) / (1024.0 * 1024.0 * 1024.0);
    double external_size_gb = get_external_data_size(fs::path(model_dir), stem);
    m.total_size_gb = main_size_gb + external_size_gb;
    if (m.total_size_gb > 0.0) {
      m.prec.mem_gb = m.total_size_gb;
    }

    result.push_back(std::move(m));
  }

  std::sort(result.begin(), result.end(),
            [](const ScannedModel &a, const ScannedModel &b) { return a.prec.priority < b.prec.priority; });

  return result;
}

}  // namespace

ModelSelection select_model_variant(const std::string &model_dir, const std::string &user_precision,
                                    const std::string & /*user_variant*/) {
  ModelSelection result;
  result.device = Device::CPU;
  result.precision = Precision::FP32;
  result.variant = "none";
  result.estimated_memory_gb = 0.0;

  const auto models = scan_models(model_dir);
  if (models.empty()) return result;

  auto fill = [&](const ScannedModel &m, const char *suffix) {
    result.filename = m.main_file;
    result.precision = m.prec.prec;
    result.variant = std::string(m.prec.tag) + suffix;
    result.estimated_memory_gb = m.prec.mem_gb;
  };

  if (!user_precision.empty()) {
    for (const auto &m : models) {
      if (m.prec.tag == user_precision) {
        fill(m, "_user");
        return result;
      }
    }
  }

  CPUDetector cpu;
  std::vector<const char *> prio;
  if (cpu.isARM64()) {
    if (cpu.hasFP16())
      prio = {"fp16", "int8", "q4f16", "q4", "fp32"};
    else
      prio = {"int8", "q4f16", "q4", "fp16", "fp32"};
  } else if (cpu.hasAVX512VNNI()) {
    prio = {"int8", "fp16", "q4f16", "q4", "fp32"};
  } else if (cpu.hasAVX512F()) {
    prio = {"fp16", "int8", "q4f16", "q4", "fp32"};
  } else if (cpu.hasAVX2()) {
    prio = {"int8", "fp16", "q4f16", "q4", "fp32"};
  } else {
    prio = {"fp32", "fp16", "int8", "q4f16", "q4"};
  }

  for (const auto *tag : prio) {
    for (const auto &m : models) {
      if (m.prec.tag == tag) {
        fill(m, "_auto");
        return result;
      }
    }
  }

  if (!models.empty()) {
    fill(models[0], "_fallback");
    return result;
  }
  return result;
}
}  // namespace ML
}  // namespace ShannonBase