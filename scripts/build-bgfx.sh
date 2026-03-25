#!/bin/bash
#
# Build native Linux aarch64 libbgfx-shared.so from bgfx/bx/bimg submodules.
# Uses amalgamated build — no GENie/premake required.
#
# Submodules must be pinned to matching versions:
#   bgfx: 36ec932f4 (API v118, 2022-10-29)
#   bx:   20efa22
#   bimg:  1955d8f
#
set -e

# Always work from the project root
cd "$(dirname "$0")/.."

CXX="${CXX:-g++}"
CXXFLAGS="-O0 -g -fPIC -std=c++14"
OUTDIR="build-bgfx"

mkdir -p "$OUTDIR"

# Apply buffer orphaning patch (proper GL orphaning instead of destroy+create)
ORPHAN_PATCH="patches/bgfx-buffer-orphan.patch"
if [ -f "$ORPHAN_PATCH" ]; then
    if git -C extern/bgfx diff --quiet src/renderer_gl.h 2>/dev/null && \
       git -C extern/bgfx diff --cached --quiet src/renderer_gl.h 2>/dev/null; then
        echo "Applying bgfx buffer orphaning patch..."
        git -C extern/bgfx apply "../../$ORPHAN_PATCH"
    else
        echo "bgfx renderer_gl.h already modified, skipping patch"
    fi
fi

# Use Apple-ABI libc++ so std::string layout matches the Mach-O game binary.
# The game was built with _LIBCPP_ABI_ALTERNATE_STRING_LAYOUT (Apple's SSO).
LIBCXX_INCLUDES="-nostdinc++ -I build-libcxx/include/c++/v1"
# -nostdlib++ is clang-only; use explicit libs for GCC compatibility (Bullseye)
LIBCXX_LINK="-L build-libcxx/lib -Wl,-rpath,\$ORIGIN/../build-libcxx/lib -lc++ -lc++abi -nodefaultlibs -lc -lm -lgcc_s -lgcc"

BX_INCLUDES="-I extern/bx/include -I extern/bx/include/compat/linux -I extern/bx/3rdparty"
BIMG_INCLUDES="-I extern/bimg/include -I extern/bx/include -I extern/bx/include/compat/linux -I extern/bimg/3rdparty -I extern/bimg/3rdparty/astc-encoder/include -I extern/bimg/3rdparty/tinyexr/deps/miniz -I extern/bimg/3rdparty/iqa/include"
BGFX_INCLUDES="-I extern/bgfx/include -I extern/bgfx/src -I extern/bx/include -I extern/bx/include/compat/linux -I extern/bimg/include -I extern/bimg/3rdparty -I extern/bgfx/3rdparty -I extern/bgfx/3rdparty/khronos"

BGFX_DEFINES="-DBGFX_SHARED_LIB_BUILD=1"
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_RENDERER_OPENGLES=31"
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_RENDERER_OPENGL=0"
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_RENDERER_VULKAN=0"
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_RENDERER_METAL=0"
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_RENDERER_DIRECT3D9=0"
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_RENDERER_DIRECT3D11=0"
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_RENDERER_DIRECT3D12=0"
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_RENDERER_AGC=0"
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_RENDERER_GNM=0"
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_RENDERER_NVN=0"
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_RENDERER_WEBGPU=0"

# Memory reduction for Mali G31 (1GB shared RAM, no VRAM).
# RenderDoc frame analysis: 3 draw calls, 67KB transient VB, 0 IB, 1 texture.
# Defaults are tuned for desktop GPUs with GB of VRAM — wildly oversized here.
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_TRANSIENT_VERTEX_BUFFER_SIZE='(1<<20)'"     # 6MB → 1MB
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_TRANSIENT_INDEX_BUFFER_SIZE='(64<<10)'"     # 2MB → 64KB
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_DYNAMIC_VERTEX_BUFFER_SIZE='(128<<10)'"     # 3MB → 128KB
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_DYNAMIC_INDEX_BUFFER_SIZE='(64<<10)'"       # 1MB → 64KB
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_MAX_DRAW_CALLS=256"                         # 65535 → 256
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_MAX_TEXTURES=64"                            # 4096 → 64
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_MAX_VERTEX_BUFFERS=64"                      # 4096 → 64
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_MAX_INDEX_BUFFERS=64"                       # 4096 → 64
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_MAX_DYNAMIC_VERTEX_BUFFERS=16"              # 4096 → 16
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_MAX_DYNAMIC_INDEX_BUFFERS=16"               # 4096 → 16
BGFX_DEFINES="$BGFX_DEFINES -DBGFX_CONFIG_DEFAULT_MAX_ENCODERS=1"                     # 8 → 1

echo "=== Building bx ==="
$CXX $CXXFLAGS $LIBCXX_INCLUDES $BX_INCLUDES -DBX_CONFIG_DEBUG=0 \
    -c extern/bx/src/amalgamated.cpp -o "$OUTDIR/bx.o"

echo "=== Building bimg ==="
for src in image.cpp image_decode.cpp image_encode.cpp image_gnf.cpp; do
    name="${src%.cpp}"
    $CXX $CXXFLAGS $LIBCXX_INCLUDES $BIMG_INCLUDES -DBX_CONFIG_DEBUG=0 \
        -c "extern/bimg/src/$src" -o "$OUTDIR/bimg_${name}.o"
done

echo "=== Building bgfx ==="
# Can't use amalgamated.cpp directly because X11's #define None 0L
# conflicts with enum None in shader_dxbc.h. Compile needed sources individually.
BGFX_SRCS=(
    extern/bgfx/src/bgfx.cpp
    extern/bgfx/src/debug_renderdoc.cpp
    extern/bgfx/src/glcontext_egl.cpp
    extern/bgfx/src/renderer_gl.cpp
    extern/bgfx/src/renderer_noop.cpp
    extern/bgfx/src/shader.cpp
    extern/bgfx/src/shader_spirv.cpp
    extern/bgfx/src/topology.cpp
    extern/bgfx/src/vertexlayout.cpp
)
for src in "${BGFX_SRCS[@]}"; do
    name="$(basename "${src%.cpp}")"
    echo "  $name"
    $CXX $CXXFLAGS $LIBCXX_INCLUDES $BGFX_INCLUDES $BGFX_DEFINES -DBX_CONFIG_DEBUG=0 \
        -c "$src" -o "$OUTDIR/bgfx_${name}.o"
done

echo "=== Building renderer stubs ==="
$CXX $CXXFLAGS $LIBCXX_INCLUDES $BGFX_INCLUDES -DBX_CONFIG_DEBUG=0 \
    -c "extern/renderer_stubs.cpp" -o "$OUTDIR/renderer_stubs.o"

echo "=== Linking libbgfx-shared.so ==="
$CXX -shared -o "$OUTDIR/libbgfx-shared.so" \
    "$OUTDIR/bx.o" "$OUTDIR"/bimg_*.o "$OUTDIR"/bgfx_*.o "$OUTDIR/renderer_stubs.o" \
    $LIBCXX_LINK -lEGL -lGLESv2 -lpthread -ldl -lrt

echo "=== Done ==="
ls -lh "$OUTDIR/libbgfx-shared.so"
echo "Exported bgfx symbols:"
nm -D "$OUTDIR/libbgfx-shared.so" | grep -c " T.*bgfx"
