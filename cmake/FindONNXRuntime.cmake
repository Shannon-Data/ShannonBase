# ================================================
# FindONNXRuntime.cmake
# A CMake module to find or download ONNX Runtime.
#
# Priority:
#   1. Native optimized ONNXRuntime under:
#        extra/onnxruntime/
#      (optionally with per-CPU tiers under lib/<arch>/glibc-hwcaps/x86-64-vN/
#       that ld.so selects at load time, see extra/onnxruntime/readme.md)
#   2. System installation
#   3. Official generic ONNXRuntime release package
#
# This allows advanced users to provide architecture-optimized
# ONNXRuntime builds while keeping the default generic package
# for maximum compatibility.
# Supports: Linux, macOS, Windows
# Supports: CPU or GPU builds (based on USE_GPU env)
# ================================================

# Set default ONNX Runtime version
if (NOT ONNXRUNTIME_VERSION)
  set(ONNXRUNTIME_VERSION "1.24.4")
endif()

# Automatically determine backend: cpu or gpu
if (DEFINED ENV{USE_GPU})
  message(STATUS "[ONNXRuntime] USE_GPU is set in the environment. Using GPU backend.")
  set(ONNXRUNTIME_BACKEND "gpu")
else()
  set(ONNXRUNTIME_BACKEND "cpu")
endif()

# Optional override: ONNXRUNTIME_WITH_CUDA=ON|OFF forces the CUDA EP decision,
# AUTO (default) follows what the resolved package actually ships.
if (NOT DEFINED ONNXRUNTIME_WITH_CUDA)
  set(ONNXRUNTIME_WITH_CUDA "AUTO")
endif()

# Directory of this module, captured before any macro expansion so that
# CMAKE_CURRENT_LIST_DIR scoping rules cannot bite us.
set(_ORT_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Finalize every exit path: detect the CUDA provider, then set up the
# dispatch shim when the layout calls for it.
macro(_ort_finalize)
  set(ONNXRUNTIME_HAS_CUDA_PROVIDER FALSE)
  if (ONNXRUNTIME_LIB_DIR AND NOT ONNXRUNTIME_WITH_CUDA STREQUAL "OFF")
    # Match the same roots as the runtime dispatcher, including packaged
    # provider variants and already-installed backend trees.
    set(_ort_cuda_providers "")
    foreach(_ort_provider_root
        "${ONNXRUNTIME_LIB_DIR}"
        "${ONNXRUNTIME_LIB_DIR}/providers/*"
        "${ONNXRUNTIME_LIB_DIR}/${_extra_arch}/providers/*"
        "${ONNXRUNTIME_LIB_DIR}/backends/providers/*"
        "${ONNXRUNTIME_LIB_DIR}/backends/${_extra_arch}/providers/*")
      file(GLOB _ort_provider_files
           "${_ort_provider_root}/libonnxruntime_providers_cuda.so*"
           "${_ort_provider_root}/libonnxruntime_providers_cuda.dylib"
           "${_ort_provider_root}/onnxruntime_providers_cuda.dll")
      list(APPEND _ort_cuda_providers ${_ort_provider_files})
    endforeach()
    unset(_ort_provider_files)
    unset(_ort_provider_root)
    if (_ort_cuda_providers)
      set(ONNXRUNTIME_HAS_CUDA_PROVIDER TRUE)
      list(GET _ort_cuda_providers 0 _ort_cuda_provider_file)
    endif()
    unset(_ort_cuda_providers)
  endif()
  if (ONNXRUNTIME_WITH_CUDA STREQUAL "ON" AND NOT ONNXRUNTIME_HAS_CUDA_PROVIDER)
    message(WARNING
      "[ONNXRuntime] ONNXRUNTIME_WITH_CUDA=ON but no CUDA provider was found "
      "next to ${ONNXRUNTIME_LIBRARIES}; the CUDA EP stays disabled.")
  endif()
  if (ONNXRUNTIME_HAS_CUDA_PROVIDER)
    message(STATUS
      "[ONNXRuntime] CUDA provider available: ${_ort_cuda_provider_file} "
      "(SHANNONBASE_ONNX_CUDA_EP; the server still probes the GPU at startup)")
    unset(_ort_cuda_provider_file)
  endif()

  include(${_ORT_MODULE_DIR}/ONNXRuntimeDispatch.cmake)
endmacro()

# Detect platform and architecture
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_os "linux")
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_arch "aarch64")
  elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    set(_arch "arm")
  else()
    set(_arch "x64")
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(_os "osx")
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    set(_arch "arm64")
  else()
    set(_arch "x64")
  endif()
