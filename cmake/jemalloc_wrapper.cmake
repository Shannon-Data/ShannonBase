#jemalloc wrapper cmake file. 
set(JEMALLOC_SRC_DIR ${CMAKE_SOURCE_DIR}/extra/jemalloc)
set(JEMALLOC_INSTALL_DIR ${CMAKE_BINARY_DIR}/jemalloc-bundled)
file(MAKE_DIRECTORY ${JEMALLOC_INSTALL_DIR})

set(JEMALLOC_LIB_PATH ${JEMALLOC_INSTALL_DIR}/lib/libjemalloc.a)

add_custom_command(
  OUTPUT ${JEMALLOC_LIB_PATH}
  COMMAND ${CMAKE_SOURCE_DIR}/extra/jemalloc/autogen.sh
  COMMAND ${CMAKE_SOURCE_DIR}/extra/jemalloc/configure --with-pic --disable-shared --prefix=${JEMALLOC_INSTALL_DIR}
  COMMAND make -j && make install
  WORKING_DIRECTORY ${JEMALLOC_SRC_DIR}
  COMMENT "Building jemalloc from submodule"
  VERBATIM
)

add_custom_target(build_jemalloc ALL DEPENDS ${JEMALLOC_LIB_PATH})

add_library(jemalloc_pic STATIC IMPORTED GLOBAL)
add_dependencies(jemalloc_pic build_jemalloc)

set_target_properties(jemalloc_pic PROPERTIES
  IMPORTED_LOCATION ${JEMALLOC_LIB_PATH}
  INTERFACE_INCLUDE_DIRECTORIES ${JEMALLOC_INSTALL_DIR}/include
)

list(APPEND EXTRA_LIBS jemalloc_pic)

