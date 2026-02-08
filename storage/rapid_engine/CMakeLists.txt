# Copyright (c) 2006, 2019, Oracle and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is also distributed with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have included with MySQL.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

# Shannon Rapid Engine CMakeLists.txt

MSVC_CPPCHECK_DISABLE()

include(${CMAKE_SOURCE_DIR}/storage/innobase/innodb.cmake)

ADD_DEFINITIONS(-DLOG_SUBSYSTEM_TAG="Rapid")

FIND_PACKAGE(ONNXRuntime REQUIRED)

MY_CHECK_CXX_COMPILER_WARNING("-Wextra-semi" HAS_EXTRA_SEMI_WARNING) #for ONNXRuntime extra ‘;’ after in-class function definition [-Werror=extra-semi]
MY_CHECK_CXX_COMPILER_WARNING("-Wno-extra-semi" HAS_NO_EXTRA_SEMI_WARNING)

# --- Explicit InnoDB include dirs ---
SET(INNOBASE_INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/storage/innobase         # For "ut/ut.h", "sync0/..." etc.
  ${CMAKE_SOURCE_DIR}/storage/innobase/include # For other InnoDB headers
)

# --- Shannon Rapid include dirs ---
SET(SHANNON_RAPID_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}                    # Rapid source dir
  ${CMAKE_SOURCE_DIR}/include                    # global include
  ${CMAKE_SOURCE_DIR}/sql                        # SQL headers
  ${CMAKE_SOURCE_DIR}/extra/lightgbm/include     # LightGBM
)

LIST(APPEND SHANNON_RAPID_INCLUDE_DIRS
    ${ONNXRUNTIME_INCLUDE_DIRS}
)

# --- Source files ---
SET(SHANNON_RAPID_COMPRESS_SOURCES
  compress/algorithms.cpp
  compress/dictionary/dictionary.cpp
)

SET(SHANNON_IMCS_SOURCES
   imcs/table.cpp
   imcs/table0view.cpp
   imcs/imcs.cpp
   imcs/imcu.cpp
   imcs/cu.cpp
   imcs/col0stats.cpp
   imcs/predicate.cpp
   imcs/varlen0data.cpp
   imcs/worker.cpp
   imcs/index/art/art.cpp
   imcs/index/encoder.cpp
   imcs/row0row.cpp
   imcs/storage0index.cpp
   utils/utils.cpp
   utils/SIMD.cpp
   utils/memory_pool.cpp
   autopilot/loader.cpp
)

SET(SHANNON_POPULATE_SOURCES
    populate/log_populate.cpp
    populate/log_redolog.cpp
    populate/log_copyinfo.cpp
)

SET(SHANNON_TRX_SOURCES
    trx/transaction.cpp
)

SET(SHANNON_READER_SOURCES
    reader/csv_reader.cpp
)

# --- Lakehouse / Arrow / Parquet optional support ---
IF((LINUX OR WIN32) AND WITH_LAKEHOUSE STREQUAL "system")
    list(APPEND SHANNON_READER_SOURCES reader/parquet_reader.cpp)

    find_package(Arrow)
    IF(NOT Arrow_FOUND)
        IF(WIN32)
            MESSAGE(FATAL_ERROR
                "Apache Arrow not found!\n"
                "Please install it using one of the following methods:\n"
                "  - Using vcpkg: vcpkg install arrow\n"
                "  - Using conda: conda install -c conda-forge pyarrow\n"
                "  - Download pre-built binaries from: https://arrow.apache.org/install/\n"
                "  - Build from source: https://arrow.apache.org/docs/developers/cpp/windows.html\n"
                "Ensure that the library is in your PATH or set ARROW_ROOT to its install location."
            )
        ELSE()
            MESSAGE(FATAL_ERROR
                "Apache Arrow not found!\n"
                "Please install it using one of the following methods:\n"
                "  - On Ubuntu/Debian: sudo apt-get install libarrow-dev libparquet-dev\n"
                "  - On CentOS/RHEL: sudo yum install arrow-devel parquet-devel\n"
                "  - From source: https://arrow.apache.org/install/\n"
                "  - Using conda: conda install -c conda-forge pyarrow\n"
                "Ensure that the library is in your PATH or set ARROW_ROOT to its install location."
            )
        ENDIF()
    ENDIF()

    find_package(Parquet)
    IF(NOT Parquet_FOUND)
        IF(WIN32)
            MESSAGE(FATAL_ERROR
                "Apache Parquet not found!\n"
                "Please install it along with Arrow:\n"
                "  - Using vcpkg: vcpkg install arrow[parquet]\n"
                "  - Using conda: conda install -c conda-forge pyarrow\n"
                "  - Download pre-built binaries from: https://arrow.apache.org/install/\n"
                "  - Build from source: https://arrow.apache.org/docs/developers/cpp/windows.html\n"
                "Ensure that the library is in your PATH or set PARQUET_ROOT to its install location."
            )
        ELSE()
            MESSAGE(FATAL_ERROR
                "Apache Parquet not found!\n"
                "Please install it along with Arrow:\n"
                "  - On Ubuntu/Debian: sudo apt-get install libparquet-dev\n"
                "  - On CentOS/RHEL: sudo yum install parquet-devel\n"
                "  - From source: https://arrow.apache.org/install/\n"
                "  - Using conda: conda install -c conda-forge pyarrow\n"
                "Ensure that the library is in your PATH or set PARQUET_ROOT to its install location."
            )
        ENDIF()
    ENDIF()

    # Enable Lakehouse feature flag
    add_definitions(-DLAKEHOUSE_ENABLED)
