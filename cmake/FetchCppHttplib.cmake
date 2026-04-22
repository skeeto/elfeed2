# Resolve cpp-httplib (header-only as we use it) into a `cpp-httplib`
# INTERFACE target carrying the include path.
#
# DEPS=LOCAL — find_path for httplib.h; Debian's libcpp-httplib-dev
#              installs to /usr/include. We don't need the system
#              package's compiled split-mode .so since we keep
#              everything header-only and just toggle MBEDTLS_SUPPORT
#              at our compile site.
# DEPS=FETCH — pull pinned source; expose its include directory.
#              cpp-httplib's own CMake offers a pre-built target but
#              header-only use is simpler and equivalent for us.

if(DEPS STREQUAL "LOCAL")
  find_path(HTTPLIB_INCLUDE_DIR httplib.h REQUIRED)
  if(NOT TARGET cpp-httplib)
    add_library(cpp-httplib INTERFACE)
    target_include_directories(cpp-httplib INTERFACE
      "${HTTPLIB_INCLUDE_DIR}")
  endif()
  return()
endif()

include(FetchContent)
fetch_content_bundled(cpp-httplib cpp-httplib)

# SOURCE_SUBDIR pointing at a nonexistent directory tells
# FetchContent_MakeAvailable to populate the source but skip
# add_subdirectory — we don't want cpp-httplib's CMakeLists to run
# (it'd find_package(OpenSSL) on its own and build a split-mode
# .so/.a we don't use). Modern alternative to the deprecated
# FetchContent_Populate call.
FetchContent_Declare(
  cpp-httplib
  URL           https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.43.0.tar.gz
  URL_HASH      SHA256=28b88a7f0751c762cc9c70c91db0d6953554c40f49285907e716dd7943a824a5
  SOURCE_SUBDIR _no_cmake_
)
FetchContent_MakeAvailable(cpp-httplib)

if(NOT TARGET cpp-httplib)
  add_library(cpp-httplib INTERFACE)
  target_include_directories(cpp-httplib INTERFACE
    "${cpp-httplib_SOURCE_DIR}")
endif()
