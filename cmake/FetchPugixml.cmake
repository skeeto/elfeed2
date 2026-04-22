# Resolve the pugixml dep into a `pugixml` target.
#
# DEPS=LOCAL — find_package(pugixml) (Debian's libpugixml-dev ships
#              a CMake config). The system package exports
#              `pugixml::pugixml`; wrap in an unqualified `pugixml`
#              target to match the FETCH path.
# DEPS=FETCH — pugixml's own CMakeLists already creates a `pugixml`
#              static target on add_subdirectory.

if(DEPS STREQUAL "LOCAL")
  find_package(pugixml REQUIRED)
  if(NOT TARGET pugixml)
    add_library(pugixml INTERFACE)
    target_link_libraries(pugixml INTERFACE pugixml::pugixml)
  endif()
  return()
endif()

include(FetchContent)
fetch_content_bundled(pugixml pugixml)

FetchContent_Declare(
  pugixml
  URL https://github.com/zeux/pugixml/releases/download/v1.14/pugixml-1.14.tar.gz
  URL_HASH SHA256=2f10e276870c64b1db6809050a75e11a897a8d7456c4be5c6b2e35a11168a015
)
FetchContent_MakeAvailable(pugixml)
