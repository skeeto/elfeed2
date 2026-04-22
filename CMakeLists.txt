cmake_minimum_required(VERSION 3.25)
project(elfeed2 VERSION 0.1.0 LANGUAGES C CXX)

# Local CMake modules (helpers + per-dep Fetch<X>.cmake files).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FetchContentBundled)

# Where to obtain dependencies:
#   FETCH (default) — pull pinned versions via FetchContent. Hermetic
#                     and reproducible; the recommended path for end-
#                     user builds, the macOS bundle, and the mingw
#                     cross binary.
#   LOCAL           — find system-installed libraries via find_*.
#                     The right call for distribution packagers
#                     (Debian, Fedora, Homebrew, …) who want their
#                     own shared-lib versions.
#
# Both paths produce identically-named imported/built targets so the
# rest of this file doesn't branch on DEPS.
set(DEPS "FETCH" CACHE STRING
    "Where to obtain dependencies: FETCH or LOCAL")
set_property(CACHE DEPS PROPERTY STRINGS FETCH LOCAL)
if(NOT DEPS MATCHES "^(FETCH|LOCAL)$")
  message(FATAL_ERROR "DEPS must be FETCH or LOCAL (got '${DEPS}')")
endif()

# Route compiler invocations through ccache when available. Speeds up
# clean rebuilds (including wxWidgets and friends, which are a lot of
# C++ source). Turn off with -DELFEED2_USE_CCACHE=OFF.
option(ELFEED2_USE_CCACHE "Use ccache for the compiler launchers if available" ON)
if(ELFEED2_USE_CCACHE)
  find_program(CCACHE_PROGRAM ccache)
  if(CCACHE_PROGRAM)
    message(STATUS "ccache: ${CCACHE_PROGRAM}")
    # wxWidgets uses precompiled headers per-target (~600 of the
    # ~800 wx compile commands carry `-include cmake_pch.hxx`),
    # and ccache + GCC + PCH is brittle — every fresh build's PCH
    # has new internal timestamps, so without `pch_defines`
    # ccache misses on every TU that includes the PCH.
    # `time_macros` covers the same hazard for any source that
    # bakes __DATE__/__TIME__ into the output.
    # Wrapping via `cmake -E env` is portable; the env var is
    # then set in each ccache subprocess regardless of how the
    # build is invoked.
    set(CMAKE_C_COMPILER_LAUNCHER
        "${CMAKE_COMMAND}" "-E" "env"
        "CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime"
        "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER
        "${CMAKE_COMMAND}" "-E" "env"
        "CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime"
        "${CCACHE_PROGRAM}")
    # Tell FetchWxWidgets to disable wx's per-target PCH. PCH speeds
    # up cold wx builds by ~30%, but the .pch binary embeds a
    # timestamp nonce that ccache treats as an extra-file mismatch
    # — so without this every clean build's wx compiles miss
    # ccache. Off only when ccache is in play; pipeline / no-cache
    # builds keep PCH on for the cold-build speedup.
    set(ELFEED2_CCACHE_ACTIVE TRUE)
  else()
    message(STATUS "ccache: not found")
  endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMake 3.28+ runs a `-fdeps-format=p1689r5` module-dependency scan
# before every C++20 compile. We use no C++20 modules (only the
# language features), so the scan is pure overhead AND defeats
# ccache: each scan is a `-E` preprocess-only invocation, which
# ccache passes through as a miss by design. Turning the scan off
# both speeds up builds and lets ccache hit on the real compiles.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Pin C to gnu17 so wxWidgets' bundled C deps (zlib, libpng, libjpeg,
# libtiff, expat) build cleanly under GCC's newer C23 default.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# Per-dep fetch / find. Each Fetch<X>.cmake handles both DEPS modes
# and produces the same target names (sqlite3, pugixml, wx::core,
# wx::base, wx::html, wx::aui, mbedtls, mbedx509, mbedcrypto,
# cpp-httplib) so this file doesn't branch on DEPS.
include(FetchWxWidgets)

