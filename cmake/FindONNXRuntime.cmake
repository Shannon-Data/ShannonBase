# ================================================
# FindONNXRuntime.cmake
# A CMake module to find or download ONNX Runtime
# Supports: Linux, macOS, Windows
# Supports: CPU or GPU builds (based on USE_GPU env)
# ================================================

# Set default ONNX Runtime version
if (NOT ONNXRUNTIME_VERSION)
  set(ONNXRUNTIME_VERSION "1.17.0")
endif()

# Automatically determine backend: cpu or gpu
if (DEFINED ENV{USE_GPU})
  message(STATUS "[ONNXRuntime] USE_GPU is set in the environment. Using GPU backend.")
  set(ONNXRUNTIME_BACKEND "gpu")
else()
  set(ONNXRUNTIME_BACKEND "cpu")
endif()

# Detect platform
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_os "linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(_os "osx")
elseif(CMAKE_SYSTEM_NAME MATCHES "Windows")
  set(_os "win")
else()
  message(FATAL_ERROR "[ONNXRuntime] Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

# Determine archive filename based on platform and backend
if (_os STREQUAL "linux")
  if (ONNXRUNTIME_BACKEND STREQUAL "cpu")
    set(_archive_name "onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz")
  elseif(ONNXRUNTIME_BACKEND STREQUAL "gpu")
    set(_archive_name "onnxruntime-linux-x64-gpu-${ONNXRUNTIME_VERSION}.tgz")
  endif()
elseif(_os STREQUAL "osx")
  set(_archive_name "onnxruntime-osx-universal2-${ONNXRUNTIME_VERSION}.tgz")
elseif(_os STREQUAL "win")
  if (ONNXRUNTIME_BACKEND STREQUAL "cpu")
    set(_archive_name "onnxruntime-win-x64-${ONNXRUNTIME_VERSION}.zip")
  elseif(ONNXRUNTIME_BACKEND STREQUAL "gpu")
    set(_archive_name "onnxruntime-win-x64-gpu-${ONNXRUNTIME_VERSION}.zip")
  endif()
endif()

# Download and install locations
set(_onnx_url "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/${_archive_name}")
set(_download_path "${CMAKE_BINARY_DIR}/${_archive_name}")
set(_extract_dir "${CMAKE_BINARY_DIR}/_deps")
set(_install_dir "${_extract_dir}/onnxruntime")

# Try system-wide ONNX Runtime installation first
find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_c_api.h PATHS /usr/include /usr/local/include /opt/include)
find_library(ONNXRUNTIME_LIBRARY onnxruntime PATHS /usr/lib /usr/local/lib /opt/lib)

if (ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
  set(ONNXRUNTIME_FOUND TRUE)
  set(ONNXRUNTIME_INCLUDE_DIRS ${ONNXRUNTIME_INCLUDE_DIR})
  set(ONNXRUNTIME_LIBRARIES ${ONNXRUNTIME_LIBRARY})
  message(STATUS "[ONNXRuntime] Found system installation.")
  return()
endif()

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

# Register the ONNX Runtime include and lib paths
set(ONNXRUNTIME_FOUND TRUE)
set(ONNXRUNTIME_INCLUDE_DIRS "${_install_dir}/include")
if (_os STREQUAL "win")
  set(ONNXRUNTIME_LIBRARY "${_install_dir}/lib/onnxruntime.lib")
else()
  set(ONNXRUNTIME_LIBRARY "${_install_dir}/lib/libonnxruntime.so")
endif()
set(ONNXRUNTIME_LIBRARIES ${ONNXRUNTIME_LIBRARY})

