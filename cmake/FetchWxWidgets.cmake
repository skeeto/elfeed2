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

# Disable wxWidgets subsystems we don't use. Each one drops source
# files from wx's own build (faster initial compile) AND removes
# the corresponding object code from the linked binary. Re-enable
# any of these the day a feature actually needs them.
set(wxUSE_STC                    OFF CACHE BOOL "" FORCE)  # Scintilla
set(wxUSE_WEBVIEW                OFF CACHE BOOL "" FORCE)  # WebKit/WebView2
set(wxUSE_MEDIACTRL              OFF CACHE BOOL "" FORCE)  # audio/video
set(wxUSE_RICHTEXT               OFF CACHE BOOL "" FORCE)  # rich-text editor
set(wxUSE_PROPGRID               OFF CACHE BOOL "" FORCE)  # property grid
set(wxUSE_RIBBON                 OFF CACHE BOOL "" FORCE)  # Office ribbon
set(wxUSE_OPENGL                 OFF CACHE BOOL "" FORCE)  # GL canvas
set(wxUSE_GLCANVAS               OFF CACHE BOOL "" FORCE)
set(wxUSE_PRINTING_ARCHITECTURE  OFF CACHE BOOL "" FORCE)  # print preview
set(wxUSE_HELP                   OFF CACHE BOOL "" FORCE)  # help system
set(wxUSE_WXHTML_HELP            OFF CACHE BOOL "" FORCE)  # html-based help
set(wxUSE_DEBUGREPORT            OFF CACHE BOOL "" FORCE)  # crash reports

# Disable wx's per-target precompiled headers ONLY when ccache is
# active (CMakeLists sets ELFEED2_CCACHE_ACTIVE for us). Rationale:
# the .pch binary embeds a timestamp nonce that ccache treats as
# an extra-file mismatch — so every fresh build's wx compiles
# miss the cache regardless of pch_defines/include_file_mtime
# sloppiness. With ccache, off wins (first cold build slower by
# ~30%, but every subsequent clean build hits cache at near-100%).
# Without ccache (pipeline builds, fresh checkouts), PCH on is
# faster, period.
if(ELFEED2_CCACHE_ACTIVE)
  # wxBUILD_PRECOMP is declared by wx as a STRING (ON/OFF/COTIRE),
  # NOT a BOOL — `CACHE BOOL` here is silently ignored because the
  # existing cache type doesn't match.
  set(wxBUILD_PRECOMP            "OFF" CACHE STRING "" FORCE)
endif()

# The release tarball includes all bundled third-party sources inline
# (zlib, libpng, libjpeg, libtiff, expat, …), so unlike the git repo
# there are no submodules to recurse.
FetchContent_Declare(
  wxWidgets
  URL      https://github.com/wxWidgets/wxWidgets/releases/download/v3.2.10/wxWidgets-3.2.10.tar.bz2
  URL_HASH SHA256=d66e929569947a4a5920699539089a9bda83a93e5f4917fb313a61f0c344b896
)
FetchContent_MakeAvailable(wxWidgets)
