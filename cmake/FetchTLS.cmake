# Resolve the TLS backend cpp-httplib uses.
#
# Both modes produce one INTERFACE target named `tls` carrying the
# right link libraries AND the right CPPHTTPLIB_*_SUPPORT compile
# define, so cpp-httplib's header gates compile in.
#
# DEPS=LOCAL — OpenSSL. Universally available wherever cpp-httplib
#              is packaged, and the OpenSSL-specific cpp-httplib
#              API has been stable across versions. mbedTLS support
#              in distro cpp-httplib (e.g. Debian's 0.18.7) has API
#              gaps — methods like set_ca_cert_path were gated on
#              CPPHTTPLIB_OPENSSL_SUPPORT only until later versions.
# DEPS=FETCH — mbedTLS, pinned 3.6.2. We chose mbedTLS over OpenSSL
#              for the bundled build because it's smaller, simpler,
#              and avoids dragging an OpenSSL dep into self-
#              contained Windows / macOS bundles.

if(DEPS STREQUAL "LOCAL")
  find_package(OpenSSL REQUIRED)
  if(NOT TARGET tls)
    add_library(tls INTERFACE)
    target_link_libraries(tls INTERFACE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(tls INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
  endif()
  return()
endif()

# FETCH path: build and use mbedTLS.
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

if(NOT TARGET tls)
  add_library(tls INTERFACE)
  target_link_libraries(tls INTERFACE mbedtls mbedx509 mbedcrypto)
  target_compile_definitions(tls INTERFACE CPPHTTPLIB_MBEDTLS_SUPPORT)
endif()
