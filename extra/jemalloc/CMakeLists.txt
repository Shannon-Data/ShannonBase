#keep same with ShannonBase Cmake version req.
cmake_minimum_required(VERSION 3.5.1)

set(JEMALLOC_INSTALL_DIR ${CMAKE_BINARY_DIR}/jemalloc-bundled)
file(MAKE_DIRECTORY ${JEMALLOC_INSTALL_DIR})

#jemalloc static lib path
set(JEMALLOC_LIB_PATH ${JEMALLOC_INSTALL_DIR}/lib/libjemalloc.a)

#build up jemalloc： configure + make + make install
add_custom_command(
  OUTPUT ${JEMALLOC_LIB_PATH}
  COMMAND ./autogen.sh
  COMMAND ./configure --with-pic --disable-shared --prefix=${JEMALLOC_INSTALL_DIR}
  COMMAND make -j && make install
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Building jemalloc from source"
  VERBATIM
)

#build jemalloc_pic
add_custom_target(build_jemalloc ALL DEPENDS ${JEMALLOC_LIB_PATH})

add_library(jemalloc_pic STATIC IMPORTED GLOBAL)
add_dependencies(jemalloc_pic build_jemalloc)

set_target_properties(jemalloc_pic PROPERTIES
  IMPORTED_LOCATION ${JEMALLOC_LIB_PATH}
  INTERFACE_INCLUDE_DIRECTORIES ${JEMALLOC_INSTALL_DIR}/include
)

