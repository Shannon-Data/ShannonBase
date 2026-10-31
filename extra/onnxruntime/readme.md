# ONNXRuntime Integration

ShannonBase provides a built-in generic ONNXRuntime package by default, which works out-of-box across supported platforms.

For users who want maximum inference performance on a specific CPU architecture, ShannonBase also supports optional native optimized ONNXRuntime builds.

## Default Behavior

If no custom ONNXRuntime build is provided, the build system automatically downloads and uses the official generic ONNXRuntime release package from Microsoft.

This provides maximum compatibility across environments.

## Optional Native Optimized Build

Advanced users may place a native optimized ONNXRuntime build under:

```text
extra/onnxruntime/
  include/
  lib/
    x86_64/
    aarch64/
```

Create the directories:

```bash
mkdir -p extra/onnxruntime/include
mkdir -p extra/onnxruntime/lib/x86_64
mkdir -p extra/onnxruntime/lib/aarch64
```

Then copy the native optimized ONNXRuntime build artifacts into the corresponding directories:

* `include/*`
* `lib/libonnxruntime*`

Example:

```text
extra/onnxruntime/lib/x86_64/libonnxruntime.so
```

When a native optimized ONNXRuntime build exists under `extra/onnxruntime`, the build system automatically prefers it over the default generic ONNXRuntime release package.

This allows users to build ONNXRuntime with architecture-specific optimizations such as:

* `-march=native`
* `-march=znver2`
* AVX2
* AVX512
* VNNI
* ARMv8 optimized kernels

## Selection Priority

The ONNXRuntime selection order is:

1. Native optimized build under `extra/onnxruntime`
2. System-wide ONNXRuntime installation
3. Official generic ONNXRuntime release package

## Notes

Native optimized builds may not be portable across different CPU architectures or machines.

The default generic ONNXRuntime package remains the recommended option for production distribution and maximum compatibility.

---

# Multi-tier native builds (auto-selected at load time)

The official Linux x64 package is **not** a plain SSE2 build: MLAS (the
GEMM/conv library behind the CPU EP) already does runtime CPUID dispatch and
uses AVX2 / AVX-512 / VNNI kernels when the host supports them. What it cannot
do is apply `-march=...` codegen to the non-MLAS kernels (Eigen-based ops,
quantization, some transformer kernels). That gap is closed by shipping several
tiers of the same library and letting the dynamic loader pick one.

## Layout

```text
extra/onnxruntime/
  include/                                     # same headers for all tiers
  lib/x86_64/
    libonnxruntime.so -> libonnxruntime.so.1   # baseline, used for LINKING
    libonnxruntime.so.1 -> libonnxruntime.so.<ver>
    libonnxruntime.so.<ver>
    glibc-hwcaps/
      x86-64-v2/libonnxruntime.so.<ver>        # SSE4.2 / POPCNT / CX16
      x86-64-v3/libonnxruntime.so.<ver>        # AVX2 / FMA / BMI2
      x86-64-v4/libonnxruntime.so.<ver>        # AVX-512 F/BW/DQ/VL
```

`cmake/FindONNXRuntime.cmake` always links the **baseline** copy, so the
recorded `RPATH` is `$ORIGIN/../lib/onnxruntime`. At process start `ld.so`
prefers `glibc-hwcaps/<highest tier the CPU supports>/` inside that directory
and falls back to the baseline. No application code, no `dlopen`, no config.

Requirements:

* glibc >= 2.33 on the target. Older glibc ignores `glibc-hwcaps` and always
  loads the baseline (safe, just untuned).
* All tiers must be built from the **same** ORT version, so that the `SONAME`
  (`libonnxruntime.so.1`) and the C API version match.

## Building the tiers

```bash
# baseline + the best tier for the build machine
extra/onnxruntime/build_tiers.sh

# explicit tiers
extra/onnxruntime/build_tiers.sh --tiers "x86-64-v2 x86-64-v3 x86-64-v4"

# smallest binaries: drop kernels your models do not use (ops.config is
# produced by onnxruntime.tools.convert_onnx_models_to_ort)
extra/onnxruntime/build_tiers.sh --ops-config ops.config
```

The script uses `--minimal_build extended --disable_ml_ops --config MinSizeRel`
and strips the result. A trimmed tier is typically **4–7 MB** (the full generic
package is ~22 MB), so shipping three tiers costs roughly what the single
generic library costs today while covering every CPU generation.

Configure-time output tells you what was found:

```text
[ONNXRuntime] Using bundled native optimized build: .../lib/x86_64/libonnxruntime.so
[ONNXRuntime] Tiered libraries: x86-64-v2;x86-64-v3 (host CPU level: x86-64-v3;
              runtime picks the best one via glibc-hwcaps)
```

## Verifying / overriding at runtime

```bash
# which file did ld.so actually map?
LD_DEBUG=libs ./bin/mysqld --verbose --help 2>&1 | grep -i onnxruntime

# force the baseline (A/B regression comparison)
LD_HWCAP_MASK=0 ./bin/mysqld ...

# prefer a specific tier even when a better one exists
LD_LIBRARY_PATH=/opt/mysql/lib/onnxruntime/glibc-hwcaps/x86-64-v2 ./bin/mysqld ...
```

## Caveats

