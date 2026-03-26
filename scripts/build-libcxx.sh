#!/bin/bash
# Build Apple-ABI-compatible libc++ for aarch64 Linux.
#
# This produces a libc++.so.1 with Apple's std::string layout
# (_LIBCPP_ABI_ALTERNATE_STRING_LAYOUT) so Mach-O binaries compiled
# against Apple's libc++ get the correct memory layout.
#
# Patches for macOS pthread compatibility (mutex/condvar signature
# detection) are applied automatically from patches/.
#
# Compiler auto-detection: uses clang if available (>= 15), falls back to GCC.
# Always builds against LLVM 15.0.7 (works with both clang and GCC).
#
# Usage: ./scripts/build-libcxx.sh
# Output: build-libcxx/lib/libc++.so.1

set -e

# Always work from the project root
cd "$(dirname "$0")/.."

BUILD_DIR=build-libcxx
SRC_DIR=extern/llvm-project
PATCH_DIR=patches

if [ ! -d "$SRC_DIR/libcxx" ]; then
    echo "Error: $SRC_DIR/libcxx not found. Run: git submodule update --init extern/llvm-project"
    exit 1
fi

# Detect compiler: prefer clang >= 15, fall back to GCC
if command -v clang++ >/dev/null 2>&1; then
    CLANG_VER=$(clang++ --version | head -1 | grep -oP '\d+' | head -1)
    if [ "$CLANG_VER" -ge 15 ] 2>/dev/null; then
        CC_TO_USE=clang
        CXX_TO_USE=clang++
        echo "Using clang $CLANG_VER"
    else
        CC_TO_USE=gcc
        CXX_TO_USE=g++
        echo "Using GCC (clang too old: $CLANG_VER)"
    fi
else
    CC_TO_USE=gcc
    CXX_TO_USE=g++
    echo "Using GCC (clang not found)"
fi

# Pin to LLVM 15.0.7 — builds with both clang and GCC
LLVM_REV="8dfdcc7b7bf66834a761bd8de445840ef68e4d1a"
CURRENT=$(cd "$SRC_DIR" && git rev-parse HEAD)
if [ "$CURRENT" != "$LLVM_REV" ]; then
    echo "Fetching LLVM 15.0.7..."
    (cd "$SRC_DIR" && git fetch --depth 1 origin "$LLVM_REV" && git checkout -f "$LLVM_REV")
fi

# Apply macOS pthread compatibility patches (idempotent)
PATCH="$(pwd)/$PATCH_DIR/libcxx-darwin-pthread-compat.patch"
if [ -f "$PATCH" ]; then
    if (cd "$SRC_DIR" && git apply --check --reverse "$PATCH" 2>/dev/null); then
        echo "Patch already applied, skipping."
    elif (cd "$SRC_DIR" && git apply --check "$PATCH" 2>/dev/null); then
        echo "Applying macOS pthread compatibility patch..."
        (cd "$SRC_DIR" && git apply "$PATCH")
    else
        echo "Warning: patch does not apply cleanly — may need updating for this LLVM version."
        echo "Continuing anyway (patches may already be applied manually)."
    fi
fi

echo "Configuring Apple-ABI libc++ build..."

cmake -G Ninja -S "$SRC_DIR/runtimes" -B "$BUILD_DIR" \
    -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
    -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC_TO_USE" \
    -DCMAKE_CXX_COMPILER="$CXX_TO_USE" \
    -DLIBCXX_ABI_VERSION=1 \
    -DLIBCXX_ABI_DEFINES="_LIBCPP_ABI_ALTERNATE_STRING_LAYOUT" \
    -DLIBCXX_ENABLE_SHARED=ON \
    -DLIBCXX_ENABLE_STATIC=OFF \
    -DLIBCXX_INCLUDE_TESTS=OFF \
    -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    -DLIBCXXABI_ENABLE_SHARED=ON \
    -DLIBCXXABI_ENABLE_STATIC=OFF \
    -DLIBCXXABI_INCLUDE_TESTS=OFF

echo ""
echo "Configuration complete. To build:"
echo "  cd $BUILD_DIR && ninja cxx cxxabi"
echo ""
echo "Output will be in $BUILD_DIR/lib/"
