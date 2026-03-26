#!/bin/bash
# Test: NecroDancer loads with both libSystem shim and SDL2 trampoline active
set -e
cd "$(dirname "$0")/.."
MACHISMO_ROOT="${MACHISMO_ROOT:-$(pwd)}"
BUILD_DIR="${BUILD_DIR:-$MACHISMO_ROOT/build}"

BINARY=../necrodancer/depot_247086/NecroDancerSP.app/Contents/MacOS/NecroDancer
[ -f "$BINARY" ] || { echo "SKIP: NecroDancer binary not found"; exit 0; }
[ -f "$BUILD_DIR/libsystem_shim.so" ] || { echo "libsystem_shim.so not built"; exit 1; }

# Build temp config with absolute paths — the example configs use relative
# paths that only work from a port installation directory.
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Adapt the example dylib_map with absolute shim path
sed "s|./libsystem_shim.so|$BUILD_DIR/libsystem_shim.so|" \
    "$MACHISMO_ROOT/examples/necrodancer/dylib_map.conf" > "$TMPDIR/dylib_map.conf"

cat > "$TMPDIR/machismo.conf" <<EOF
[general]
dylib_map = $TMPDIR/dylib_map.conf

[trampoline.sdl2]
lib = libSDL2-2.0.so.0
prefix = _SDL_

[trampoline.steam_noop]
lib = STUB
prefix = __ZN3wos5steam
prefix = __ZNK3wos5steam
EOF

# Run with config. SDL_VIDEODRIVER=dummy suppresses GUI dialogs/windows.
# Timeout after 10s in case game reaches interactive state.
# Use SIGKILL directly — game threads can survive SIGTERM.
output=$(timeout -s KILL 10 env \
    MACHISMO_CONFIG="$TMPDIR/machismo.conf" \
    SDL_VIDEODRIVER=dummy \
    LD_LIBRARY_PATH="$BUILD_DIR" \
    "$BUILD_DIR/machismo" "$BINARY" 2>&1 || true)

# Verify libSystem.B loaded via shim (not stubbed)
echo "$output" | grep -q "libSystem.B.dylib.*libsystem_shim.so.*loaded"

# Verify SDL trampoline patched functions
echo "$output" | grep -q "trampoline.*patched.*from"

# Verify we resolved significantly more binds than with STUB
resolved=$(echo "$output" | grep -o '[0-9]* binds resolved' | grep -o '[0-9]*' | head -1)
[ "$resolved" -gt 3900 ]

echo "Integration: libSystem shim loaded, $resolved binds resolved, SDL trampolined"
