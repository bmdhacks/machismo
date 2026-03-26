#!/bin/bash
#
# Build optimized LuaJIT 2.1 for aarch64 Linux (Cortex-A35 target).
#
# Produces an unstripped libluajit-5.1.so.2 with -O2 and optional
# -mcpu=cortex-a35 tuning for the RK3326 handheld target.
#
# The unstripped binary lets perf show named LuaJIT internal functions
# (lj_vmeta_tgetv, lj_gc_step, etc.) instead of hex addresses.
#
# Submodule: extern/LuaJIT (v2.1 branch)
#
# Usage: ./scripts/build-luajit.sh
# Output: build-luajit/lib/libluajit-5.1.so.2
#
set -e

# Always work from the project root
cd "$(dirname "$0")/.."

SRC_DIR=extern/LuaJIT
BUILD_DIR=build-luajit

if [ ! -d "$SRC_DIR/src" ]; then
    echo "Error: $SRC_DIR/src not found. Run: git submodule update --init extern/LuaJIT"
    exit 1
fi

# Detect compiler: prefer gcc (LuaJIT's Makefile is gcc-oriented)
CC="${CC:-gcc}"

# Target CPU tuning. Default to cortex-a35 for handheld; override with
# LUAJIT_MCPU= (empty) for generic aarch64 or LUAJIT_MCPU=native for host.
MCPU="${LUAJIT_MCPU:-cortex-a35}"
if [ -n "$MCPU" ]; then
    CPU_FLAGS="-mcpu=$MCPU"
    echo "Building LuaJIT with -mcpu=$MCPU"
else
    CPU_FLAGS=""
    echo "Building LuaJIT for generic aarch64"
fi

echo "=== Cleaning previous build ==="
make -C "$SRC_DIR" clean 2>/dev/null || true

echo "=== Building LuaJIT ==="
make -C "$SRC_DIR" -j$(nproc) \
    CC="$CC" \
    CCOPT="-O2 -fomit-frame-pointer $CPU_FLAGS" \
    CCOPT_arm64="$CPU_FLAGS" \
    CCDEBUG="-g" \
    BUILDMODE=dynamic \
    Q=

echo "=== Collecting output ==="
mkdir -p "$BUILD_DIR/lib"
mkdir -p "$BUILD_DIR/share/luajit-2.1/jit"

# The Makefile produces src/libluajit.so with soname libluajit-5.1.so.2
cp "$SRC_DIR/src/libluajit.so" "$BUILD_DIR/lib/libluajit-5.1.so.2"

# Copy jit module Lua files (jit.p profiler, jit.v verbose, jit.dump, etc.)
cp "$SRC_DIR/src/jit/"*.lua "$BUILD_DIR/share/luajit-2.1/jit/"

# Also copy the luajit binary for standalone testing (jit verification, bytecode listing)
cp "$SRC_DIR/src/luajit" "$BUILD_DIR/luajit"

echo "=== Done ==="
ls -lh "$BUILD_DIR/lib/libluajit-5.1.so.2"
echo ""
echo "Symbols (NOT stripped — visible in perf):"
nm -D "$BUILD_DIR/lib/libluajit-5.1.so.2" | wc -l
echo ""
echo "jit modules:"
ls "$BUILD_DIR/share/luajit-2.1/jit/"*.lua | wc -l
echo ""
echo "To use: copy build-luajit/lib/libluajit-5.1.so.2 to port/necrodancer/libs/"
echo "        copy build-luajit/share/ to port/necrodancer/libs/share/"
