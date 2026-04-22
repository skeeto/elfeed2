# Resolve mbedTLS into mbedtls / mbedx509 / mbedcrypto targets.
#
# DEPS=LOCAL — Debian's libmbedtls-dev installs the three shared libs
#              under standard names; find them and create matching
#              IMPORTED targets so downstream link lines don't change.
# DEPS=FETCH — pull the pinned 3.6.2 release and let its own CMake
#              build all three as static libs.

if(DEPS STREQUAL "LOCAL")
  find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h REQUIRED)
  find_library(MBEDTLS_LIB     mbedtls    REQUIRED)
  find_library(MBEDX509_LIB    mbedx509   REQUIRED)
  find_library(MBEDCRYPTO_LIB  mbedcrypto REQUIRED)

  # Refuse mbedTLS >= 4: the 4.x release moved ctr_drbg.h into a
  # `private/` subdir and otherwise broke API compat. cpp-httplib
  # v0.43 only knows the 2.x/3.x layout, so a build against v4
  # would fail mid-compile with a confusing missing-header error
  # from inside httplib.h. Catch it here with a clear message
  # instead. Debian / Fedora / most distros ship 3.x today.
  if(EXISTS "${MBEDTLS_INCLUDE_DIR}/mbedtls/build_info.h")
    file(STRINGS "${MBEDTLS_INCLUDE_DIR}/mbedtls/build_info.h"
         _ver_major REGEX "MBEDTLS_VERSION_MAJOR[ \t]+[0-9]+")
    if(_ver_major MATCHES "MBEDTLS_VERSION_MAJOR[ \t]+([0-9]+)")
      if(CMAKE_MATCH_1 GREATER_EQUAL 4)
        message(FATAL_ERROR
          "system mbedTLS ${CMAKE_MATCH_1}.x is not compatible "
          "with cpp-httplib v0.43 (which targets mbedTLS 2.x/3.x). "
          "Install mbedTLS 3.x, or use -DDEPS=FETCH (which pulls "
          "the pinned 3.6.2).")
      endif()
    endif()
  endif()

  foreach(_pair
            "mbedtls;${MBEDTLS_LIB}"
            "mbedx509;${MBEDX509_LIB}"
            "mbedcrypto;${MBEDCRYPTO_LIB}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _path)
    if(NOT TARGET ${_name})
      add_library(${_name} UNKNOWN IMPORTED)
      set_target_properties(${_name} PROPERTIES
        IMPORTED_LOCATION             "${_path}"
        INTERFACE_INCLUDE_DIRECTORIES "${MBEDTLS_INCLUDE_DIR}")
    endif()
  endforeach()
  return()
endif()

include(FetchContent)
fetch_content_bundled(mbedtls mbedtls)

set(ENABLE_PROGRAMS         OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING          OFF CACHE BOOL "" FORCE)
set(MBEDTLS_AS_SUBPROJECT   ON  CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS  OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  mbedtls
  URL      https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
  URL_HASH SHA256=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
)
FetchContent_MakeAvailable(mbedtls)