elseif(CMAKE_SYSTEM_NAME MATCHES "Windows")
  set(_os "win")
  set(_arch "x64") # Windows ARM not currently supported by ONNX Runtime
else()
  message(FATAL_ERROR "[ONNXRuntime] Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

# Determine archive filename based on platform, architecture and backend
if (_os STREQUAL "linux")
  if (_arch STREQUAL "aarch64")
    if (ONNXRUNTIME_BACKEND STREQUAL "cpu")
      set(_archive_name "onnxruntime-linux-${_arch}-${ONNXRUNTIME_VERSION}.tgz")
    else()
      message(WARNING "[ONNXRuntime] GPU backend not available for ARM64, using CPU")
      set(_archive_name "onnxruntime-linux-${_arch}-${ONNXRUNTIME_VERSION}.tgz")
    endif()
  elseif(_arch STREQUAL "arm")
    if (ONNXRUNTIME_BACKEND STREQUAL "cpu")
      set(_archive_name "onnxruntime-linux-${_arch}-${ONNXRUNTIME_VERSION}.tgz")
    else()
      message(WARNING "[ONNXRuntime] GPU backend not available for ARM, using CPU")
      set(_archive_name "onnxruntime-linux-${_arch}-${ONNXRUNTIME_VERSION}.tgz")
    endif()
  else()
    if (ONNXRUNTIME_BACKEND STREQUAL "cpu")
      set(_archive_name "onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz")
    else()
      set(_archive_name "onnxruntime-linux-x64-gpu-${ONNXRUNTIME_VERSION}.tgz")
    endif()
  endif()
elseif(_os STREQUAL "osx")
  if (_arch STREQUAL "arm64")
    set(_archive_name "onnxruntime-osx-arm64-${ONNXRUNTIME_VERSION}.tgz")
  else()
    set(_archive_name "onnxruntime-osx-x64-${ONNXRUNTIME_VERSION}.tgz")
  endif()
elseif(_os STREQUAL "win")
  if (ONNXRUNTIME_BACKEND STREQUAL "cpu")
    set(_archive_name "onnxruntime-win-x64-${ONNXRUNTIME_VERSION}.zip")
  else()
    set(_archive_name "onnxruntime-win-x64-gpu-${ONNXRUNTIME_VERSION}.zip")
  endif()
endif()

# Download and install locations
set(_onnx_url "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/${_archive_name}")
set(_download_path "${CMAKE_BINARY_DIR}/${_archive_name}")
set(_extract_dir "${CMAKE_BINARY_DIR}/_deps")
set(_install_dir "${_extract_dir}/onnxruntime")

# Escape hatches for offline / mirrored / rate-limited environments.
set(ONNXRUNTIME_ARCHIVE_URL "" CACHE STRING
  "Override the ONNXRuntime archive URL (mirror or pre-published copy)")
set(ONNXRUNTIME_ARCHIVE_SHA256 "" CACHE STRING
  "Expected SHA256 of the ONNXRuntime archive (empty disables the check)")
set(ONNXRUNTIME_DOWNLOAD_RETRIES "3" CACHE STRING
  "Number of attempts when downloading the ONNXRuntime archive")

if (ONNXRUNTIME_ARCHIVE_URL)
  set(_onnx_url "${ONNXRUNTIME_ARCHIVE_URL}")
endif()

# ============================================================
# 1. Prefer bundled native optimized ONNXRuntime if available
# ============================================================
#
# Two layouts are auto-detected:
#
#   baseline only (legacy):
#     extra/onnxruntime/lib/<arch>/libonnxruntime.so*
#
#   multi-tier (recommended, glibc >= 2.33):
#     extra/onnxruntime/lib/<arch>/libonnxruntime.so*                 <- baseline
#     extra/onnxruntime/lib/<arch>/glibc-hwcaps/x86-64-v2/...         <- tier
#     extra/onnxruntime/lib/<arch>/glibc-hwcaps/x86-64-v3/...
#     extra/onnxruntime/lib/<arch>/glibc-hwcaps/x86-64-v4/...
#
# We always link against the baseline copy so the recorded RPATH points at
# the shared directory; ld.so then transparently picks the best matching
# tier at load time. All tiers must be built from the same ORT version so
# they export the same SONAME (libonnxruntime.so.1).
# ============================================================

set(_extra_onnxruntime_root
    "${CMAKE_SOURCE_DIR}/extra/onnxruntime")

if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(_extra_arch "aarch64")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  set(_extra_arch "x86_64")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
  set(_extra_arch "arm")
else()
  set(_extra_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(_extra_lib_dirs
    "${_extra_onnxruntime_root}/lib/${_extra_arch}"
    "${_extra_onnxruntime_root}/lib")

# Micro-architecture level of the CPU this build is configured on.
# Informational only: the actual selection happens in ld.so at runtime.
set(ONNXRUNTIME_HOST_TIER "")
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64" AND EXISTS "/proc/cpuinfo")
  file(READ "/proc/cpuinfo" _ort_cpuinfo LIMIT 131072)
  string(REGEX MATCH "(^|\n)flags[ \t]*:([^\n]*)" _ort_flags_match "${_ort_cpuinfo}")
  set(_ort_cpu_flags "${CMAKE_MATCH_2}")
  if (_ort_cpu_flags MATCHES "sse4_2" AND
      _ort_cpu_flags MATCHES "popcnt" AND
      _ort_cpu_flags MATCHES "cx16")
    set(ONNXRUNTIME_HOST_TIER "x86-64-v2")
  endif()
  if (ONNXRUNTIME_HOST_TIER AND
      _ort_cpu_flags MATCHES "(^| )avx( |$)" AND
      _ort_cpu_flags MATCHES "avx2" AND
      _ort_cpu_flags MATCHES "fma" AND
      _ort_cpu_flags MATCHES "f16c" AND
      _ort_cpu_flags MATCHES "bmi2" AND
      _ort_cpu_flags MATCHES "movbe")
    set(ONNXRUNTIME_HOST_TIER "x86-64-v3")
  endif()
  if (ONNXRUNTIME_HOST_TIER STREQUAL "x86-64-v3" AND
      _ort_cpu_flags MATCHES "avx512f" AND
      _ort_cpu_flags MATCHES "avx512bw" AND
      _ort_cpu_flags MATCHES "avx512dq" AND
      _ort_cpu_flags MATCHES "avx512vl")
    set(ONNXRUNTIME_HOST_TIER "x86-64-v4")
  endif()
  unset(_ort_cpuinfo)
  unset(_ort_cpu_flags)
  unset(_ort_flags_match)
endif()

# Tiers actually shipped next to the baseline library.
set(ONNXRUNTIME_AVAILABLE_TIERS "")
foreach(_dir ${_extra_lib_dirs})
  file(GLOB _ort_tier_dirs
       "${_dir}/glibc-hwcaps/x86-64-v*"
       "${_dir}/x86-64-v*")
  foreach(_tier_dir ${_ort_tier_dirs})
    if (IS_DIRECTORY "${_tier_dir}" AND EXISTS "${_tier_dir}/libonnxruntime.so")
      get_filename_component(_tier_name "${_tier_dir}" NAME)
      list(APPEND ONNXRUNTIME_AVAILABLE_TIERS "${_tier_name}")
    endif()
  endforeach()
endforeach()
if (ONNXRUNTIME_AVAILABLE_TIERS)
  list(REMOVE_DUPLICATES ONNXRUNTIME_AVAILABLE_TIERS)
  list(SORT ONNXRUNTIME_AVAILABLE_TIERS)
endif()

find_path(ONNXRUNTIME_NATIVE_INCLUDE_DIR
  NAMES onnxruntime_c_api.h
  PATHS
    "${_extra_onnxruntime_root}/include"
  NO_DEFAULT_PATH)

# Baseline library only (not the tiered copies; find_library does not recurse).
find_library(ONNXRUNTIME_NATIVE_LIBRARY
  NAMES onnxruntime
  PATHS
    ${_extra_lib_dirs}
  NO_DEFAULT_PATH)

# Tiered bundle without a baseline copy: link against the highest tier.
if (NOT ONNXRUNTIME_NATIVE_LIBRARY AND ONNXRUNTIME_AVAILABLE_TIERS)
  list(GET ONNXRUNTIME_AVAILABLE_TIERS -1 _ort_any_tier)
  foreach(_dir ${_extra_lib_dirs})
    foreach(_sub "glibc-hwcaps/${_ort_any_tier}" "${_ort_any_tier}")
      if (EXISTS "${_dir}/${_sub}/libonnxruntime.so")
        set(ONNXRUNTIME_NATIVE_LIBRARY "${_dir}/${_sub}/libonnxruntime.so")
        set(_ort_baseline_missing TRUE)
        set(_ort_fallback_lib_root "${_dir}")
        break()
      endif()
    endforeach()
    if (ONNXRUNTIME_NATIVE_LIBRARY)
      break()
    endif()
  endforeach()
endif()

if (ONNXRUNTIME_NATIVE_INCLUDE_DIR AND
    ONNXRUNTIME_NATIVE_LIBRARY)

  set(ONNXRUNTIME_FOUND TRUE)

  set(ONNXRUNTIME_INCLUDE_DIRS
      ${ONNXRUNTIME_NATIVE_INCLUDE_DIR})

  set(ONNXRUNTIME_LIBRARIES
      ${ONNXRUNTIME_NATIVE_LIBRARY})

  get_filename_component(
    ONNXRUNTIME_LIB_DIR
    ${ONNXRUNTIME_NATIVE_LIBRARY}
    DIRECTORY)

  # Keep the arch root so tiers stay discoverable when there is no baseline.
  if (_ort_baseline_missing AND _ort_fallback_lib_root)
    set(ONNXRUNTIME_LIB_DIR "${_ort_fallback_lib_root}")
  endif()

  message(STATUS
    "[ONNXRuntime] Using bundled native optimized build: "
    "${ONNXRUNTIME_NATIVE_LIBRARY}")

  if (ONNXRUNTIME_AVAILABLE_TIERS)
    message(STATUS
      "[ONNXRuntime] Tiered libraries: ${ONNXRUNTIME_AVAILABLE_TIERS} "
      "(host CPU level: ${ONNXRUNTIME_HOST_TIER}; "
      "runtime picks the best one via glibc-hwcaps)")
    if (_ort_baseline_missing)
      message(WARNING
        "[ONNXRuntime] No baseline libonnxruntime.so next to the tier "
        "directories; this build will only run on CPUs supporting "
        "${_ort_any_tier}.")
    endif()
  elseif(ONNXRUNTIME_HOST_TIER)
    message(STATUS
      "[ONNXRuntime] Single-tier bundle, host CPU level: "
      "${ONNXRUNTIME_HOST_TIER}")
  endif()

  _ort_finalize()
  return()
endif()

# ============================================================
# 2. Try system-wide ONNX Runtime installation
# ============================================================
find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_c_api.h 
  PATHS /usr/include /usr/local/include /opt/include
  NO_DEFAULT_PATH)
find_library(ONNXRUNTIME_LIBRARY onnxruntime 
  PATHS /usr/lib /usr/local/lib /opt/lib /usr/lib/aarch64-linux-gnu /usr/lib/arm-linux-gnueabihf
  NO_DEFAULT_PATH)

if (ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
  set(ONNXRUNTIME_FOUND TRUE)
  set(ONNXRUNTIME_INCLUDE_DIRS ${ONNXRUNTIME_INCLUDE_DIR})
  set(ONNXRUNTIME_LIBRARIES ${ONNXRUNTIME_LIBRARY})
  get_filename_component(
    ONNXRUNTIME_LIB_DIR
    ${ONNXRUNTIME_LIBRARY}
    DIRECTORY)
  message(STATUS "[ONNXRuntime] Found system installation.")
  _ort_finalize()
  return()
endif()

# Check if already downloaded and extracted
if (EXISTS "${_install_dir}/include/onnxruntime_c_api.h")
  message(STATUS "[ONNXRuntime] Found existing installation at ${_install_dir}")
else()
  # NuGet mirrors the official binaries behind a stable URL; .nupkg is a zip.
  if (ONNXRUNTIME_BACKEND STREQUAL "gpu")
    set(_ort_nuget_id "microsoft.ml.onnxruntime.gpu")
  else()
    set(_ort_nuget_id "microsoft.ml.onnxruntime")
  endif()
  set(_ort_nuget_file "${_ort_nuget_id}.${ONNXRUNTIME_VERSION}.nupkg")
  set(_ort_nuget_url
    "https://api.nuget.org/v3-flatcontainer/${_ort_nuget_id}/${ONNXRUNTIME_VERSION}/${_ort_nuget_file}")

  if (_os STREQUAL "linux")
    if (_arch STREQUAL "aarch64")
      set(_ort_rid "linux-arm64")
    elseif(_arch STREQUAL "arm")
      set(_ort_rid "linux-arm")
    else()
      set(_ort_rid "linux-x64")
    endif()
  elseif(_os STREQUAL "osx")
    if (_arch STREQUAL "arm64")
      set(_ort_rid "osx-arm64")
    else()
      set(_ort_rid "osx-x64")
    endif()
  else()
    set(_ort_rid "win-x64")
  endif()

  # Tried in order: explicit override, GitHub release tarball, NuGet package.
  set(_ort_sources "")
  if (ONNXRUNTIME_ARCHIVE_URL)
    list(APPEND _ort_sources "tarball|${ONNXRUNTIME_ARCHIVE_URL}|${_archive_name}")
  else()
    list(APPEND _ort_sources "tarball|${_onnx_url}|${_archive_name}")
    list(APPEND _ort_sources "nuget|${_ort_nuget_url}|${_ort_nuget_file}")
  endif()

  message(STATUS "[ONNXRuntime] No native/system installation found. Sources:")
  foreach(_ort_source IN LISTS _ort_sources)
    string(REPLACE "|" ";" _ort_src "${_ort_source}")
    list(GET _ort_src 1 _ort_url)
    message(STATUS "[ONNXRuntime]   ${_ort_url}")
  endforeach()

  set(_ort_dl_log "${CMAKE_BINARY_DIR}/onnxruntime-download.log")
  set(_ort_archive "")
  set(_ort_kind "")
  set(_ort_failures "")
  foreach(_ort_source IN LISTS _ort_sources)
    string(REPLACE "|" ";" _ort_src "${_ort_source}")
    list(GET _ort_src 0 _ort_kind_try)
    list(GET _ort_src 1 _ort_url)
    list(GET _ort_src 2 _ort_file)
    set(_ort_path "${CMAKE_BINARY_DIR}/${_ort_file}")
    set(_ort_ok FALSE)
    set(_ort_error "no attempt made")

    # A previously downloaded archive can be reused (CI caches, offline builds).
    if (EXISTS "${_ort_path}")
      file(SIZE "${_ort_path}" _ort_cached_size)
      if (_ort_cached_size GREATER 1048576)
        message(STATUS "[ONNXRuntime] Reusing cached ${_ort_file}")
        set(_ort_ok TRUE)
      else()
        file(REMOVE "${_ort_path}")
      endif()
    endif()

    set(_ort_attempt 0)
    while (NOT _ort_ok AND _ort_attempt LESS ONNXRUNTIME_DOWNLOAD_RETRIES)
      math(EXPR _ort_attempt "${_ort_attempt} + 1")
      file(REMOVE "${_ort_path}")
      message(STATUS
        "[ONNXRuntime] ${_ort_kind_try} attempt ${_ort_attempt}/"
        "${ONNXRUNTIME_DOWNLOAD_RETRIES}")

      set(_ort_dl_args
          STATUS _ort_dl_status
          SHOW_PROGRESS
          TIMEOUT 900
          INACTIVITY_TIMEOUT 120)
      if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.18)
        list(APPEND _ort_dl_args LOG "${_ort_dl_log}")
      endif()
      if (ONNXRUNTIME_ARCHIVE_SHA256 AND _ort_kind_try STREQUAL "tarball")
        list(APPEND _ort_dl_args
             EXPECTED_HASH "SHA256=${ONNXRUNTIME_ARCHIVE_SHA256}")
      endif()
      file(DOWNLOAD "${_ort_url}" "${_ort_path}" ${_ort_dl_args})

      list(GET _ort_dl_status 0 _ort_dl_code)
      list(LENGTH _ort_dl_status _ort_dl_len)
      if (_ort_dl_len GREATER 1)
        list(GET _ort_dl_status 1 _ort_error)
      endif()

      if (_ort_dl_code EQUAL 0 AND EXISTS "${_ort_path}")
        file(SIZE "${_ort_path}" _ort_dl_size)
        if (_ort_dl_size GREATER 1048576)
          set(_ort_ok TRUE)
        else()
          set(_ort_error "archive is only ${_ort_dl_size} bytes")
        endif()
      else()
        set(_ort_error "(${_ort_dl_code}) ${_ort_error}")
      endif()
      unset(_ort_dl_status)

      if (NOT _ort_ok)
        message(WARNING
          "[ONNXRuntime] ${_ort_kind_try} attempt ${_ort_attempt} failed: ${_ort_error}")
        if (_ort_attempt LESS ONNXRUNTIME_DOWNLOAD_RETRIES)
          execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 5)
        endif()
      endif()
    endwhile()

    if (_ort_ok)
      set(_ort_archive "${_ort_path}")
      set(_ort_kind "${_ort_kind_try}")
      break()
    endif()
    string(APPEND _ort_failures "\n    ${_ort_url}\n      ${_ort_error}")
  endforeach()

  if (NOT _ort_archive)
    message(FATAL_ERROR
      "[ONNXRuntime] Could not obtain ONNX Runtime ${ONNXRUNTIME_VERSION}:"
      "${_ort_failures}\n"
      "  Workarounds: pass -DONNXRUNTIME_ARCHIVE_URL=<mirror or local copy>, "
      "pre-place the archive at ${_download_path}, or provide a native build "
      "under extra/onnxruntime (see extra/onnxruntime/readme.md).")
  endif()

  # Extract the archive
  file(MAKE_DIRECTORY "${_extract_dir}")
  file(REMOVE_RECURSE "${_install_dir}")
  set(_ort_tar_rc 0)
  if (_ort_kind STREQUAL "nuget")
    set(_ort_nuget_dir "${_extract_dir}/_ort_nuget")
    file(REMOVE_RECURSE "${_ort_nuget_dir}")
    file(MAKE_DIRECTORY "${_ort_nuget_dir}")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xf "${_ort_archive}" --format=zip
      WORKING_DIRECTORY "${_ort_nuget_dir}"
      RESULT_VARIABLE _ort_tar_rc
    )
    if (_ort_tar_rc EQUAL 0)
      # Lay the package out like the official tarball: include/ + lib/.
      file(MAKE_DIRECTORY "${_install_dir}/include" "${_install_dir}/lib")
      file(COPY "${_ort_nuget_dir}/build/native/include/"
           DESTINATION "${_install_dir}/include")
      file(GLOB _ort_rid_files "${_ort_nuget_dir}/runtimes/${_ort_rid}/native/*")
      if (NOT _ort_rid_files)
        message(FATAL_ERROR
          "[ONNXRuntime] ${_ort_archive} has no runtimes/${_ort_rid}/native payload")
      endif()
      file(COPY ${_ort_rid_files} DESTINATION "${_install_dir}/lib")
      file(REMOVE_RECURSE "${_ort_nuget_dir}")
    endif()
  elseif (_archive_name MATCHES "\\.zip$")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xzf "${_ort_archive}" --format=zip
      WORKING_DIRECTORY "${_extract_dir}"
      RESULT_VARIABLE _ort_tar_rc
    )
  else()
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xzf "${_ort_archive}"
      WORKING_DIRECTORY "${_extract_dir}"
      RESULT_VARIABLE _ort_tar_rc
    )
  endif()
  if (NOT _ort_tar_rc EQUAL 0)
    file(REMOVE "${_ort_archive}")
    message(FATAL_ERROR
      "[ONNXRuntime] Failed to extract ${_ort_archive} (exit ${_ort_tar_rc}). "
      "Removed the archive; re-run cmake to download it again.")
  endif()

  # Rename the extracted directory to a fixed path
  if (_ort_kind STREQUAL "tarball")
    file(GLOB _dirs "${_extract_dir}/onnxruntime-*")
    list(LENGTH _dirs _ort_dirs_len)
    if (_ort_dirs_len EQUAL 0)
      message(FATAL_ERROR
        "[ONNXRuntime] ${_ort_archive} does not contain an onnxruntime-* directory")
    endif()
    list(GET _dirs 0 _extracted_dir)
    file(RENAME "${_extracted_dir}" "${_install_dir}")
  endif()
