#jemalloc wrapper cmake file. 
# jemalloc_wrapper.cmake

include(ExternalProject)

set(JEMALLOC_INSTALL_DIR ${CMAKE_BINARY_DIR}/jemalloc-bundled)
set(JEMALLOC_LIB_PATH ${JEMALLOC_INSTALL_DIR}/lib/libjemalloc.a)
set(JEMALLOC_INCLUDE_DIR ${JEMALLOC_INSTALL_DIR}/include)

ExternalProject_Add(jemalloc_ext
  PREFIX ${CMAKE_BINARY_DIR}/_deps/jemalloc
  SOURCE_DIR ${CMAKE_SOURCE_DIR}/extra/jemalloc
  PATCH_COMMAND ${CMAKE_SOURCE_DIR}/extra/jemalloc/autogen.sh
  CONFIGURE_COMMAND ${CMAKE_SOURCE_DIR}/extra/jemalloc/configure --with-pic --disable-shared --prefix=${JEMALLOC_INSTALL_DIR}
  BUILD_COMMAND make -j
  INSTALL_COMMAND make install
  BUILD_IN_SOURCE 1
  WORKING_DIRECTORY ${JEMALLOC_SRC_DIR}
  BUILD_BYPRODUCTS ${JEMALLOC_LIB_PATH}
  LOG_CONFIGURE ON
  LOG_BUILD ON
  LOG_INSTALL ON
)

add_library(jemalloc_pic STATIC IMPORTED GLOBAL)
add_dependencies(jemalloc_pic jemalloc_ext)

set_target_properties(jemalloc_pic PROPERTIES
  IMPORTED_LOCATION ${JEMALLOC_LIB_PATH}
  #INTERFACE_INCLUDE_DIRECTORIES ${JEMALLOC_INCLUDE_DIR}
)

set(JEMALLOC_INCLUDE_HINT "${JEMALLOC_INSTALL_DIR}/include" CACHE INTERNAL "jemalloc include dir")

list(APPEND EXTRA_LIBS jemalloc_pic)
