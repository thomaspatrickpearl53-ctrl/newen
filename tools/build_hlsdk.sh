#!/bin/sh
# Builds hlsdk-portable's Half-Life game libraries for a 64-bit newen (docs/MOBILE_PLAN.md).
# Valve's own hl.so/client.so are 32-bit, so a 64-bit engine runs these instead: Valve's SDK code,
# made portable by the FWGS project (github.com/FWGS/hlsdk-portable, Valve's Half-Life SDK licence).
# Nothing of it goes into newen's sources: it's fetched into build-hlsdk/ at a fixed commit.
#
#   tools/build_hlsdk.sh [output folder]     default: bin64
#
# The libraries land in <output>/hlsdk/valve/dlls and cl_dlls, which a 64-bit newen searches before
# the Half-Life folder. Extra CMake arguments can come through HLSDK_CMAKE_ARGS (the Android build
# passes the NDK's toolchain there).
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
OUT="${1:-bin64}"
case "$OUT" in /*) ;; *) OUT="$ROOT/$OUT" ;; esac
CMAKE="${CMAKE:-cmake}"
HLSDK_URL=https://github.com/FWGS/hlsdk-portable
HLSDK_COMMIT=89e83b92d561f5ceb11594c2f15e2e1519bc6c36   # 2026-11-08
SRC="$ROOT/build-hlsdk/src"
BUILD="$ROOT/build-hlsdk/${HLSDK_BUILD_NAME:-linux64}"

if [ ! -d "$SRC/.git" ]; then
	git clone --filter=blob:none "$HLSDK_URL" "$SRC"
fi
if [ "$(git -C "$SRC" rev-parse HEAD)" != "$HLSDK_COMMIT" ]; then
	git -C "$SRC" fetch --depth 1 origin "$HLSDK_COMMIT"
	git -C "$SRC" checkout -q "$HLSDK_COMMIT"
fi
# FreeVGUI (BSD): the 64-bit client links it in, because Valve's vgui.so is 32-bit only
git -C "$SRC" submodule update --init --depth 1 freevgui

if command -v ninja >/dev/null 2>&1; then GEN=Ninja; else GEN="Unix Makefiles"; fi
# shellcheck disable=SC2086
"$CMAKE" -S "$SRC" -B "$BUILD" -G "$GEN" -DCMAKE_BUILD_TYPE=Release -D64BIT=ON \
	-DCMAKE_INSTALL_PREFIX="$OUT/hlsdk" -DGAMEDIR=valve $HLSDK_CMAKE_ARGS
"$CMAKE" --build "$BUILD" --parallel
"$CMAKE" --install "$BUILD"
echo
echo "hlsdk-portable's libraries are in $OUT/hlsdk/valve"
