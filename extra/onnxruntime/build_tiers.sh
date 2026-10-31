#!/usr/bin/env bash
#
# Build size-trimmed ONNX Runtime shared libraries for ShannonBase, one per
# x86-64 micro-architecture tier, laid out so that glibc picks the best one
# at load time (glibc-hwcaps, glibc >= 2.33).
#
# Results land in the layout auto-detected by cmake/FindONNXRuntime.cmake:
#
#   extra/onnxruntime/include/                            headers
#   extra/onnxruntime/lib/x86_64/                         baseline (portable)
#   extra/onnxruntime/lib/x86_64/glibc-hwcaps/x86-64-v3/  AVX2/FMA/BMI2
#   extra/onnxruntime/lib/x86_64/glibc-hwcaps/x86-64-v4/  AVX-512
#
# Usage:
#   ./build_tiers.sh                          # baseline + host tier
#   ./build_tiers.sh --tiers "x86-64-v2 x86-64-v3 x86-64-v4"
#   ./build_tiers.sh --ops-config ops.config  # trim kernels to used ops
#   ./build_tiers.sh --with-gpu               # extra CUDA provider package
#   ./build_tiers.sh --jobs 5 --work-dir /data/ort-build
#
set -euo pipefail

ORT_VERSION="${ORT_VERSION:-1.24.4}"
JOBS="${JOBS:-5}"
TIERS="${TIERS:-}"
OPS_CONFIG=""
WITH_GPU=0
CUDA_ARCHS=""
CUDA_HOME="${CUDA_HOME:-/usr/local/cuda}"
CUDNN_HOME="${CUDNN_HOME:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTRA_ROOT="$HERE"
LIB_ROOT="$EXTRA_ROOT/lib/x86_64"
WORK_DIR="${ORT_WORK_DIR:-$HOME/.cache/shannonbase/onnxruntime-build}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)     ORT_VERSION="$2"; shift 2 ;;
    --jobs)        JOBS="$2";        shift 2 ;;
    --tiers)       TIERS="$2";       shift 2 ;;
    --ops-config)  OPS_CONFIG="$2";  shift 2 ;;
    --work-dir)    WORK_DIR="$2";    shift 2 ;;
    --with-gpu)    WITH_GPU=1;        shift ;;
    --cuda-arch)   CUDA_ARCHS="$2";   shift 2 ;;
    --cuda-home)   CUDA_HOME="$2";    shift 2 ;;
    --cudnn-home)  CUDNN_HOME="$2";   shift 2 ;;
    -h|--help)     sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

arch="$(uname -m)"
if [[ "$arch" != "x86_64" ]]; then
  echo "This script only handles x86_64 tiers (host is $arch)." >&2
  exit 1
fi

# --- tier -> compiler flags -------------------------------------------------
# GCC >= 11 understands -march=x86-64-vN; fall back to explicit switches.
tier_flags() {
  local tier="$1" gcc_major
  gcc_major="$(gcc -dumpversion 2>/dev/null | cut -d. -f1 || echo 0)"
  if [[ "${gcc_major:-0}" -ge 11 ]]; then
    echo "-march=${tier}"
    return
  fi
  case "$tier" in
    x86-64-v2) echo "-msse4.2 -mpopcnt -mcx16 -mssse3 -msse4.1" ;;
    x86-64-v3) echo "-msse4.2 -mpopcnt -mcx16 -mssse3 -msse4.1 -mavx -mavx2 -mfma -mf16c -mbmi -mbmi2 -mmovbe -mlzcnt" ;;
    x86-64-v4) echo "-msse4.2 -mpopcnt -mcx16 -mssse3 -msse4.1 -mavx -mavx2 -mfma -mf16c -mbmi -mbmi2 -mmovbe -mlzcnt \
                     -mavx512f -mavx512bw -mavx512cd -mavx512dq -mavx512vl" ;;
    *) echo "" ;;
  esac
}

