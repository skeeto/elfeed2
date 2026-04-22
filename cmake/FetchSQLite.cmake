# Resolve the SQLite3 dep into a `sqlite3` target.
#
# DEPS=LOCAL — find the system shared library (Debian's libsqlite3-dev
#              etc.). Wrap it in an INTERFACE target named `sqlite3` so
#              the rest of the build links the same way as in FETCH.
# DEPS=FETCH — build the amalgamation as a static library, with a
#              hardened compile-define set chosen to match what we
#              actually use (no DQS, threadsafe, no shared cache).

if(DEPS STREQUAL "LOCAL")
  # CMake ships FindSQLite3.cmake. CMake 3.27 renamed the imported
  # target from SQLite::SQLite3 to SQLite3::SQLite3 (deprecating the
  # old spelling); prefer the new name and fall back so we work on
  # older CMake too.
  find_package(SQLite3 REQUIRED)
  if(TARGET SQLite3::SQLite3)
    set(_sqlite_target SQLite3::SQLite3)
  else()
    set(_sqlite_target SQLite::SQLite3)
  endif()
  if(NOT TARGET sqlite3)
    add_library(sqlite3 INTERFACE)
    target_link_libraries(sqlite3 INTERFACE ${_sqlite_target})
  endif()
  return()
endif()

include(FetchContent)
fetch_content_bundled(sqlite sqlite)

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
  # FTS5 is an opt-in amalgamation feature; without this define
  # `CREATE VIRTUAL TABLE ... USING fts5(...)` fails at schema
  # init and the filter path's MATCH queries silently match
  # nothing. LOCAL builds get FTS5 from the distro's libsqlite3,
  # which has it enabled by default on every platform we target.
  SQLITE_ENABLE_FTS5
)
