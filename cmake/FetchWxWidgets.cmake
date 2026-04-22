# Resolve the wxWidgets dep into wx::core, wx::base, wx::html, wx::aui
# imported targets that the rest of the build can link to uniformly.
#
# DEPS=LOCAL — system wxWidgets via find_package. Two resolution paths:
#
#   CONFIG mode — finds wxWidgetsConfig.cmake from a CMake-built install.
#     Creates the wx::* imported targets directly.
#
#   MODULE mode — uses CMake's bundled FindwxWidgets.cmake, which shells
#     out to wx-config and returns variables (wxWidgets_LIBRARIES,
#     wxWidgets_USE_FILE) instead of targets. This is the typical
#     Linux distro path (apt install libwxgtk3.2-dev).
#
# DEPS=FETCH — pull the pinned 3.2.10 release tarball, build statically.

if(DEPS STREQUAL "LOCAL")
  find_package(wxWidgets 3.2 QUIET CONFIG
    COMPONENTS core base html aui)
  if(NOT wxWidgets_FOUND)
    find_package(wxWidgets 3.2 REQUIRED MODULE
      COMPONENTS core base html aui)
  endif()
  if(wxWidgets_USE_FILE)
    # MODULE result: include the use-file (sets compile flags +
    # definitions globally) and synthesise wx::* targets from the
    # combined link list. All four aliases point at the same
    # libraries; the linker dedupes.
    include(${wxWidgets_USE_FILE})
    foreach(_comp core base html aui)
      if(NOT TARGET wx::${_comp})
        add_library(wx::${_comp} INTERFACE IMPORTED)
        target_link_libraries(wx::${_comp}
          INTERFACE ${wxWidgets_LIBRARIES})
      endif()
    endforeach()
  endif()
  return()
endif()

include(FetchContent)
fetch_content_bundled(wxWidgets wxWidgets)

# Must be set BEFORE FetchContent_MakeAvailable.
set(wxBUILD_SHARED  OFF CACHE BOOL "" FORCE)
set(wxBUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(wxBUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(wxBUILD_DEMOS   OFF CACHE BOOL "" FORCE)
set(wxBUILD_INSTALL OFF CACHE BOOL "" FORCE)
set(wxUSE_STC       OFF CACHE BOOL "" FORCE)
set(wxUSE_WEBVIEW   OFF CACHE BOOL "" FORCE)
set(wxUSE_MEDIACTRL OFF CACHE BOOL "" FORCE)

# The release tarball includes all bundled third-party sources inline
# (zlib, libpng, libjpeg, libtiff, expat, …), so unlike the git repo
# there are no submodules to recurse.
FetchContent_Declare(
  wxWidgets
  URL      https://github.com/wxWidgets/wxWidgets/releases/download/v3.2.10/wxWidgets-3.2.10.tar.bz2
  URL_HASH SHA256=d66e929569947a4a5920699539089a9bda83a93e5f4917fb313a61f0c344b896
)
FetchContent_MakeAvailable(wxWidgets)
