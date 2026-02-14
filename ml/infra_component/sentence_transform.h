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

#ifndef __SHANNONBASE_RAPID_SENTENCE_TRANSFORM_H__
#define __SHANNONBASE_RAPID_SENTENCE_TRANSFORM_H__

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>
#include "ml/infra_component/tokenizer.h"

#if defined(_WIN32)
#include <intrin.h>  // MSVC: __cpuidex / _xgetbv
#elif defined(__APPLE__)
#include <sys/sysctl.h>  // sysctlbyname
#include <sys/types.h>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#if defined(__aarch64__)
#include <TargetConditionals.h>  // TARGET_CPU_ARM64
#include <arm_neon.h>            // NEON intrinsics
#endif
#elif defined(__linux__)
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#if defined(__aarch64__) || defined(__arm__)
#include <asm/hwcap.h>  // HWCAP_* / HWCAP2_*
#include <sys/auxv.h>
#endif
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#endif

namespace ShannonBase {
namespace ML {
namespace SentenceTransform {
namespace fs = std::filesystem;
enum class Precision { FP32, FP16, BF16, INT8, UINT8, UNKNOWN };
enum class Device { CPU, GPU, NPU, ANE };  // ANE = Apple Neural Engine

struct ModelSelection {
  std::string filename;
  Precision precision;
  Device device;
  std::string opt_level;  // O1/O2/O3/O4/auto
};

class CPUDetector {
 public:
  enum class ModelType {
    // Apple
    APPLE_ANE_FP16,     // Core ML / ANE
    APPLE_M_NEON_FP16,  // Apple Silicon NEON FP16
    APPLE_M_NEON_INT8,  // Apple Silicon NEON INT8
    // x86
    AVX512_VNNI_INT8,
    AVX512F_INT8,
    AVX2_FMA_UINT8,
    AVX2_FP32,
    SSE4_FP32,
    // ARM / Linux
    ARM64_SVE_INT8,      // SVE (Neoverse N2, Graviton3+)
    ARM64_DOTPROD_INT8,  // ARMv8.2 dot-product
    ARM64_NEON_FP32,     // AArch64
    ARM32_NEON_FP32,     // ARMv7 + NEON
    BASELINE_FP32
  };

  CPUDetector() noexcept { detectFeatures(); }

  bool hasSSE4_1() const noexcept { return m_f.sse4_1; }
  bool hasSSE4_2() const noexcept { return m_f.sse4_2; }
  bool hasAVX() const noexcept { return m_f.avx; }
  bool hasAVX2() const noexcept { return m_f.avx2; }
  bool hasFMA() const noexcept { return m_f.fma; }
  bool hasAVX512F() const noexcept { return m_f.avx512f; }
  bool hasAVX512BW() const noexcept { return m_f.avx512bw; }
  bool hasAVX512DQ() const noexcept { return m_f.avx512dq; }
  bool hasAVX512VNNI() const noexcept { return m_f.avx512vnni; }

  bool isARM64() const noexcept { return m_f.arm64; }
  bool isARM32() const noexcept { return m_f.arm32; }
  bool hasNEON() const noexcept { return m_f.neon; }
  bool hasDotProd() const noexcept { return m_f.dotprod; }
  bool hasSVE() const noexcept { return m_f.sve; }
  bool hasFP16() const noexcept { return m_f.fp16; }
  bool hasBF16() const noexcept { return m_f.bf16; }

  bool isAppleSilicon() const noexcept { return m_f.apple_silicon; }
  bool hasAppleANE() const noexcept { return m_f.apple_ane; }
  bool isAppleIntelMac() const noexcept { return m_f.apple_intel; }