# Force UTF-8 source AND execution charsets on MSVC. By default
# MSVC picks the source charset based on the system locale, which
# on most Windows installs is NOT UTF-8 — so non-ASCII literals
# (em-dash, middle dot, etc.) in our sources would get
# reinterpreted at compile time and emerge as mojibake at run
# time. Placed AFTER FetchWxWidgets so wx's bundled C/C++ deps
# keep their own settings. GCC, Clang, and mingw already default
# to UTF-8 source/exec charsets.
if(MSVC)
  add_compile_options(/utf-8)
endif()

include(FetchSQLite)
include(FetchPugixml)

# TLS + cpp-httplib aren't needed on Windows — http_win.cpp uses
# WinHTTP and httplib.h is never included. FetchTLS picks the
# right backend per DEPS mode (OpenSSL for LOCAL, mbedTLS for
# FETCH) and exposes a `tls` interface target.
if(NOT WIN32)
  include(FetchTLS)
  include(FetchCppHttplib)
endif()

# --- Elfeed ---

set(elfeed_sources
  src/app.cpp
  src/main_frame.cpp
  src/entry_list.cpp
  src/entry_detail.cpp
  src/feeds_panel.cpp
  src/log_panel.cpp
  src/downloads_panel.cpp
  src/db.cpp
  src/feed.cpp
  src/filter.cpp
  src/html.cpp
  src/config.cpp
  src/fetch.cpp
  src/download.cpp
  src/image_cache.cpp
  src/data_uri_handler.cpp
  src/util.cpp
  src/elfeed_import.cpp
)
if(WIN32)
  # Configure the .rc from a template so PROJECT_VERSION (and its
  # MAJOR/MINOR/PATCH split) drives both the VERSIONINFO numeric
  # tuples and the StringFileInfo "FileVersion" / "ProductVersion"
  # display strings — no manual sync between CMakeLists and the rc.
  set(elfeed2_rc_in  "${CMAKE_CURRENT_SOURCE_DIR}/src/elfeed2.rc.in")
  set(elfeed2_rc_out "${CMAKE_CURRENT_BINARY_DIR}/src/elfeed2.rc")
  configure_file("${elfeed2_rc_in}" "${elfeed2_rc_out}" @ONLY)
  list(APPEND elfeed_sources src/http_win.cpp "${elfeed2_rc_out}")
  enable_language(RC)
else()
  list(APPEND elfeed_sources src/http_posix.cpp)
endif()

