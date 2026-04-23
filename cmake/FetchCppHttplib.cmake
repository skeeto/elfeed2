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
  # Distros (Debian, Ubuntu) ship cpp-httplib in split mode: the
  # header declares the API, the implementation lives in a
  # separate libhttplib.so we have to link. Debian names it
  # `libcpp-httplib.so`, upstream's CMakeLists names it
  # `libhttplib.so`; check both. find_library returns NOTFOUND
  # on systems that only ship header-only — that's also valid;
  # in that case we mark CPPHTTPLIB_HEADER_ONLY so the header
  # inlines its definitions in our TU.
  find_library(HTTPLIB_LIBRARY NAMES cpp-httplib httplib)
  if(NOT TARGET cpp-httplib)
    add_library(cpp-httplib INTERFACE)
    target_include_directories(cpp-httplib INTERFACE
      "${HTTPLIB_INCLUDE_DIR}")
    if(HTTPLIB_LIBRARY)
      target_link_libraries(cpp-httplib INTERFACE
        "${HTTPLIB_LIBRARY}")
    else()
      target_compile_definitions(cpp-httplib INTERFACE
        CPPHTTPLIB_HEADER_ONLY)
    endif()
  endif()
else()
  include(FetchContent)
  fetch_content_bundled(cpp-httplib cpp-httplib)

  # SOURCE_SUBDIR pointing at a nonexistent directory tells
  # FetchContent_MakeAvailable to populate the source but skip
  # add_subdirectory — we don't want cpp-httplib's CMakeLists to
  # run (it'd find_package(OpenSSL) on its own and build a
  # split-mode .so/.a we don't use). Modern alternative to the
  # deprecated FetchContent_Populate call.
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
endif()

# Hook zlib into cpp-httplib on both LOCAL and FETCH. Many
# real-world feed servers send gzipped content regardless of
# whether Accept-Encoding asked for it (Cloudflare, some nginx
# configs). Without decompression we get gzip bytes back and
# pugixml fails with "No document element found." With
# CPPHTTPLIB_ZLIB_SUPPORT the header-only code auto-adds
# Accept-Encoding: gzip, deflate and auto-decompresses the
# response before handing us the body. ZLIB is part of the
# base system on every platform we target (macOS, every Linux
# distro, mingw-w64's runtime), so using the system package
# keeps the build portable without pulling more third-party
# source.
find_package(ZLIB REQUIRED)
target_link_libraries(cpp-httplib INTERFACE ZLIB::ZLIB)
target_compile_definitions(cpp-httplib INTERFACE
  CPPHTTPLIB_ZLIB_SUPPORT)
