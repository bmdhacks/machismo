#!/bin/bash
#
# Build SFML 2.5.1 with Apple-ABI libc++ for Mach-O game compatibility.
# Links against build-libcxx/lib/libc++.so.1 (same string ABI as game binary).
#
set -e

# Always work from the project root
cd "$(dirname "$0")/.."

SRCDIR="extern/sfml"
BUILDDIR="build-sfml"
LIBCXX_DIR="$(pwd)/build-libcxx"

mkdir -p "$BUILDDIR"

# Set both C and CXX compilers to clang so -nostdlib++ linker flag
# doesn't fail cmake's C compiler test. Add cmake policy for cmake 4.x compat.
cmake -S "$SRCDIR" -B "$BUILDDIR" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_CXX_FLAGS="-nostdinc++ -include $(pwd)/extern/sfml_compat.h -I${LIBCXX_DIR}/include/c++/v1 -fPIC -D_LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR" \
    -DCMAKE_EXE_LINKER_FLAGS="-nostdlib++ -L${LIBCXX_DIR}/lib -Wl,-rpath,${LIBCXX_DIR}/lib -lc++" \
    -DCMAKE_SHARED_LINKER_FLAGS="-nostdlib++ -L${LIBCXX_DIR}/lib -Wl,-rpath,${LIBCXX_DIR}/lib -lc++" \
    -DBUILD_SHARED_LIBS=ON \
    -DSFML_BUILD_EXAMPLES=OFF \
    -DSFML_BUILD_DOC=OFF \
    -DSFML_BUILD_AUDIO=ON \
    -DSFML_BUILD_GRAPHICS=ON \
    -DSFML_BUILD_WINDOW=ON \
    -DSFML_BUILD_NETWORK=ON

cmake --build "$BUILDDIR" -j$(nproc)

# Strip X11 DT_NEEDED entries from graphics/window.
# The game uses bgfx for rendering (not SFML's OpenGL backend), so the X11
# code paths are never called. Removing the deps lets these .so files load
# on KMSDRM handhelds that don't have X11 installed.
if command -v patchelf >/dev/null 2>&1; then
    for lib in "$BUILDDIR"/lib/libsfml-graphics.so.2.5.* "$BUILDDIR"/lib/libsfml-window.so.2.5.*; do
        [ -f "$lib" ] || continue
        patchelf --remove-needed libX11.so.6 --remove-needed libXrandr.so.2 "$lib" 2>/dev/null && \
            echo "Stripped X11 deps from $(basename "$lib")"
    done
fi

echo "=== SFML built ==="
ls -lh "$BUILDDIR"/lib/libsfml-*.so*
echo "libc++ linkage:"
ldd "$BUILDDIR"/lib/libsfml-system.so.2.5.1 | grep "c++"
