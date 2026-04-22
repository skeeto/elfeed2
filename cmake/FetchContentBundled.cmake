# Helper that lets every FetchContent_Declare in the top-level
# CMakeLists fall back to a local checkout under deps/<dir> when
# present, instead of downloading and re-extracting on each clean
# build. Useful when developing with a hacked dep, or when offline.
#
# Usage:
#   include(FetchContentBundled)
#   fetch_content_bundled(wxWidgets wxWidgets)   # name, deps subdir
#   FetchContent_Declare(wxWidgets URL ... URL_HASH ...)
#   FetchContent_MakeAvailable(wxWidgets)
#
# The convention FETCHCONTENT_SOURCE_DIR_<NAME> is the documented
# wat to override fetch behavior; we just compute the right name
# and point it at the right place.

function(fetch_content_bundled name dir)
  if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/deps/${dir}")
    string(TOUPPER "${name}" upper)
    set(FETCHCONTENT_SOURCE_DIR_${upper}
        "${CMAKE_SOURCE_DIR}/deps/${dir}" PARENT_SCOPE)
    message(STATUS "${name}: bundled (deps/${dir})")
  else()
    message(STATUS "${name}: downloaded")
  endif()
endfunction()
