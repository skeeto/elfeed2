cmake_minimum_required(VERSION 3.25)
project(elfeed2 VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Pin C to gnu17 so wxWidgets' bundled C deps (zlib, libpng, libjpeg,
# libtiff, expat) build cleanly under GCC's newer C23 default.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- wxWidgets ---

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/wxWidgets")
  set(FETCHCONTENT_SOURCE_DIR_WXWIDGETS
      "${CMAKE_CURRENT_SOURCE_DIR}/deps/wxWidgets")
  message(STATUS "wxWidgets: bundled")
else()
  message(STATUS "wxWidgets: downloaded")
endif()

set(wxBUILD_SHARED  OFF CACHE BOOL "" FORCE)
set(wxBUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(wxBUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(wxBUILD_DEMOS   OFF CACHE BOOL "" FORCE)
set(wxBUILD_INSTALL OFF CACHE BOOL "" FORCE)
set(wxUSE_STC       OFF CACHE BOOL "" FORCE)
set(wxUSE_WEBVIEW   OFF CACHE BOOL "" FORCE)
set(wxUSE_MEDIACTRL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  wxWidgets
  URL      https://github.com/wxWidgets/wxWidgets/releases/download/v3.2.10/wxWidgets-3.2.10.tar.bz2
  URL_HASH SHA256=d66e929569947a4a5920699539089a9bda83a93e5f4917fb313a61f0c344b896
)
FetchContent_MakeAvailable(wxWidgets)

# --- SQLite3 (amalgamation) ---

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/sqlite")
  set(FETCHCONTENT_SOURCE_DIR_SQLITE "${CMAKE_CURRENT_SOURCE_DIR}/deps/sqlite")
  message(STATUS "SQLite: bundled")
else()
  message(STATUS "SQLite: downloaded")
endif()

FetchContent_Declare(
  sqlite
  URL https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip
  URL_HASH SHA256=6cebd1d8403fc58c30e93939b246f3e6e58d0765a5cd50546f16c00fd805d2c3
)
FetchContent_MakeAvailable(sqlite)

add_library(sqlite3 STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_DQS=0
  SQLITE_THREADSAFE=2
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  SQLITE_LIKE_DOESNT_MATCH_BLOBS
  SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_SHARED_CACHE
)

# --- pugixml ---

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/pugixml")
  set(FETCHCONTENT_SOURCE_DIR_PUGIXML "${CMAKE_CURRENT_SOURCE_DIR}/deps/pugixml")
  message(STATUS "pugixml: bundled")
else()
  message(STATUS "pugixml: downloaded")
endif()

FetchContent_Declare(
  pugixml
  URL https://github.com/zeux/pugixml/releases/download/v1.14/pugixml-1.14.tar.gz
  URL_HASH SHA256=2f10e276870c64b1db6809050a75e11a897a8d7456c4be5c6b2e35a11168a015
)
FetchContent_MakeAvailable(pugixml)

# --- mbedTLS + cpp-httplib (POSIX only; Windows uses WinHTTP) ---

if(NOT WIN32)
  # mbedTLS
  if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/mbedtls")
    set(FETCHCONTENT_SOURCE_DIR_MBEDTLS
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/mbedtls")
    message(STATUS "mbedTLS: bundled")
  else()
    message(STATUS "mbedTLS: downloaded")
  endif()

  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_AS_SUBPROJECT ON CACHE BOOL "" FORCE)
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    mbedtls
    URL      https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
    URL_HASH SHA256=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
  )
  FetchContent_MakeAvailable(mbedtls)

  # cpp-httplib (header-only; only need its include path)
  if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/cpp-httplib")
    set(FETCHCONTENT_SOURCE_DIR_CPP-HTTPLIB
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/cpp-httplib")
    message(STATUS "cpp-httplib: bundled")
  else()
    message(STATUS "cpp-httplib: downloaded")
  endif()

  FetchContent_Declare(
    cpp-httplib
    URL      https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.43.0.tar.gz
    URL_HASH SHA256=28b88a7f0751c762cc9c70c91db0d6953554c40f49285907e716dd7943a824a5
  )
  FetchContent_MakeAvailable(cpp-httplib)
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
  src/util.cpp
)
if(WIN32)
  list(APPEND elfeed_sources src/http_win.cpp src/elfeed2.rc)
  enable_language(RC)
else()
  list(APPEND elfeed_sources src/http_posix.cpp)
endif()

add_executable(elfeed2 WIN32 MACOSX_BUNDLE ${elfeed_sources})

if(WIN32)
  # The RC compiler runs from the build dir; make it find the manifest
  # next to the .rc file, and rebuild when the manifest changes.
  set_source_files_properties(src/elfeed2.rc PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src"
    OBJECT_DEPENDS      "${CMAKE_CURRENT_SOURCE_DIR}/src/elfeed2.manifest"
  )
endif()

target_link_libraries(elfeed2 PRIVATE
  wx::core wx::base wx::html wx::aui
  sqlite3 pugixml
)

if(WIN32)
  target_link_libraries(elfeed2 PRIVATE winhttp)
else()
  target_link_libraries(elfeed2 PRIVATE
    mbedtls mbedx509 mbedcrypto
  )
  target_include_directories(elfeed2 PRIVATE ${cpp-httplib_SOURCE_DIR})
  target_compile_definitions(elfeed2 PRIVATE CPPHTTPLIB_MBEDTLS_SUPPORT)
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
