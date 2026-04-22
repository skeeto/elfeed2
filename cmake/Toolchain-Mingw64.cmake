# Cross-compile Elfeed2 for 64-bit Windows using mingw-w64.
#
# Usage:
#   cmake -B build-win \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Mingw64.cmake
#   cmake --build build-win
#
# The resulting elfeed2.exe is fully self-contained: libstdc++,
# libgcc, and winpthread are statically linked, so the binary
# doesn't drag any mingw-w64 runtime DLLs along with it. Users
# only need the WebView2 runtime if they care about that — but
# we don't (wxUSE_WEBVIEW=OFF in CMakeLists), so a plain Windows
# install runs the .exe directly.
#
# Required tooling on the host:
#   x86_64-w64-mingw32-gcc / g++ / windres
# Distros (one of):
#   apt: gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64
#   dnf: mingw64-gcc mingw64-gcc-c++
#   brew: mingw-w64
#   pacman: mingw-w64-gcc

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_triple x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_triple}-g++)
set(CMAKE_RC_COMPILER  ${_triple}-windres)

# Where to look for cross-built dependencies and the mingw sysroot.
# Different distros put it in different places — list the common
# locations and let CMake pick whichever exists.
set(CMAKE_FIND_ROOT_PATH
  /usr/${_triple}
  /usr/${_triple}/sys-root/mingw
  /opt/homebrew/${_triple}
  /opt/homebrew/opt/mingw-w64/toolchain-x86_64
  /usr/local/${_triple}
)

# Host-side tools (ccache, python, etc.) come from the host PATH;
# libraries / headers / cmake packages come only from the cross
# sysroot so we don't accidentally pick up a host library.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Statically link libgcc, libstdc++, and winpthread. Without this
# the .exe needs libgcc_s_seh-1.dll, libstdc++-6.dll, and
# libwinpthread-1.dll alongside it on the target system.
# *_INIT seeds CMAKE_*_FLAGS on first configure; users can still
# override by setting CMAKE_EXE_LINKER_FLAGS explicitly.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-static -static-libgcc -static-libstdc++")
