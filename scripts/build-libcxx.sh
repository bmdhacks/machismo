#!/bin/bash
# Build Apple-ABI-compatible libc++ for aarch64 Linux.
#
# This produces a libc++.so.1 with Apple's std::string layout
# (_LIBCPP_ABI_ALTERNATE_STRING_LAYOUT) so Mach-O binaries compiled
# against Apple's libc++ get the correct memory layout.
#
# Usage: ./scripts/build-libcxx.sh
# Output: build-libcxx/lib/libc++.so.1

set -e

# Always work from the project root
cd "$(dirname "$0")/.."

BUILD_DIR=build-libcxx
SRC_DIR=extern/llvm-project

if [ ! -d "$SRC_DIR/libcxx" ]; then
    echo "Error: $SRC_DIR/libcxx not found. Run: git submodule update --init extern/llvm-project"
    exit 1
fi

echo "Configuring Apple-ABI libc++ build..."

cmake -G Ninja -S "$SRC_DIR/runtimes" -B "$BUILD_DIR" \
    -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
    -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLIBCXX_ABI_VERSION=1 \
    -DLIBCXX_ABI_DEFINES="_LIBCPP_ABI_ALTERNATE_STRING_LAYOUT" \
    -DLIBCXX_ENABLE_SHARED=ON \
    -DLIBCXX_ENABLE_STATIC=OFF \
    -DLIBCXX_ENABLE_EXPERIMENTAL_LIBRARY=OFF \
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
