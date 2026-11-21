#!/bin/sh
# Builds newen on Linux with gcc, CMake and Ninja (or make).
#   ./build.sh          32-bit (gcc -m32), runs Valve's own Half-Life libraries. Output: bin/newen
#   ARCH=64 ./build.sh  64-bit, runs hlsdk-portable's libraries (tools/build_hlsdk.sh builds them,
#                       docs/MOBILE_PLAN.md says why). Output: bin64/newen
# 32-bit needs: cmake, gcc with 32-bit support (Arch: lib32-gcc-libs), 32-bit SDL2 (Arch: lib32-sdl2-compat),
# 32-bit OpenGL (Arch: lib32-mesa or lib32-nvidia-utils). 64-bit needs the ordinary sdl2 and OpenGL.
# Extra arguments go to CMake's first configure.
set -e
cd "$(dirname "$0")"
CMAKE="${CMAKE:-cmake}"
if command -v ninja >/dev/null 2>&1; then GEN=Ninja; else GEN="Unix Makefiles"; fi
if [ "$ARCH" = 64 ]; then
	BUILD=build-linux64
	if [ ! -f $BUILD/CMakeCache.txt ]; then
		"$CMAKE" -S . -B $BUILD -G "$GEN" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOUT_DIR="$(pwd)/bin64" "$@"
	fi
	OUT=bin64/newen
else
	BUILD=build-linux
	if [ ! -f $BUILD/CMakeCache.txt ]; then
		"$CMAKE" -S . -B $BUILD -G "$GEN" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
			-DCMAKE_C_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 -DCMAKE_LIBRARY_ARCHITECTURE=i386-linux-gnu "$@"
	fi
	OUT=bin/newen
fi
"$CMAKE" --build $BUILD --parallel
echo
echo "Built $OUT"