ENDIF()

# --- Iterators and optimizer ---
SET(SHANNON_ITERATOR_SOURCES
    executor/iterators/iterator.cpp
    executor/iterators/aggregate_iterator.cpp
    executor/iterators/hash_join_iterator.cpp
    executor/iterators/table_scan_iterator.cpp
    ${SHANNON_READER_SOURCES}
)

SET(SHANNON_OPTIMIZE
    optimizer/optimizer.cpp
    optimizer/query_plan.cpp
    optimizer/path/access_path.cpp
    optimizer/rules/condition_pushdown.cpp
    optimizer/rules/join_reorder.cpp
    optimizer/rules/prune.cpp
    cost/cost.cpp
    statistics/statistics.cpp
)

SET(SHANNON_ML
    ml/ml.cpp
)

SET(SHANNON_PERSISTENCE
)

# --- All Rapid sources ---
SET(SHANNON_RAPID_SOURCES
    handler/ha_shannon_rapid.cc
    handler/ha_shannon_rapidpart.cc
    ${SHANNON_IMCS_SOURCES}
    ${SHANNON_TRX_SOURCES}
    ${SHANNON_ITERATOR_SOURCES}
    ${SHANNON_POPULATE_SOURCES}
    ${SHANNON_ML}
    ${SHANNON_OPTIMIZE}
    ${SHANNON_PERSISTENCE}
)

# --- Compress library ---
ADD_LIBRARY(rapid_compress STATIC ${SHANNON_RAPID_COMPRESS_SOURCES})
ADD_DEPENDENCIES(rapid_compress GenError)

# Configure compress library includes
# Note: UNIV_NO_ERR_MSGS is defined to reduce binary size
TARGET_INCLUDE_DIRECTORIES(rapid_compress
    PRIVATE
        ${INNOBASE_INCLUDE_DIRS}
        ${SHANNON_RAPID_INCLUDE_DIRS}
)

# --- NUMA library support ---
UNSET(NUMA_LIBRARY)
IF(HAVE_LIBNUMA)
  SET(NUMA_LIBRARY "numa")
ENDIF()

# --- Libraries for plugin ---
SET(SHANNON_RAPID_LIBRARIES
    rapid_compress
    sql_dd
    ${ZLIB_LIBRARY}
    ${LZ4_LIBRARY}
    ${NUMA_LIBRARY}
    _lightgbm
    gomp
    ${ONNXRUNTIME_LIBRARIES}
)

# --- Add Lakehouse libraries if enabled ---
IF(WITH_LAKEHOUSE)
    list(APPEND SHANNON_RAPID_LIBRARIES Arrow::arrow_shared Parquet::parquet_shared)
ENDIF()

# --- Add MySQL plugin ---
MYSQL_ADD_PLUGIN(shannon_rapid
  ${SHANNON_RAPID_SOURCES}
  STORAGE_ENGINE
  MANDATORY
  MODULE_OUTPUT_NAME ha_rapid
  LINK_LIBRARIES ${SHANNON_RAPID_LIBRARIES}
)