  ModelType recommendedModel() const noexcept {
    // Apple Silicon: first ANE，then NEON
    if (m_f.apple_silicon) {
      if (m_f.apple_ane) return ModelType::APPLE_ANE_FP16;
      if (m_f.fp16) return ModelType::APPLE_M_NEON_FP16;
      return ModelType::APPLE_M_NEON_INT8;
    }
    // x86
    if (m_f.avx512vnni) return ModelType::AVX512_VNNI_INT8;
    if (m_f.avx512f) return ModelType::AVX512F_INT8;
    if (m_f.avx2 && m_f.fma) return ModelType::AVX2_FMA_UINT8;
    if (m_f.avx2) return ModelType::AVX2_FP32;
    if (m_f.sse4_2) return ModelType::SSE4_FP32;
    // ARM Linux / Android
    if (m_f.arm64) {
      if (m_f.sve) return ModelType::ARM64_SVE_INT8;
      if (m_f.dotprod) return ModelType::ARM64_DOTPROD_INT8;
      return ModelType::ARM64_NEON_FP32;
    }
    if (m_f.arm32 && m_f.neon) return ModelType::ARM32_NEON_FP32;
    return ModelType::BASELINE_FP32;
  }

  std::string getCPUInfo() const {
    std::string s = "CPU Features: ";
    auto flag = [&](bool v, const char *n) {
      if (v) {
        s += n;
        s += ' ';
      }
    };
    flag(m_f.apple_silicon, "Apple-Silicon");
    flag(m_f.apple_ane, "ANE");
    flag(m_f.apple_intel, "Apple-Intel");
    flag(m_f.arm64, "ARM64");
    flag(m_f.arm32, "ARM32");
    flag(m_f.neon, "NEON");
    flag(m_f.dotprod, "DotProd");
    flag(m_f.sve, "SVE");
    flag(m_f.fp16, "FP16");
    flag(m_f.bf16, "BF16");
    flag(m_f.sse4_1, "SSE4.1");
    flag(m_f.sse4_2, "SSE4.2");
    flag(m_f.avx, "AVX");
    flag(m_f.avx2, "AVX2");
    flag(m_f.fma, "FMA");
    flag(m_f.avx512f, "AVX512F");
    flag(m_f.avx512bw, "AVX512BW");
    flag(m_f.avx512dq, "AVX512DQ");
    flag(m_f.avx512vnni, "AVX512-VNNI");
    return s;
  }

 private:
  struct CPUFeatures {
    // x86
    bool sse4_1 = false;
    bool sse4_2 = false;
    bool avx = false;
    bool avx2 = false;
    bool fma = false;
    bool avx512f = false;
    bool avx512bw = false;
    bool avx512dq = false;
    bool avx512vnni = false;
    // ARM
    bool arm64 = false;
    bool arm32 = false;
    bool neon = false;
    bool dotprod = false;
    bool sve = false;
    bool fp16 = false;
    bool bf16 = false;
    // Apple
    bool apple_silicon = false;
    bool apple_intel = false;
    bool apple_ane = false;  // Apple Neural Engine
  } m_f;

  static void cpuid_query(std::array<int, 4> &r, int func, int sub = 0) noexcept {
#if defined(_MSC_VER)
    __cpuidex(r.data(), func, sub);
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    unsigned eax, ebx, ecx, edx;
    __cpuid_count(func, sub, eax, ebx, ecx, edx);
    r[0] = int(eax);
    r[1] = int(ebx);
    r[2] = int(ecx);
    r[3] = int(edx);
#else
    r.fill(0);
#endif
  }

  static bool osxsave_check() noexcept {
#if defined(_MSC_VER)
    return (_xgetbv(0) & 0x6) == 0x6;
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    unsigned long long xcr0 = 0;
    __asm__ __volatile__("xgetbv" : "=A"(xcr0) : "c"(0));
    return (xcr0 & 0x6) == 0x6;
#else
    return false;
#endif
  }

#if defined(__APPLE__)
  static std::string sysctl_str(const char *name) noexcept {
    char buf[256] = {};
    size_t len = sizeof(buf);
    ::sysctlbyname(name, buf, &len, nullptr, 0);
    return buf;
  }
  static int sysctl_int(const char *name) noexcept {
    int val = 0;
    size_t len = sizeof(val);
    ::sysctlbyname(name, &val, &len, nullptr, 0);
    return val;
  }
#endif

