# ================================================
# FindONNXRuntime.cmake
# A CMake module to find or download ONNX Runtime
# Supports: Linux, macOS, Windows
# Supports: CPU or GPU builds (based on USE_GPU env)
# ================================================

# Set default ONNX Runtime version
if (NOT ONNXRUNTIME_VERSION)
  set(ONNXRUNTIME_VERSION "1.22.0")
endif()

set (ONNXRUNTIME_LIB_VERSION "1")

# Automatically determine backend: cpu or gpu
if (DEFINED ENV{USE_GPU})
  message(STATUS "[ONNXRuntime] USE_GPU is set in the environment. Using GPU backend.")
  set(ONNXRUNTIME_BACKEND "gpu")
else()
  set(ONNXRUNTIME_BACKEND "cpu")
endif()

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

# Try system-wide ONNX Runtime installation first
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
  message(STATUS "[ONNXRuntime] Found system installation.")
  return()
endif()

# Check if already downloaded and extracted
if (EXISTS "${_install_dir}")
  message(STATUS "[ONNXRuntime] Found existing installation at ${_install_dir}")
else()
  # If not found, download the binary archive
  message(STATUS "[ONNXRuntime] System installation not found. Downloading: ${_onnx_url}")

  file(DOWNLOAD ${_onnx_url} ${_download_path}
       SHOW_PROGRESS STATUS _dl_status)

  list(GET _dl_status 0 _dl_code)
  if (NOT _dl_code EQUAL 0)
    message(FATAL_ERROR "[ONNXRuntime] Failed to download: ${_onnx_url}")
  endif()

  # Extract the archive
  file(MAKE_DIRECTORY ${_extract_dir})
  if (_archive_name MATCHES ".zip$")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xzf ${_download_path} --format=zip
      WORKING_DIRECTORY ${_extract_dir}
    )
  else()
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xzf ${_download_path}
      WORKING_DIRECTORY ${_extract_dir}
    )
  endif()

  # Rename the extracted directory to a fixed path
  file(GLOB _dirs "${_extract_dir}/onnxruntime-*")
  list(GET _dirs 0 _extracted_dir)
  file(RENAME ${_extracted_dir} ${_install_dir})
endif()

# Register the ONNX Runtime include and lib paths
set(ONNXRUNTIME_FOUND TRUE)
set(ONNXRUNTIME_INCLUDE_DIRS "${_install_dir}/include")
set(ONNXRUNTIME_LIB_DIR "${_install_dir}/lib/")

if (_os STREQUAL "win")
  set(ONNXRUNTIME_LIBRARY "${_install_dir}/lib/onnxruntime.lib.${ONNXRUNTIME_LIB_VERSION}")
elseif(APPLE)
  set(ONNXRUNTIME_LIBRARY "${_install_dir}/lib/libonnxruntime.dylib.${ONNXRUNTIME_LIB_VERSION}")
else()
  set(ONNXRUNTIME_LIBRARY "${_install_dir}/lib/libonnxruntime.so.${ONNXRUNTIME_LIB_VERSION}")
endif()
set(ONNXRUNTIME_LIBRARIES ${ONNXRUNTIME_LIBRARY})