cmake_minimum_required(VERSION 3.25)
project(elfeed2 VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEPS "AUTO" CACHE STRING
    "Dependency resolution: AUTO, DOWNLOAD, or LOCAL")
set_property(CACHE DEPS PROPERTY STRINGS AUTO DOWNLOAD LOCAL)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- Dear ImGui (always bundled or downloaded) ---

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui")
  set(FETCHCONTENT_SOURCE_DIR_IMGUI "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui")
  message(STATUS "Dear ImGui: bundled")
else()
  message(STATUS "Dear ImGui: downloaded")
endif()

FetchContent_Declare(
  imgui
  URL https://github.com/ocornut/imgui/archive/f5f6ca07be7ce0ea9eed6c04d55833bac3f6b50b.tar.gz
  URL_HASH SHA256=91aed5e92c1e24aada8dfd2a6933222341ec291dc29a4add49828610d5cb6765
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)

# --- SDL3 ---

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/SDL")
  set(FETCHCONTENT_SOURCE_DIR_SDL3 "${CMAKE_CURRENT_SOURCE_DIR}/deps/SDL")
  set(_sdl3_origin "bundled")
endif()

if(DEPS STREQUAL "LOCAL")
  find_package(SDL3 REQUIRED)
  message(STATUS "SDL3: system")
elseif(DEPS STREQUAL "AUTO"
       AND NOT DEFINED FETCHCONTENT_SOURCE_DIR_SDL3
       AND FETCHCONTENT_FULLY_DISCONNECTED)
  find_package(SDL3 QUIET)
  if(SDL3_FOUND)
    set(_sdl3_origin "system")
  endif()
endif()

if(NOT _sdl3_origin STREQUAL "system" AND NOT DEPS STREQUAL "LOCAL")
  if(NOT _sdl3_origin)
    set(_sdl3_origin "downloaded")
  endif()
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    SDL3
    URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.2.30.tar.gz
    URL_HASH SHA256=8bfb8f70a72f216dff66cde64b9d7b863d5e8aa46be7f2fd400d7f1f9baafe76
  )
  FetchContent_MakeAvailable(SDL3)
endif()
message(STATUS "SDL3: ${_sdl3_origin}")

target_link_libraries(imgui PUBLIC SDL3::SDL3)

# --- SQLite3 (amalgamation) ---

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/sqlite")
  set(FETCHCONTENT_SOURCE_DIR_SQLITE "${CMAKE_CURRENT_SOURCE_DIR}/deps/sqlite")
  set(_sqlite_origin "bundled")
endif()

if(NOT _sqlite_origin)
  set(_sqlite_origin "downloaded")
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
message(STATUS "SQLite: ${_sqlite_origin}")

# --- pugixml ---

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deps/pugixml")
  set(FETCHCONTENT_SOURCE_DIR_PUGIXML "${CMAKE_CURRENT_SOURCE_DIR}/deps/pugixml")
  set(_pugixml_origin "bundled")
endif()

if(DEPS STREQUAL "LOCAL")
  find_package(pugixml REQUIRED)
  set(_pugixml_origin "system")
elseif(DEPS STREQUAL "AUTO"
       AND NOT DEFINED FETCHCONTENT_SOURCE_DIR_PUGIXML
       AND FETCHCONTENT_FULLY_DISCONNECTED)
  find_package(pugixml QUIET)
  if(pugixml_FOUND)
    set(_pugixml_origin "system")
  endif()
endif()

if(NOT _pugixml_origin STREQUAL "system" AND NOT DEPS STREQUAL "LOCAL")
  if(NOT _pugixml_origin)
    set(_pugixml_origin "downloaded")
  endif()
  FetchContent_Declare(
    pugixml
    URL https://github.com/zeux/pugixml/releases/download/v1.14/pugixml-1.14.tar.gz
    URL_HASH SHA256=2f10e276870c64b1db6809050a75e11a897a8d7456c4be5c6b2e35a11168a015
  )
  FetchContent_MakeAvailable(pugixml)
endif()
message(STATUS "pugixml: ${_pugixml_origin}")

# --- libcurl ---

find_package(CURL REQUIRED)
message(STATUS "CURL: ${CURL_VERSION_STRING}")

# --- Elfeed ---

add_executable(elfeed2 src/elfeed.cpp src/platform_sdl3.cpp src/db.cpp
                       src/config.cpp src/feed.cpp src/html.cpp
                       src/fetch.cpp src/filter.cpp src/download.cpp)
target_link_libraries(elfeed2 PRIVATE imgui sqlite3 pugixml CURL::libcurl)

if(APPLE)
  target_link_libraries(elfeed2 PRIVATE "-framework OpenGL")
elseif(NOT WIN32)
  target_link_libraries(elfeed2 PRIVATE GL)
endif()

target_compile_definitions(elfeed2 PRIVATE
  ELFEED_VERSION="${PROJECT_VERSION}"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(elfeed2 PRIVATE -Wall -Wconversion)
endif()