  void detectFeatures() noexcept {
    // Apple
#if defined(__APPLE__)
#if defined(__aarch64__)
    m_f.arm64 = true;
    m_f.apple_silicon = true;
    m_f.neon = true;  // M has NEON
    // FP16 / BF16 / DotProd：M1+ support
    m_f.fp16 = sysctl_int("hw.optional.arm.FEAT_FP16") != 0;
    m_f.bf16 = sysctl_int("hw.optional.arm.FEAT_BF16") != 0;
    m_f.dotprod = sysctl_int("hw.optional.arm.FEAT_DotProd") != 0;
    m_f.sve = sysctl_int("hw.optional.arm.FEAT_SVE") != 0;
    // Apple Neural Engine：uing sysctl
    // M1/M2/M3/M4 has ANE；IOKit
    {
      auto model = sysctl_str("hw.model");  // e.g. "Mac14,3"
      m_f.apple_ane = !model.empty();
    }
#else  // Apple x86
    m_f.apple_intel = true;
    detectX86();
#endif
    return;
#endif
    // ARM Linux / Android
#if defined(__aarch64__)
    m_f.arm64 = true;
    m_f.neon = true;  // AArch64 has NEON
    detectARM64Linux();
    return;
#endif
#if defined(__arm__)
    m_f.arm32 = true;
    detectARM32Linux();
    return;
#endif
    // x86 Linux / Windows
    detectX86();
  }
  // x86
  void detectX86() noexcept {
    std::array<int, 4> r{};
    cpuid_query(r, 0);
    const int maxLeaf = r[0];
    if (maxLeaf < 1) return;

    cpuid_query(r, 1);
    const int ecx1 = r[2];
    const bool osxsave = (ecx1 & (1 << 27)) != 0;
    const bool avx_cpu = (ecx1 & (1 << 28)) != 0;

    m_f.sse4_1 = (ecx1 & (1 << 19)) != 0;
    m_f.sse4_2 = (ecx1 & (1 << 20)) != 0;
    m_f.fma = (ecx1 & (1 << 12)) != 0;

    // AVX / AVX2
    const bool avx_ok = osxsave && avx_cpu && osxsave_check();
    m_f.avx = avx_ok;

    if (maxLeaf >= 7) {
      cpuid_query(r, 7, 0);
      const int ebx7 = r[1];
      const int ecx7 = r[2];

      m_f.avx2 = avx_ok && ((ebx7 & (1 << 5)) != 0);
      m_f.avx512f = ((ebx7 & (1 << 16)) != 0);
      m_f.avx512bw = ((ebx7 & (1 << 30)) != 0);
      m_f.avx512dq = ((ebx7 & (1 << 17)) != 0);
      m_f.avx512vnni = ((ecx7 & (1 << 11)) != 0);
    }
  }

  // AArch64 Linux
  void detectARM64Linux() noexcept {
#if defined(__linux__) && defined(__aarch64__)
    unsigned long hwcap = ::getauxval(AT_HWCAP);
    unsigned long hwcap2 = ::getauxval(AT_HWCAP2);
    m_f.fp16 = (hwcap & HWCAP_FPHP) != 0;
    m_f.dotprod = (hwcap & HWCAP_ASIMDDP) != 0;
    m_f.sve = (hwcap & HWCAP_SVE) != 0;
    m_f.bf16 = (hwcap2 & HWCAP2_BF16) != 0;
    (void)hwcap2;
#endif
  }

