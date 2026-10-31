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
   Copyright (c) 2023 -, Shannon Data AI and/or its affiliates.
*/
#include "ml/infra_component/ort_runtime.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>

#include <filesystem>
#include <mutex>
#include <set>
#include <string>

#include "sql/log.h"

#include "extra/onnxruntime/ort_cpu_tiers.h"

namespace ShannonBase {
namespace ML {
namespace OrtRuntime {

namespace {

constexpr size_t kDefaultGpuMinBytes = 256ULL * 1024 * 1024;

std::string detect_cpu_tier() {
  const auto tiers = ShannonOrt::cpu_tiers();
  return tiers.empty() ? std::string() : tiers.front();
}

bool nvml_probe(int &devices, std::string &name, std::string &why) {
  void *handle = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    why = "libnvidia-ml.so.1 unavailable (no driver)";
    return false;
  }
  auto init = reinterpret_cast<int (*)()>(dlsym(handle, "nvmlInit_v2"));
  auto get_count = reinterpret_cast<int (*)(unsigned int *)>(dlsym(handle, "nvmlDeviceGetCount_v2"));
  auto get_handle = reinterpret_cast<int (*)(unsigned int, void **)>(dlsym(handle, "nvmlDeviceGetHandleByIndex_v2"));
  auto get_name = reinterpret_cast<int (*)(void *, char *, unsigned int)>(dlsym(handle, "nvmlDeviceGetName"));
  auto shutdown = reinterpret_cast<int (*)()>(dlsym(handle, "nvmlShutdown"));

  if (init == nullptr || get_count == nullptr) {
    dlclose(handle);
    why = "NVML entry points missing";
    return false;
  }
  if (init() != 0) {
    dlclose(handle);
    why = "nvmlInit failed (driver/kernel module mismatch)";
    return false;
  }

  bool ok = false;
  unsigned int count = 0;
  if (get_count(&count) == 0 && count > 0) {
    devices = static_cast<int>(count);
    if (get_handle != nullptr && get_name != nullptr) {
      void *dev = nullptr;
      char buf[96] = {0};
      if (get_handle(0, &dev) == 0 && get_name(dev, buf, sizeof(buf)) == 0) name = buf;
    }
    ok = true;
  } else {
    why = "NVML reports no device";
  }

  if (shutdown != nullptr) shutdown();
  dlclose(handle);
  return ok;
}

// Fall back to the device nodes when NVML is not installed.
bool device_node_probe(int &devices) {
  if (access("/dev/nvidiactl", R_OK) != 0) return false;
  int count = 0;
  for (int i = 0; i < 16; ++i) {
    std::string node = "/dev/nvidia" + std::to_string(i);
    if (access(node.c_str(), R_OK) == 0) ++count;
  }
  devices = count;
  return count > 0;
}

size_t gpu_min_bytes() {
  const char *env = getenv("SHANNON_ORT_GPU_MIN_BYTES");
  if (env != nullptr && *env != '\0') {
    char *end = nullptr;
    unsigned long long v = strtoull(env, &end, 10);
    if (end != env && v > 0) return static_cast<size_t>(v);
  }
  return kDefaultGpuMinBytes;
}

void probe_backend(Capability &cap) {
  Dl_info info{};
  const OrtApi &api = Ort::GetApi();
  if (dladdr(reinterpret_cast<const void *>(&api), &info) != 0 && info.dli_fname != nullptr) {
    cap.backend = info.dli_fname;
  }
}

}  // namespace

const Capability &probe() {
  static Capability cap = [] {
    Capability c;
#ifdef SHANNONBASE_ONNX_CUDA_EP
    c.cuda_built = true;
#else
    c.cuda_built = false;
    c.reason = "built without the CUDA provider";
#endif
    c.cpu_tier = detect_cpu_tier();

    const char *ep = getenv("SHANNON_ORT_EP");
    if (ep != nullptr && std::string(ep) == "cpu") {
      c.reason = "SHANNON_ORT_EP=cpu";
    } else if (c.cuda_built) {
      if (!nvml_probe(c.cuda_devices, c.gpu_name, c.reason) && !device_node_probe(c.cuda_devices)) {
        if (c.reason.empty()) c.reason = "no CUDA device visible to this process";
      } else {
        c.cuda_available = true;
        c.reason.clear();
      }
    }

    probe_backend(c);
    sql_print_information("ONNX Runtime: backend=%s cpu_tier=%s cuda_built=%d cuda_available=%d gpus=%d%s%s",
                          c.backend.empty() ? "(unknown)" : c.backend.c_str(), c.cpu_tier.c_str(),
                          static_cast<int>(c.cuda_built), static_cast<int>(c.cuda_available), c.cuda_devices,
                          c.gpu_name.empty() ? "" : " gpu=", c.gpu_name.empty() ? "" : c.gpu_name.c_str());
    if (!c.reason.empty()) sql_print_information("ONNX Runtime: CUDA EP not used: %s", c.reason.c_str());
    return c;
  }();
  return cap;
}

std::string execute(Ort::SessionOptions &options, const char *tag, size_t model_bytes) {
  static std::mutex log_mutex;
  static std::set<std::string> logged;

  const Capability &cap = probe();
  std::string ep = "CPU";
  std::string why;

  if (!cap.cuda_built) {
    why = cap.reason;
  } else if (!cap.cuda_available) {
    why = cap.reason;
  } else if (model_bytes > 0 && model_bytes < gpu_min_bytes()) {
    why = "model smaller than the GPU threshold";
  } else {
#ifdef SHANNONBASE_ONNX_CUDA_EP
    OrtCUDAProviderOptions cuda_options{};
    cuda_options.device_id = 0;
    cuda_options.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchHeuristic;
    cuda_options.arena_extend_strategy = 1;  // kSameAsRequested
    OrtStatus *status = Ort::GetApi().SessionOptionsAppendExecutionProvider_CUDA(*options, &cuda_options);
    if (status == nullptr) {
      ep = "CUDA";
    } else {
      why = std::string("append failed: ") + Ort::GetApi().GetErrorMessage(status);
      Ort::GetApi().ReleaseStatus(status);
    }
#endif
  }

  {
    std::lock_guard<std::mutex> guard(log_mutex);
    if (logged.insert(std::string(tag) + "/" + ep).second) {
      if (why.empty()) {
        sql_print_information("ONNX Runtime: %s uses %s EP", tag, ep.c_str());
      } else {
        sql_print_information("ONNX Runtime: %s uses %s EP (%s)", tag, ep.c_str(), why.c_str());
      }
    }
  }
  return ep;
}

std::string describe() {
  const Capability &cap = probe();
  std::string out = "cpu_tier=" + (cap.cpu_tier.empty() ? std::string("?") : cap.cpu_tier);
  out += cap.cuda_built ? " cuda_built=yes" : " cuda_built=no";
  out += cap.cuda_available ? " cuda_available=yes" : " cuda_available=no";
  if (!cap.backend.empty()) out += " backend=" + cap.backend;
  return out;
}

size_t file_size(const std::string &path) {
  if (path.empty()) return 0;
  std::error_code ec;
  auto size = std::filesystem::file_size(path, ec);
  return ec ? 0 : static_cast<size_t>(size);
}

}  // namespace OrtRuntime
}  // namespace ML
}  // namespace ShannonBase
