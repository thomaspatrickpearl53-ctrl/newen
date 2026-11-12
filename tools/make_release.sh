#!/bin/sh
# Builds newen's release archives on Linux into dist/:
#   newen-<version>-linux-x86.tar.gz   gcc -m32 (the same build as build.sh)
#   newen-<version>-win32.zip          cross-built with MinGW (i686-w64-mingw32-gcc)
# The version is ENGINE_VERSION in src/engine/common.h.
# Needs: what build.sh needs, plus mingw-w64 (Arch: mingw-w64-gcc) and python3 for the zip.
# The Windows build made here is MinGW's, not Visual C++'s (build.bat): it has no guard around crashes in
# client.dll, and it needs Windows 10 or later (it uses the system's UCRT, no VCRUNTIME140.dll).
set -e
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^#define ENGINE_VERSION "\(.*\)"/\1/p' src/engine/common.h)
[ -n "$VER" ] || { echo "ENGINE_VERSION not found"; exit 1; }
rm -rf dist
mkdir -p dist

# Linux
sh ./build.sh
L=dist/newen-$VER-linux-x86
mkdir -p "$L"
cp bin/newen "$L/"
strip --strip-debug "$L/newen"
cp -r base "$L/"
cp README.md LICENSE THIRD_PARTY_LICENSES.txt "$L/"
tar -C dist -czf "$L.tar.gz" "newen-$VER-linux-x86"

# Windows
if [ ! -f build-mingw/CMakeCache.txt ]; then
	cmake -S . -B build-mingw -G "Unix Makefiles" -DCMAKE_SYSTEM_NAME=Windows \
		-DCMAKE_C_COMPILER=i686-w64-mingw32-gcc -DCMAKE_RC_COMPILER=i686-w64-mingw32-windres \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo "-DOUT_DIR=$PWD/build-mingw/out"
fi
cmake --build build-mingw --parallel
W=dist/newen-$VER-win32
mkdir -p "$W"
cp build-mingw/out/newen.exe build-mingw/out/SDL2.dll "$W/"
i686-w64-mingw32-strip --strip-debug "$W/newen.exe"
cp -r base "$W/"
cp README.md LICENSE THIRD_PARTY_LICENSES.txt "$W/"
(cd dist && python3 -c "
import os, sys, zipfile
top = sys.argv[1]
with zipfile.ZipFile(top + '.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(top):
        for f in sorted(files):
            z.write(os.path.join(d, f))
" "newen-$VER-win32")

rm -rf "$L" "$W"
echo
ls -l dist