# --- host tier detection (mirrors cmake/FindONNXRuntime.cmake) --------------
detect_host_tier() {
  local flags
  flags="$(grep -m1 '^flags' /proc/cpuinfo | cut -d: -f2 || true)"
  [[ "$flags" =~ sse4_2 && "$flags" =~ popcnt && "$flags" =~ cx16 ]] || { echo ""; return; }
  local tier="x86-64-v2"
  if [[ "$flags" =~ " avx " && "$flags" =~ avx2 && "$flags" =~ fma &&
        "$flags" =~ f16c && "$flags" =~ bmi2 && "$flags" =~ movbe ]]; then
    tier="x86-64-v3"
  fi
  if [[ "$tier" == "x86-64-v3" && "$flags" =~ avx512f && "$flags" =~ avx512bw &&
        "$flags" =~ avx512dq && "$flags" =~ avx512vl ]]; then
    tier="x86-64-v4"
  fi
  echo "$tier"
}

HOST_TIER="$(detect_host_tier)"
if [[ -z "$TIERS" ]]; then
  TIERS="x86-64-v2 $HOST_TIER"
  [[ "$HOST_TIER" == "x86-64-v3" || "$HOST_TIER" == "x86-64-v4" ]] && TIERS="x86-64-v2 x86-64-v3"
fi
# Deduplicate, keep order, drop empties.
read -r -a tier_list <<< "$(echo "$TIERS" | tr ' ' '\n' | awk 'NF && !seen[$0]++' | tr '\n' ' ')"

echo "ORT version   : $ORT_VERSION"
echo "Host CPU tier : ${HOST_TIER:-unknown}"
echo "Tiers to build: ${tier_list[*]}"
echo "Work dir      : $WORK_DIR"

# --- fetch source once ------------------------------------------------------
SRC_DIR="$WORK_DIR/onnxruntime-$ORT_VERSION"
if [[ ! -d "$SRC_DIR/.git" ]]; then
  mkdir -p "$WORK_DIR"
  git clone --depth 1 --branch "v$ORT_VERSION" --recursive \
    https://github.com/microsoft/onnxruntime "$SRC_DIR"
fi

# --- build one tier ---------------------------------------------------------
build_variant() {
  local name="$1" flags="$2" out_dir="$3"
  local build_dir="$WORK_DIR/build-$name"

  echo "=== building $name (${flags:-default flags}) ==="
  local args=(
    --config MinSizeRel
    --build_shared_lib
    --parallel "$JOBS"
    --skip_tests
    --disable_ml_ops
    --compile_no_warning_as_error
    --build_dir "$build_dir"
  )
  # ORT_MINIMAL_BUILD: strips training, language bindings, unused tooling.
  # Pair with --include_ops_by_config to also drop unused kernels.
  if [[ -n "$OPS_CONFIG" ]]; then
    args+=(--minimal_build extended --include_ops_by_config "$OPS_CONFIG")
  fi
  if [[ -n "$flags" ]]; then
    args+=(--cmake_extra_defines "CMAKE_CXX_FLAGS=$flags")
  fi

  (cd "$SRC_DIR" && ./build.sh "${args[@]}")

  local lib_dir="$build_dir/MinSizeRel"
  local so
  so="$(ls -1 "$lib_dir"/libonnxruntime.so.*.* 2>/dev/null | head -1)"
  if [[ -z "$so" ]]; then
    echo "build failed: no libonnxruntime.so found in $lib_dir" >&2
    exit 1
  fi

  mkdir -p "$out_dir"
  cp -a "$lib_dir"/libonnxruntime.so* "$out_dir"/
  [[ -f "$lib_dir/libonnxruntime_providers_shared.so" ]] &&
    cp -a "$lib_dir/libonnxruntime_providers_shared.so" "$out_dir"/
  strip --strip-unneeded "$out_dir"/libonnxruntime.so.*.* 2>/dev/null || true

  # Normalise the symlink chain: this is what ld.so resolves at load time.
  local soname
  soname="$(basename "$so")"
  ln -sf "$soname" "$out_dir/libonnxruntime.so.1"
  ln -sf "libonnxruntime.so.1" "$out_dir/libonnxruntime.so"

  # Headers are identical for every tier.
  mkdir -p "$EXTRA_ROOT/include"
  cp -a "$SRC_DIR/include/onnxruntime/core/session/." "$EXTRA_ROOT/include/"

  echo "=== $name -> $(du -h "$out_dir/libonnxruntime.so.1" | cut -f1) ==="
}

# --- optional CUDA provider package -----------------------------------------
gpu_archs() {
  nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null |
    tr -d ' ' | awk 'NF && !seen[$0]++' | tr -d '.' | paste -sd';' -
}

