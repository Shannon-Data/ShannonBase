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

# This is the CMakeLists for Shannon Rapid
INCLUDE(${CMAKE_SOURCE_DIR}/storage/innobase/innodb.cmake)

MSVC_CPPCHECK_DISABLE()

ADD_DEFINITIONS(-DLOG_SUBSYSTEM_TAG="Rapid")

# TODO: Fix up all non-canonical include paths and remove this.
INCLUDE_DIRECTORIES(
  ${CMAKE_SOURCE_DIR}/sql
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/storage/innobase
  ${CMAKE_SOURCE_DIR}/storage/rapid_engine
)

#FOR AUTO-ML MODELS
INCLUDE_DIRECTORIES( ${CMAKE_SOURCE_DIR}/extra/lightgbm/include)
#LINK_DIRECTORIES(${CMAKE_SOURCE_DIR}/storage/rapid_engine/ml/LightGBM/lib/)

# Conflicting YYSTYPE, because we have multiple Bison grammars.
# WL#11100 Migrate to Bison 3.x should fix this.
#
# STRING(REPLACE "-flto" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
# STRING(REPLACE "-flto" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")


SET(SHANNON_RAPID_COMPRESS_SOURCES
  compress/algorithms.cpp
  compress/dictionary/dictionary.cpp
)
SET(SHANNON_IMCS_SOURCES
   imcs/table.cpp
   imcs/data_table.cpp
   imcs/imcs.cpp
   imcs/imcu.cpp
   imcs/cu.cpp
   imcs/chunk.cpp
   imcs/index/art/art.cpp
   imcs/index/encoder.cpp
   utils/utils.cpp
   utils/SIMD.cpp
   imcs/purge/purge.cpp
   autopilot/loader.cpp
)
SET(SHANNON_POPULATE_SOURCES
   populate/log_parser.cpp
   populate/populate.cpp
)
SET (SHANNON_TRX_SOURCES
    trx/transaction.cpp
    trx/readview.cpp
)

SET(SHANNON_READER_SOURCES
    reader/csv_reader.cpp
)

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

    add_definitions(-DLAKEHOUSE_ENABLED)

    IF(WIN32)
        set_property(TARGET ha_rapid PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

        if(CMAKE_TOOLCHAIN_FILE)
            find_package(PkgConfig QUIET)
        endif()

        if(MSVC)
            target_link_libraries(ha_rapid PRIVATE ws2_32)
        endif()
    ENDIF()
ENDIF()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

SET (SHANNON_ITERATOR_SOURCES
    executor/iterators/iterator.cpp
    executor/iterators/aggregate_iterator.cpp
    executor/iterators/hash_join_iterator.cpp
    executor/iterators/table_scan_iterator.cpp
    ${SHANNON_READER_SOURCES}
)
set (SHANNON_OPTIMIZE
    optimizer/optimizer.cpp
    optimizer/path/access_path.cpp
    optimizer/rules/const_fold_rule.cpp
    cost/cost.cpp
    statistics/statistics.cpp
)
set (SHANNON_ML
    ml/ml.cpp
)
set (SHANNON_PERSISTENCE
)
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

ADD_LIBRARY(rapid_compress STATIC ${SHANNON_RAPID_COMPRESS_SOURCES})
TARGET_COMPILE_DEFINITIONS(
  rapid_compress PRIVATE UNIV_LIBRARY UNIV_NO_ERR_MSGS)
ADD_DEPENDENCIES(rapid_compress GenError)

UNSET(NUMA_LIBRARY)
IF(HAVE_LIBNUMA)
  SET(NUMA_LIBRARY "numa")
ENDIF()

SET(SHANNON_RAPID_LIBRARIES sql_dd ${ZLIB_LIBRARY} ${LZ4_LIBRARY} ${NUMA_LIBRARY} _lightgbm gomp)

IF(WITH_LAKEHOUSE)
    SET(SHANNON_RAPID_LIBRARIES ${SHANNON_RAPID_LIBRARIES} INTERFACE Arrow::arrow_shared Parquet::parquet_shared)
ENDIF()

MYSQL_ADD_PLUGIN(shannon_rapid
  ${SHANNON_RAPID_SOURCES} ${SHANNON_RAPID_COMPRESS_SOURCES}
  STORAGE_ENGINE
  MANDATORY
  MODULE_OUTPUT_NAME ha_rapid
  LINK_LIBRARIES ${SHANNON_RAPID_LIBRARIES})

TARGET_LINK_DIRECTORIES(shannon_rapid PRIVATE ${ONNXRUNTIME_LIB_DIR})

# Avoid generating Hardware Capabilities due to crc32 instructions
IF(SOLARIS_INTEL)
  MY_CHECK_CXX_COMPILER_FLAG("-Wa,-nH" HAVE_WA_NH)
  IF(HAVE_WA_NH)
    ADD_COMPILE_FLAGS(
      COMPILE_FLAGS "-Wa,-nH"
    )
  ENDIF()
ENDIF()

# On linux: /usr/include/stdio.h:#define BUFSIZ 8192
# On Solaris: /usr/include/iso/stdio_iso.h:#define    BUFSIZ  1024
IF(SOLARIS AND MY_COMPILER_IS_GNU)
  ADD_COMPILE_FLAGS(
    COMPILE_FLAGS "-Wno-format-truncation"
    )
ENDIF()

# Generated code with logical-op warnings
MY_CHECK_CXX_COMPILER_WARNING("-Wlogical-op" HAS_WARN_FLAG)
IF(HAS_WARN_FLAG)
  ADD_COMPILE_FLAGS(
    COMPILE_FLAGS "${HAS_WARN_FLAG}")
ENDIF()
MY_CHECK_CXX_COMPILER_WARNING("-Wcast-function-type" HAS_WARN_FLAG)
IF(HAS_WARN_FLAG)
  ADD_COMPILE_FLAGS(
    COMPILE_FLAGS "${HAS_WARN_FLAG}")
ENDIF()