endif()

# The NuGet payload ships a bare libonnxruntime.so whose SONAME is
# libonnxruntime.so.1. Rebuild the tarball layout (versioned file + SONAME
# symlinks) so the DT_NEEDED name resolves at run time.
if (_os STREQUAL "linux" AND
    EXISTS "${_install_dir}/lib/libonnxruntime.so" AND
    NOT IS_SYMLINK "${_install_dir}/lib/libonnxruntime.so")
  set(_ort_soversion "${ONNXRUNTIME_VERSION}")
  if (_ort_soversion MATCHES "^([0-9]+)")
    set(_ort_soversion "${CMAKE_MATCH_1}")
  endif()
  set(_ort_versioned_name "libonnxruntime.so.${ONNXRUNTIME_VERSION}")
  if (NOT EXISTS "${_install_dir}/lib/${_ort_versioned_name}")
    file(RENAME "${_install_dir}/lib/libonnxruntime.so"
         "${_install_dir}/lib/${_ort_versioned_name}")
  else()
    file(REMOVE "${_install_dir}/lib/libonnxruntime.so")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
    "${_ort_versioned_name}"
    "${_install_dir}/lib/libonnxruntime.so.${_ort_soversion}")
  execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
    "libonnxruntime.so.${_ort_soversion}"
    "${_install_dir}/lib/libonnxruntime.so")
  message(STATUS
    "[ONNXRuntime] Normalized ${_install_dir}/lib to the versioned layout "
    "(libonnxruntime.so -> libonnxruntime.so.${_ort_soversion} -> "
    "${_ort_versioned_name})")
endif()

# Register the ONNX Runtime include and lib paths
set(ONNXRUNTIME_FOUND TRUE)
set(ONNXRUNTIME_INCLUDE_DIRS "${_install_dir}/include")
set(ONNXRUNTIME_LIB_DIR "${_install_dir}/lib/")

if (_os STREQUAL "win")
  set(ONNXRUNTIME_LIBRARY "${_install_dir}/lib/onnxruntime.lib")
elseif(APPLE)
  set(ONNXRUNTIME_LIBRARY "${_install_dir}/lib/libonnxruntime.dylib")
else()
  set(ONNXRUNTIME_LIBRARY "${_install_dir}/lib/libonnxruntime.so")
endif()
set(ONNXRUNTIME_LIBRARIES ${ONNXRUNTIME_LIBRARY})

if (NOT EXISTS "${ONNXRUNTIME_LIBRARY}")
  message(FATAL_ERROR
    "[ONNXRuntime] ${_install_dir} is incomplete: ${ONNXRUNTIME_LIBRARY} is "
    "missing. Delete ${_download_path} and ${_install_dir}, then re-run cmake.")
endif()

_ort_finalize()