cuda_version() {
  "${CUDA_HOME}/bin/nvcc" --version 2>/dev/null |
    sed -n 's/.*release \([0-9][0-9.]*\).*/\1/p' | head -1
}

cudnn_major() {
  local header
  for header in ${CUDNN_HOME:+"$CUDNN_HOME/include/cudnn_version.h"} \
                /usr/include/cudnn_version.h \
                /usr/local/cuda/include/cudnn_version.h; do
    [[ -f "$header" ]] || continue
    sed -n 's/^#define CUDNN_MAJOR[[:space:]]*\([0-9]*\).*/\1/p' "$header" | head -1
    return
  done
}

build_gpu_package() {
  if ! command -v nvidia-smi >/dev/null 2>&1; then
    echo "nvidia-smi not found: cannot detect the GPU architecture" >&2
    exit 1
  fi
  local archs cuda_ver cudnn_ver tag out_dir build_dir lib_dir args
  archs="${CUDA_ARCHS:-$(gpu_archs)}"
  if [[ -z "$archs" ]]; then
    echo "no NVIDIA GPU detected by nvidia-smi (use --cuda-arch to override)" >&2
    exit 1
  fi
  cuda_ver="$(cuda_version)"
  cudnn_ver="$(cudnn_major)"
  tag="cuda$( [[ -n "$cuda_ver" ]] && echo "$cuda_ver" | cut -d. -f1-2 || echo unknown )-cudnn${cudnn_ver:-unknown}"
  out_dir="$LIB_ROOT/providers/$tag"
  build_dir="$WORK_DIR/build-gpu"

  echo "=== GPU package: CMAKE_CUDA_ARCHITECTURES=$archs -> $out_dir ==="
  args=(
    --config MinSizeRel
    --build_shared_lib
    --parallel "$JOBS"
    --skip_tests
    --disable_ml_ops
    --compile_no_warning_as_error
    --build_dir "$build_dir"
    --use_cuda
    --cuda_home "$CUDA_HOME"
    --cmake_extra_defines "CMAKE_CUDA_ARCHITECTURES=$archs"
  )
  [[ -n "$CUDNN_HOME" ]] && args+=(--cudnn_home "$CUDNN_HOME")
  [[ -n "$OPS_CONFIG" ]] && args+=(--minimal_build extended --include_ops_by_config "$OPS_CONFIG")
  (cd "$SRC_DIR" && ./build.sh "${args[@]}")

  lib_dir="$build_dir/MinSizeRel"
  mkdir -p "$out_dir"
  cp -a "$lib_dir"/libonnxruntime.so* "$out_dir"/
  for f in libonnxruntime_providers_cuda.so libonnxruntime_providers_shared.so \
           libonnxruntime_providers_tensorrt.so; do
    [[ -f "$lib_dir/$f" ]] && cp -a "$lib_dir/$f" "$out_dir"/
  done

  # Distinct SONAME so dlopen() never collides with the shim itself.
  local main_lib
  main_lib="$(ls -1 "$out_dir"/libonnxruntime.so.*.* 2>/dev/null | head -1)"
  if [[ -n "$main_lib" ]]; then
    if command -v patchelf >/dev/null 2>&1; then
      patchelf --set-soname libonnxruntime_${tag//[.-]/_}.so.1 "$main_lib"
    else
      echo "patchelf not found: keeping the original SONAME (shim still works)" >&2
    fi
    ln -sf "$(basename "$main_lib")" "$out_dir/libonnxruntime.so.1"
    ln -sf libonnxruntime.so.1 "$out_dir/libonnxruntime.so"
  fi
  echo "=== GPU package ready: $(du -sh "$out_dir" | cut -f1) ==="
}

# Baseline: portable, used for linking and for CPUs below the lowest tier.
build_variant "baseline" "" "$LIB_ROOT"

for tier in "${tier_list[@]}"; do
  flags="$(tier_flags "$tier")"
  build_variant "$tier" "$flags" "$LIB_ROOT/glibc-hwcaps/$tier"
done

[[ "$WITH_GPU" == "1" ]] && build_gpu_package

echo
echo "Installed layout:"
find "$LIB_ROOT" -name 'libonnxruntime.so.*' -printf '  %-70p %s bytes\n' | sort
echo
echo "Verify runtime selection with:"
echo "  LD_DEBUG=libs <mysqld> 2>&1 | grep -i onnxruntime"