  // ARMv7 Linux
  void detectARM32Linux() noexcept {
#if defined(__linux__) && defined(__arm__)
    unsigned long hwcap = ::getauxval(AT_HWCAP);
    m_f.neon = (hwcap & HWCAP_NEON) != 0;
#endif
  }
};

ModelSelection select_model_variant(const std::string &model_dir, const std::string &user_precision = "",
                                    const std::string &user_opt = "");

enum class STATUS_T {
  OK = 0,
  ERROR_INVALID_INPUT = 1,
  ERROR_MODEL_NOT_INIT = 2,
  ERROR_ONNX_INFERENCE_FAIL = 3,
  ERROR_OUTPUT_TENSOR_EMPTY = 4,
  ERROR_OUTPUT_SHAPE_INVALID = 5,
  ERROR_TOKENIZER_FAIL = 6,
  ERROR_PLATFORM_UNSUPPORTED = 7
};

class MiniLMEmbedding {
 public:
  using EmbeddingVector = std::vector<float>;

  struct EmbeddingResult {
    std::string text;
    EmbeddingVector embedding;
    double confidence = 0.0;
  };

  MiniLMEmbedding(const std::string &modelPath, const std::string &tokenizerPath = "");
  ~MiniLMEmbedding() = default;

  EmbeddingResult EmbedText(const std::string &text);
  std::vector<EmbeddingResult> EmbedFile(const std::string &filePath, size_t maxChunkSize = 512);
  std::vector<EmbeddingResult> EmbedBatch(const std::vector<std::string> &texts);

  static double CosineSimilarity(const EmbeddingVector &a, const EmbeddingVector &b);

  std::vector<std::pair<size_t, double>> SemanticSearch(const EmbeddingVector &queryEmbedding,
                                                        const std::vector<EmbeddingResult> &corpus, size_t topK = 5);

 private:
  void InitializeONNX();

  void ConfigureExecutionProviders(Ort::SessionOptions &opts);

  STATUS_T Tokenize(const std::string &text, tokenizers::Tokenizer::Encoding &enc) const;

  STATUS_T RunInference(const std::vector<int64_t> &input_ids, const std::vector<int64_t> &attention_mask,
                        const std::vector<int64_t> &token_type_ids, EmbeddingVector &result);

  void NormalizeL2(EmbeddingVector &vec) {
    double norm = std::sqrt(std::inner_product(vec.begin(), vec.end(), vec.begin(), 0.0));
    if (norm > 1e-9)
      for (float &v : vec) v /= static_cast<float>(norm);
  }

  std::vector<std::string> ReadAndChunkFile(const std::string &filePath, size_t maxChunkSize);

 private:
  std::string m_modelPath;
  std::string m_tokenizerPath;
  CPUDetector m_cpuDetector;

  std::unique_ptr<tokenizers::Tokenizer> m_tokenizer;
  std::unique_ptr<Ort::Env> m_ortEnv;
  std::unique_ptr<Ort::Session> m_ortSession;
  std::unique_ptr<Ort::SessionOptions> m_sessionOptions;
  std::vector<std::string> m_inputNames;
  std::vector<std::string> m_outputNames;
};

class DocumentEmbeddingManager {
 public:
  DocumentEmbeddingManager(const std::string &modelPath, const std::string &tokenizer)
      : m_embedder(modelPath, tokenizer) {}

  void ProcessDocument(const std::string &filePath);
  bool ProcessText(const std::string &text, size_t maxChunkSize = 512);
  std::vector<std::pair<std::string, double>> SemanticSearch(const std::string &query, size_t topK = 3);
  void SaveEmbeddings(const std::string &outputPath);

  inline std::vector<MiniLMEmbedding::EmbeddingResult> &Results() { return m_documentEmbeddings; }

 private:
  std::vector<std::string> SplitTextIntoChunks(const std::string &text, size_t maxChunkSize);
  MiniLMEmbedding m_embedder;
  std::vector<MiniLMEmbedding::EmbeddingResult> m_documentEmbeddings;
};
}  // namespace SentenceTransform
}  // namespace ML
}  // namespace ShannonBase
#endif  // __SHANNONBASE_RAPID_SENTENCE_TRANSFORM_H__