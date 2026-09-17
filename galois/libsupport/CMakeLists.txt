add_library(galois_support STATIC)
add_library(Galois::support ALIAS galois_support)
set_target_properties(galois_support PROPERTIES EXPORT_NAME support)
add_dependencies(lib galois_support)

set(sources
        src/GetEnv.cpp
        src/Logging.cpp
)

target_sources(galois_support PRIVATE ${sources})

target_include_directories(galois_support PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

include(FetchContent)
# Prefer an installed fmt; fmtlib renamed its default branch from master to
# main, so the original unpinned fetch no longer resolves. The tag is a real
# release rather than a moving branch, so a fetch stays reproducible.
FetchContent_Declare(fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG 10.2.1
  FIND_PACKAGE_ARGS NAMES fmt
)
FetchContent_MakeAvailable(fmt)
target_link_libraries(galois_support fmt::fmt)

add_subdirectory(test)

install(
  DIRECTORY include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  COMPONENT dev
  FILES_MATCHING PATTERN "*.h"
)

install(
  TARGETS galois_support
  EXPORT GaloisTargets
  LIBRARY
    DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    COMPONENT shlib
  ARCHIVE
    DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    COMPONENT lib
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
