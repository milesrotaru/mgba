# Copyright (c) 2026 gsf2wav contributors
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# CMake toolchain file for cross-compiling gsf2wav to 64-bit Windows with
# MinGW-w64 (Debian/Ubuntu: gcc-mingw-w64-x86-64-posix mingw-w64-x86-64-dev).
# The "posix" threading flavour is used because gsf2wav renders tracks on
# several threads.
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=src/platform/gsf2wav/cmake/mingw-w64-x86_64.cmake \
#         -DBUILD_GSF2WAV=ON -DBUILD_QT=OFF -DBUILD_SDL=OFF ...
# or just run src/platform/gsf2wav/tools/build-windows.sh.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# One self-contained .exe: no zlib1.dll / libwinpthread-1.dll / libgcc DLLs
# for the user to hunt down.
# (Linking zlib statically needs -DZLIB_LIBRARY=<...>/libz.a; CMake's MinGW
# platform module prefers the libz.dll.a import library otherwise. The build
# script does that.)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc")
