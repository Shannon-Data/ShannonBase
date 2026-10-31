# ================================================
# ONNXRuntimeDispatch.cmake
#
# Builds the ort_dispatch shim: a very small shared library that keeps the
# link-time name libonnxruntime.so.1 but dlopen()s the best available backend
# at process start. Included from FindONNXRuntime.cmake so that every
# resolution path runs it.
#
#   ONNXRUNTIME_DISPATCH = AUTO (default) | ON | OFF
#
# AUTO turns the shim on only when the package actually ships several
# backends (per-CPU tiers or an extra provider package); a single flat
# libonnxruntime.so is linked directly, exactly as before.
# ================================================
include_guard(GLOBAL)

if (NOT DEFINED ONNXRUNTIME_DISPATCH)
  set(ONNXRUNTIME_DISPATCH "AUTO")
endif()

if (TARGET shannon_ort_dispatch)
  # Second find_package() in the same tree: just re-point the link libraries.
  set(ONNXRUNTIME_DISPATCH_ENABLED TRUE)
  set(ONNXRUNTIME_LIBRARIES shannon_ort_dispatch)
  return()
endif()

set(ONNXRUNTIME_DISPATCH_ENABLED FALSE)

set(_ort_dispatch_wanted FALSE)
if (ONNXRUNTIME_DISPATCH STREQUAL "ON")
  set(_ort_dispatch_wanted TRUE)
elseif(ONNXRUNTIME_DISPATCH STREQUAL "AUTO" AND ONNXRUNTIME_LIB_DIR)
  file(GLOB _ort_backend_subdirs
       "${ONNXRUNTIME_LIB_DIR}/glibc-hwcaps"
       "${ONNXRUNTIME_LIB_DIR}/x86_64"
       "${ONNXRUNTIME_LIB_DIR}/aarch64"
       "${ONNXRUNTIME_LIB_DIR}/providers"
       "${ONNXRUNTIME_LIB_DIR}/backends")
  if (_ort_backend_subdirs)
    set(_ort_dispatch_wanted TRUE)
  endif()
  unset(_ort_backend_subdirs)
endif()

if (NOT _ort_dispatch_wanted)
  message(STATUS
    "[ONNXRuntime] Single flat library, linking directly "
    "(set ONNXRUNTIME_DISPATCH=ON to force the dispatch shim)")
  return()
endif()

if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(_ort_arch "aarch64")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
  set(_ort_arch "x86_64")
else()
  set(_ort_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()

add_library(shannon_ort_dispatch SHARED
  "${CMAKE_CURRENT_LIST_DIR}/../extra/onnxruntime/ort_dispatch.cpp")

set_target_properties(shannon_ort_dispatch PROPERTIES
  OUTPUT_NAME "onnxruntime"
  PREFIX "lib"
  VERSION "1.0.0"
  SOVERSION "1"
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/library_output_directory")

target_include_directories(shannon_ort_dispatch PRIVATE
  ${ONNXRUNTIME_INCLUDE_DIRS})
target_link_libraries(shannon_ort_dispatch PRIVATE ${CMAKE_DL_LIBS})

# Where the backends lived at configure time, used as a fallback lookup when
# the shim runs from the build tree instead of an installed tree.
target_compile_definitions(shannon_ort_dispatch PRIVATE
  SHANNON_ORT_BACKEND_DIR="${ONNXRUNTIME_LIB_DIR}"
  SHANNON_ORT_ARCH="${_ort_arch}")

set(ONNXRUNTIME_INSTALL_LIB_SUBDIR "backends/${_ort_arch}")

# The shim replaces the real library at link time; the real one is still
# installed next to it and dlopen()ed at runtime.
set(ONNXRUNTIME_REAL_LIBRARIES ${ONNXRUNTIME_LIBRARIES})
set(ONNXRUNTIME_LIBRARIES shannon_ort_dispatch)
set(ONNXRUNTIME_DISPATCH_ENABLED TRUE)

message(STATUS
  "[ONNXRuntime] Dispatch shim enabled: libonnxruntime.so.1 -> "
  "(${_ort_arch} tier | GPU provider) -> baseline, backends in "
  "lib/onnxruntime/${ONNXRUNTIME_INSTALL_LIB_SUBDIR}")
