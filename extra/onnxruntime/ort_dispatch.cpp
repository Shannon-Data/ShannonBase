// libonnxruntime.so.1 shim: forwards OrtGetApiBase() to the best backend.
//
// ShannonBase links "libonnxruntime.so.1". This tiny library exports exactly
// that entry point, picks a backend (GPU provider -> per-CPU tier -> baseline)
// and dlopen()s it, so the whole Ort:: C++ API runs against the chosen one.
//
// Environment overrides:
//   SHANNON_ORT_EP=cpu            never pick the GPU provider
//   SHANNON_ORT_TIER=x86-64-v3    pin a CPU tier (v8.2 on aarch64)
//   SHANNON_ORT_LIB_DIR=<dir>     backend root instead of the install dir
//   SHANNON_ORT_DEBUG=1           print the selection to stderr

#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "onnxruntime_c_api.h"
#include "ort_cpu_tiers.h"

#ifndef SHANNON_ORT_BACKEND_DIR
#define SHANNON_ORT_BACKEND_DIR ""
#endif
#ifndef SHANNON_ORT_ARCH
#define SHANNON_ORT_ARCH "x86_64"
#endif

// ORT_EXPORT is empty on Linux, so export the entry point ourselves.
#if defined(_WIN32)
#define SHANNON_ORT_EXPORT __declspec(dllexport)
#else
#define SHANNON_ORT_EXPORT __attribute__((visibility("default")))
#endif

namespace {

const char *g_debug = nullptr;

void debug_log(const std::string &msg) {
  if (g_debug == nullptr || *g_debug == '\0') return;
  fprintf(stderr, "ort_dispatch: %s\n", msg.c_str());
}

std::string real_path(const std::string &path) {
  char buf[PATH_MAX];
  if (realpath(path.c_str(), buf) == nullptr) return std::string();
  return std::string(buf);
}

bool is_dir(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool starts_with(const std::string &s, const char *prefix) {
  return s.compare(0, strlen(prefix), prefix) == 0;
}

// Directory holding this shim, so relatives resolve the same in build and
// installed trees.
std::string self_dir() {
  Dl_info info;
  if (dladdr(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&OrtGetApiBase)), &info) == 0 ||
      info.dli_fname == nullptr) {
    return std::string();
  }
  std::string p(info.dli_fname);
  size_t slash = p.rfind('/');
  return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

std::vector<std::string> list_dirs(const std::string &dir, const char *prefix) {
  std::vector<std::string> out;
  DIR *d = opendir(dir.c_str());
  if (d == nullptr) return out;
  while (struct dirent *e = readdir(d)) {
    std::string name(e->d_name);
    if (name == "." || name == "..") continue;
    if (prefix != nullptr && !starts_with(name, prefix)) continue;
    if (is_dir(dir + "/" + name)) out.push_back(name);
  }
  closedir(d);
  return out;
}

// Absolute path of this shim, so it is never mistaken for a backend.
const std::string &self_real_path() {
  static std::string path = [] {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&OrtGetApiBase)), &info) == 0 ||
        info.dli_fname == nullptr) {
      return std::string();
    }
    return real_path(info.dli_fname);
  }();
  return path;
}

// Real backend library inside dir, skipping anything that resolves to us.
std::string find_backend(const std::string &dir) {
  if (!is_dir(dir)) return std::string();
  DIR *d = opendir(dir.c_str());
  if (d == nullptr) return std::string();
  const std::string self = self_real_path();
  std::string best;
  std::string best_path;
  while (struct dirent *e = readdir(d)) {
    std::string name(e->d_name);
    if (!starts_with(name, "libonnxruntime.so")) continue;
    if (starts_with(name, "libonnxruntime_providers")) continue;
    std::string full = dir + "/" + name;
    std::string real = real_path(full);
    if (real.empty()) continue;
    if (!self.empty() && real == self) continue;
    if (!is_file(real)) continue;
    // Longest name wins: libonnxruntime.so.1.24.4 over libonnxruntime.so.
    if (name.size() > best.size()) {
      best = name;
      best_path = full;
    }
  }
  closedir(d);
  return best_path;
}


