#!/bin/sh
# Builds newen (32-bit) on Linux with gcc -m32, CMake and Ninja (or make). Output: bin/newen
# Needs: cmake, gcc with 32-bit support (Arch: lib32-gcc-libs), 32-bit SDL2 (Arch: lib32-sdl2-compat),
# 32-bit OpenGL (Arch: lib32-mesa or lib32-nvidia-utils). Extra arguments go to CMake's first configure.
set -e
cd "$(dirname "$0")"
CMAKE="${CMAKE:-cmake}"
if [ ! -f build-linux/CMakeCache.txt ]; then
	if command -v ninja >/dev/null 2>&1; then GEN=Ninja; else GEN="Unix Makefiles"; fi
	"$CMAKE" -S . -B build-linux -G "$GEN" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DCMAKE_C_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 -DCMAKE_LIBRARY_ARCHITECTURE=i386-linux-gnu "$@"
fi
"$CMAKE" --build build-linux --parallel
echo
echo "Built bin/newen"
