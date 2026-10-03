#!/bin/sh
# Copyright (c) 2026 gsf2wav contributors
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Cross-builds gsf2wav.exe for 64-bit Windows with MinGW-w64, then zips it up
# with its documentation (package_windows.py).
#
#   build-windows.sh [BUILD_DIR [OUT_DIR]]
#
# On Debian/Ubuntu the tools are:
#   apt install cmake python3 gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix \
#               mingw-w64-x86-64-dev libz-mingw-w64-dev
# The exe is statically linked (zlib and the C++-free runtime included), so it
# depends only on DLLs every Windows installation has. To run the test suite
# against it under Wine:
#   GSF2WAV_RUNNER=wine64 python3 src/platform/gsf2wav/test/run.py BUILD_DIR/gsf2wav/gsf2wav.exe
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
build=${1:-$root/build-windows}
out=${2:-$root/dist}
zlib=${ZLIB_STATIC:-/usr/x86_64-w64-mingw32/lib/libz.a}

if ! command -v x86_64-w64-mingw32-gcc-posix >/dev/null 2>&1; then
	echo "build-windows.sh: x86_64-w64-mingw32-gcc-posix not found (see the apt line at the top of this script)" >&2
	exit 1
fi
if [ ! -f "$zlib" ]; then
	echo "build-windows.sh: no static zlib at $zlib (set ZLIB_STATIC, or install libz-mingw-w64-dev)" >&2
	exit 1
fi
if [ -n "$(git -C "$root" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
	echo "build-windows.sh: warning: uncommitted changes, so the version will say -dirty" >&2
fi

# Only the pieces gsf2wav needs: no frontends, no optional libraries. The
# configure step is cheap and re-reads the git version every time.
cmake -S "$root" -B "$build" -G "Unix Makefiles" \
	-DCMAKE_TOOLCHAIN_FILE="$root/src/platform/gsf2wav/cmake/mingw-w64-x86_64.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DZLIB_LIBRARY="$zlib" \
	-DBUILD_GSF2WAV=ON -DBUILD_QT=OFF -DBUILD_SDL=OFF \
	-DBUILD_SHARED=OFF -DBUILD_STATIC=ON \
	-DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF -DUSE_EPOXY=OFF \
	-DUSE_FFMPEG=OFF -DUSE_PNG=OFF -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF -DUSE_SQLITE3=OFF \
	-DUSE_ELF=OFF -DUSE_LUA=OFF -DUSE_JSON_C=OFF -DUSE_FREETYPE=OFF -DUSE_LZMA=OFF \
	-DUSE_DISCORD_RPC=OFF -DENABLE_SCRIPTING=OFF -DENABLE_GDB_STUB=OFF -DM_CORE_GB=OFF
cmake --build "$build" --target gsf2wav -j "$(nproc 2>/dev/null || echo 2)"

exe=$build/gsf2wav/gsf2wav.exe
mkdir -p "$out"
python3 "$here/package_windows.py" "$exe" "$out"