# --- Target compile definitions and options ---
# Note: MUTEX_FUTEX/MUTEX_SYS is already defined by innodb.cmake
# We only need to add Rapid-specific definitions here

# PUBLIC: Suppress specific warnings for this target and dependents
TARGET_COMPILE_OPTIONS(shannon_rapid PUBLIC -Wno-error=unused-parameter)
IF(HAS_NO_EXTRA_SEMI_WARNING)
    TARGET_COMPILE_OPTIONS(shannon_rapid PRIVATE "${HAS_NO_EXTRA_SEMI_WARNING}")
ENDIF()
# --- Include directories configuration ---
# PUBLIC: InnoDB headers are part of the API and needed by dependents (e.g., MySQL core)
# PRIVATE: Rapid internal headers are not exposed to dependents
TARGET_INCLUDE_DIRECTORIES(shannon_rapid
    PUBLIC
        ${INNOBASE_INCLUDE_DIRS}    # InnoDB headers (ut/ut.h, etc.)
    PRIVATE
        ${SHANNON_RAPID_INCLUDE_DIRS}  # Rapid internal headers
)

# --- Link directories ---
TARGET_LINK_DIRECTORIES(shannon_rapid PRIVATE ${ONNXRUNTIME_LIB_DIR})

IF(UNIX AND NOT APPLE)
    GET_TARGET_PROPERTY(SHANNON_RAPID_RPATH shannon_rapid INSTALL_RPATH)
    IF(NOT SHANNON_RAPID_RPATH)
        SET(SHANNON_RAPID_RPATH "")
    ENDIF()
    
    IF(ONNXRUNTIME_LIB_DIR)
        LIST(APPEND SHANNON_RAPID_RPATH "${ONNXRUNTIME_LIB_DIR}")
    ENDIF()
    
    SET_TARGET_PROPERTIES(shannon_rapid PROPERTIES INSTALL_RPATH "${SHANNON_RAPID_RPATH}")
    
    SET_TARGET_PROPERTIES(shannon_rapid PROPERTIES 
        BUILD_WITH_INSTALL_RPATH FALSE
        BUILD_RPATH "${ONNXRUNTIME_LIB_DIR};$ORIGIN/../lib"
    )
ENDIF()

# --- Windows-specific configuration for Lakehouse ---
IF(WITH_LAKEHOUSE AND WIN32)
    set_property(TARGET shannon_rapid PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    if(CMAKE_TOOLCHAIN_FILE)
        find_package(PkgConfig QUIET)
    endif()

    if(MSVC)
        target_link_libraries(shannon_rapid PRIVATE ws2_32)
    endif()
ENDIF()

# --- Compiler flags / platform tweaks ---
IF(MY_COMPILER_IS_GNU_OR_CLANG)
    # Apply to the target specifically, not globally
    TARGET_COMPILE_OPTIONS(shannon_rapid PRIVATE -Wno-cast-qual)
ENDIF()

# Avoid generating Hardware Capabilities due to crc32 instructions
IF(SOLARIS_INTEL)
  MY_CHECK_CXX_COMPILER_FLAG("-Wa,-nH" HAVE_WA_NH)
  IF(HAVE_WA_NH)
    TARGET_COMPILE_OPTIONS(shannon_rapid PRIVATE "-Wa,-nH")
  ENDIF()
ENDIF()

# On linux: /usr/include/stdio.h:#define BUFSIZ 8192
# On Solaris: /usr/include/iso/stdio_iso.h:#define BUFSIZ 1024
IF(SOLARIS AND MY_COMPILER_IS_GNU)
  TARGET_COMPILE_OPTIONS(shannon_rapid PRIVATE "-Wno-format-truncation")
ENDIF()

# Generated code with logical-op warnings
MY_CHECK_CXX_COMPILER_WARNING("-Wlogical-op" HAS_WARN_FLAG)
IF(HAS_WARN_FLAG)
  TARGET_COMPILE_OPTIONS(shannon_rapid PRIVATE "${HAS_WARN_FLAG}")
ENDIF()

MY_CHECK_CXX_COMPILER_WARNING("-Wcast-function-type" HAS_WARN_FLAG)
IF(HAS_WARN_FLAG)
  TARGET_COMPILE_OPTIONS(shannon_rapid PRIVATE "${HAS_WARN_FLAG}")
ENDIF()