* A `-march`-tuned `.so` **crashes with SIGILL** on a CPU lacking those
  features. That is why tiered builds are never used for linking and only live
  in `glibc-hwcaps/` directories that the loader gates on CPUID.
* aarch64 tiers are not built by `build_tiers.sh` yet; the shim already
  understands `aarch64/v8.0|v8.2|v8.6|v9` directories, so only the build step
  is missing. NEON itself is part of the AArch64 baseline, no tier needed.
* Every extra tier is one more build to produce and to keep at the same ORT
  version — align the list with the actual deployment fleet.

---

# GPU and runtime selection

Three things are decided independently:

| Layer | Decided by | Where |
|---|---|---|
| ISA level (AVX2, i8mm, SVE2) | build (`-march`, tier dirs) | `build_tiers.sh` |
| Execution providers (CUDA, …) | build (provider present?) | `FindONNXRuntime.cmake` |
| Which EP/tier a process uses | runtime | `ML::OrtRuntime`, `ort_dispatch` |

## Build-time: is the CUDA provider available?

`FindONNXRuntime.cmake` looks for `libonnxruntime_providers_cuda.so` next to
the resolved library and, when present, defines `SHANNONBASE_ONNX_CUDA_EP` for
`sql_ml`. `-DONNXRUNTIME_WITH_CUDA=OFF` disables it, `=ON` warns loudly when the
package does not ship the provider.

The official `-gpu` package keeps the main library at the same size; the bulk
(200-400 MB) lives in the provider libraries.

## Runtime: is the GPU actually usable?

`ML::OrtRuntime::probe()` runs once per process and checks:

1. was the CUDA provider compiled in (`SHANNONBASE_ONNX_CUDA_EP`)?
2. does `libnvidia-ml.so.1` report a device (falls back to `/dev/nvidiaN`)?
3. is the model large enough (`SHANNON_ORT_GPU_MIN_BYTES`, default 256 MiB)?
   Small encoders such as MiniLM or the PII model stay on the CPU.

`ML::OrtRuntime::execute()` then appends the CUDA EP through
`OrtApi::SessionOptionsAppendExecutionProvider_CUDA` and falls back to CPU on
any error. Every ORT session site calls it:

```
ml/infra_component/sentence_transform.cpp    MiniLM
ml/infra_component/pii_detector.cpp          PIIDetector
ml/infra_component/llm_generate_onnx.cpp     TextGenerator
storage/rapid_engine/ml/query_arbitrator.cpp QueryArbitrator
```

Startup log lines:

```text
ONNX Runtime: backend=/opt/mysql/lib/onnxruntime/backends/x86_64/.../libonnxruntime.so.1.24.4
              cpu_tier=x86-64-v3 cuda_built=1 cuda_available=1 gpus=1 gpu=NVIDIA A100
ONNX Runtime: TextGenerator uses CUDA EP
```

Environment overrides:

```bash
SHANNON_ORT_EP=cpu            # never use the GPU
SHANNON_ORT_GPU_MIN_BYTES=…   # lower/raise the model-size threshold
```

## Dispatch shim (`ONNXRUNTIME_DISPATCH`)

When the package ships more than one backend, ShannonBase links a tiny
`libonnxruntime.so.1` shim instead of the real library. The shim exports
`OrtGetApiBase()` — the only C entry point the `Ort::` API uses — and forwards
it to the backend it selects:

```
GPU provider package  ->  per-CPU tier (hwcaps on x86, AT_HWCAP on aarch64)  ->  baseline
```

`ONNXRUNTIME_DISPATCH=AUTO` (default) enables it only when
`glibc-hwcaps/`, `x86_64/`, `aarch64/`, `providers/` or `backends/`
sub-directories are present, so a plain single-library package keeps linking
directly. `=ON` forces it, `=OFF` disables it.

This is the only way to tier aarch64, where glibc has no `hwcaps` directories.
Backends are searched relative to the shim, so the same tree works from the
build directory and from the installed prefix:

```text
lib/onnxruntime/
  libonnxruntime.so -> .so.1 -> .so.1.0.0     # shim
  backends/x86_64/glibc-hwcaps/x86-64-v3/…    # tier
  backends/x86_64/…                           # baseline
  backends/providers/cuda12.4-cudnn9/…        # optional GPU package
```

```bash
SHANNON_ORT_DEBUG=1       # print the selected backend to stderr
SHANNON_ORT_TIER=x86-64-v3 / v8.6   # pin a CPU tier
SHANNON_ORT_LIB_DIR=…     # alternate backend root
```

## GPU package

```bash
# auto-detects the compute capability from nvidia-smi and the CUDA/cuDNN
# versions, then builds libonnxruntime + providers into
#   lib/x86_64/providers/cuda12.4-cudnn9/
extra/onnxruntime/build_tiers.sh --with-gpu

extra/onnxruntime/build_tiers.sh --with-gpu --cuda-home /usr/local/cuda-12.4
```

`CMAKE_CUDA_ARCHITECTURES` is set from the detected GPUs only, which keeps the
fatbin small; pass `--cuda-arch "86;89"` to build for a mixed fleet instead.
Each GPU package gets a distinct `SONAME`
(`libonnxruntime_cuda12_4_cudnn9.so.1`) so the shim never re-opens itself.