bool gpu_usable() {
  const char *ep = getenv("SHANNON_ORT_EP");
  if (ep != nullptr && strcmp(ep, "cpu") == 0) return false;
  const char *cvd = getenv("CUDA_VISIBLE_DEVICES");
  if (cvd != nullptr && (cvd[0] == '\0' || strcmp(cvd, "-1") == 0 || strcmp(cvd, "none") == 0)) {
    return false;
  }
  return access("/dev/nvidiactl", R_OK) == 0;
}

void add_gpu_candidates(const std::string &base, std::vector<std::string> &out) {
  if (!gpu_usable()) return;
  const std::string roots[] = {base + "/providers", base + "/" SHANNON_ORT_ARCH + "/providers",
                               base + "/backends/providers",
                               base + "/backends/" SHANNON_ORT_ARCH + "/providers"};
  for (const std::string &root : roots) {
    std::vector<std::string> providers = list_dirs(root, nullptr);
    for (const std::string &p : providers) {
      if (!starts_with(p, "cuda") && !starts_with(p, "rocm") && !starts_with(p, "tensorrt")) continue;
      std::string dir = root + "/" + p;
      if (!find_backend(dir).empty()) out.push_back(dir);
    }
  }
}

std::vector<std::string> build_candidates(const std::string &root, bool allow_gpu) {
  std::vector<std::string> out;
  std::vector<std::string> tiers = ShannonOrt::cpu_tiers();

  const char *pinned = getenv("SHANNON_ORT_TIER");
  if (pinned != nullptr && *pinned) tiers.insert(tiers.begin(), pinned);

  if (allow_gpu) add_gpu_candidates(root, out);

  for (const std::string &tier : tiers) {
    out.push_back(root + "/backends/" + SHANNON_ORT_ARCH + "/glibc-hwcaps/" + tier);
    out.push_back(root + "/backends/" + SHANNON_ORT_ARCH + "/" + tier);
    out.push_back(root + "/" + SHANNON_ORT_ARCH + "/glibc-hwcaps/" + tier);
    out.push_back(root + "/" + SHANNON_ORT_ARCH + "/" + tier);
    // The backend root may already be the per-arch directory.
    out.push_back(root + "/glibc-hwcaps/" + tier);
    out.push_back(root + "/" + tier);
  }
  out.push_back(root + "/backends/" + SHANNON_ORT_ARCH);
  out.push_back(root + "/" + SHANNON_ORT_ARCH);
  out.push_back(root);
  return out;
}

const OrtApiBase *resolve() {
  g_debug = getenv("SHANNON_ORT_DEBUG");

  std::vector<std::string> roots;
  const char *env_root = getenv("SHANNON_ORT_LIB_DIR");
  if (env_root != nullptr && *env_root) roots.push_back(env_root);
  const std::string dir = self_dir();
  if (!dir.empty()) roots.push_back(dir);
  if (SHANNON_ORT_BACKEND_DIR[0] != '\0' && SHANNON_ORT_BACKEND_DIR != dir) {
    roots.push_back(SHANNON_ORT_BACKEND_DIR);
  }

  for (size_t r = 0; r < roots.size(); ++r) {
    std::vector<std::string> dirs = build_candidates(roots[r], true);
    for (size_t i = 0; i < dirs.size(); ++i) {
      std::string lib = find_backend(dirs[i]);
      if (lib.empty()) continue;
      void *handle = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (handle == nullptr) {
        const char *err = dlerror();
        debug_log("dlopen failed for " + lib + ": " + (err == nullptr ? "" : err));
        continue;
      }
      auto fn = reinterpret_cast<const OrtApiBase *(ORT_API_CALL *)(void)>(dlsym(handle, "OrtGetApiBase"));
      if (fn == nullptr) {
        dlclose(handle);
        continue;
      }
      debug_log("using backend " + lib);
      return fn();
    }
  }

  fprintf(stderr,
          "ort_dispatch: no ONNX Runtime backend found under '%s' "
          "(set SHANNON_ORT_LIB_DIR)\n",
          roots.empty() ? "" : roots[0].c_str());
  return nullptr;
}

}  // namespace

SHANNON_ORT_EXPORT const OrtApiBase *ORT_API_CALL OrtGetApiBase(void) NO_EXCEPTION {
  static const OrtApiBase *api = resolve();
  return api;
}