if(APPLE)
  # The .icns file ships inside the bundle's Resources directory and
  # is referenced via CFBundleIconFile. Mark it as a resource on the
  # target so CMake places it at the right path.
  list(APPEND elfeed_sources src/elfeed2.icns)
  set_source_files_properties(src/elfeed2.icns PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources")
endif()

add_executable(elfeed2 WIN32 MACOSX_BUNDLE ${elfeed_sources})

if(APPLE)
  # Ship an Info.plist — otherwise the bundle has no CFBundleIdentifier,
  # and NSOpenPanel / NSSavePanel fail to appear with "domain name
  # cannot be nil or empty" because they key their remembered state on
  # the bundle ID. CMake's built-in MacOSXBundleInfo.plist.in template
  # isn't always found under Ninja; provide our own so we're certain.
  set_target_properties(elfeed2 PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST           "${CMAKE_CURRENT_SOURCE_DIR}/src/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER       "com.nullprogram.elfeed2"
    # Icon file (no extension here — the plist template adds .icns).
    MACOSX_BUNDLE_ICON_FILE            "elfeed2"
    # CFBundleName is what macOS shows in the menu bar when this
    # app is active; capitalize for display. The internal binary
    # name (CFBundleExecutable) stays lowercase via the target
    # name "elfeed2", matching paths and SetAppName().
    MACOSX_BUNDLE_BUNDLE_NAME          "Elfeed2"
    MACOSX_BUNDLE_BUNDLE_VERSION       "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_LONG_VERSION_STRING  "${PROJECT_VERSION}"
    # Surfaces in Finder's Get Info > Copyright row, and the
    # generic About dialog wxAboutBox shows on macOS.
    MACOSX_BUNDLE_COPYRIGHT            "Public Domain (Unlicense)"
    # Old-style "Get Info" string. Some Spotlight tooling reads
    # this; keeping it set is cheap insurance.
    MACOSX_BUNDLE_INFO_STRING          "Elfeed2 ${PROJECT_VERSION}, Public Domain"
  )
endif()

if(WIN32)
  # The RC compiler runs from the build dir; point it at src/ so the
  # `#include "wx/msw/wx.rc"` and the `1 24 "elfeed2.manifest"`
  # relative-path reference both resolve. Rebuild on manifest or .in
  # changes too.
  set_source_files_properties("${elfeed2_rc_out}" PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src"
    OBJECT_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/src/elfeed2.manifest;${CMAKE_CURRENT_SOURCE_DIR}/src/elfeed2.ico;${elfeed2_rc_in}"
  )
endif()

target_link_libraries(elfeed2 PRIVATE
  wx::core wx::base wx::html wx::aui
  sqlite3 pugixml
)

if(WIN32)
  target_link_libraries(elfeed2 PRIVATE winhttp)
else()
  # `tls` carries both the link libs (OpenSSL or mbedTLS depending
  # on DEPS) AND the right CPPHTTPLIB_*_SUPPORT compile define on
  # its INTERFACE properties — see cmake/FetchTLS.cmake.
  target_link_libraries(elfeed2 PRIVATE tls cpp-httplib)
  if(APPLE)
    # Newer cpp-httplib versions (v0.43.1+ as packaged by Homebrew)
    # added native keychain CA enumeration that calls into Security
    # framework. Link defensively — both frameworks are part of
    # macOS, no install footprint.
    target_link_libraries(elfeed2 PRIVATE
      "-framework Security" "-framework CoreFoundation")
  endif()
endif()

target_compile_definitions(elfeed2 PRIVATE
  ELFEED_VERSION="${PROJECT_VERSION}"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(elfeed2 PRIVATE -Wall)
  # wxWidgets headers trip -Wconversion heavily; mark them as system to
  # suppress those warnings on our own code without losing -Wall.
  target_include_directories(elfeed2 SYSTEM PRIVATE
    $<TARGET_PROPERTY:wx::core,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# --- Packaging (CPack) ---
# `cmake --build build --target package` produces a release artifact:
#   macOS:   elfeed2-<ver>-macos-arm64.dmg   (drag the .app to
#            Applications)
#   Windows: not produced here; the release pipeline cross-builds
#            the .exe via the mingw toolchain, signs it, and zips it
#            with README.md after. See .github/workflows/release.yml.

# Pin our install rule to its own component so CPack can restrict
# the DMG to it. Without this, mbedTLS / pugixml subprojects' own
# install rules run too and dump include/ and lib/ into the DMG.
install(TARGETS elfeed2
  RUNTIME DESTINATION . COMPONENT app
  BUNDLE  DESTINATION . COMPONENT app
)

set(CPACK_PACKAGE_NAME    "elfeed2")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
if(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
  set(CPACK_PACKAGE_FILE_NAME "elfeed2-${PROJECT_VERSION}-macos-arm64")
  set(CPACK_DMG_VOLUME_NAME   "Elfeed2")
  # Force APFS instead of CMake's default HFS+. On macOS Sequoia
  # (15.x), `hdiutil create -fs HFS+` from an APFS source volume
  # produces a silently corrupt DMG that fails to verify. APFS DMG
  # creation works reliably. CPACK_DMG_FILESYSTEM is honored by
  # CMake 3.20+.
  set(CPACK_DMG_FILESYSTEM "APFS")
  # Limit the DMG to our `app` component — keeps subproject
  # install leftovers (mbedTLS headers / static libs) out.
  set(CPACK_COMPONENTS_ALL "app")
endif()
include(CPack)
