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
