#!/bin/bash
# Test: resolver reads dylib_map.conf and maps libraries correctly
set -e
cd "$(dirname "$0")/.."
MACHISMO_ROOT="${MACHISMO_ROOT:-$(pwd)}"
BUILD_DIR="${BUILD_DIR:-$MACHISMO_ROOT/build}"

NECRO_BIN="../necrodancer/depot_247086/NecroDancerSP.app/Contents/MacOS/NecroDancer"

if [ ! -f "$NECRO_BIN" ]; then
    echo "SKIP: NecroDancer binary not found"
    exit 0
fi

# Build a temp config with absolute paths so dylib_map.conf resolves correctly
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT
cat > "$TMPDIR/machismo.conf" <<EOF
[general]
dylib_map = $MACHISMO_ROOT/examples/necrodancer/dylib_map.conf
EOF

output=$(MACHISMO_CONFIG="$TMPDIR/machismo.conf" \
    SDL_VIDEODRIVER=dummy \
    LD_LIBRARY_PATH="$BUILD_DIR" \
    "$BUILD_DIR/machismo" "$NECRO_BIN" 2>&1 || true)

# Should load the mapping config
echo "$output" | grep -q "loaded.*dylib mappings"

# Should successfully load libraries that exist on this system
echo "$output" | grep -q "libz.1.dylib.*loaded"
echo "$output" | grep -q "libfreetype.*loaded"

# Should stub Steam and Galaxy
echo "$output" | grep -q "libsteam_api.*stubbed"
echo "$output" | grep -q "libGalaxy.*stubbed"

# Should skip macOS frameworks
echo "$output" | grep -q "CoreFoundation.*skipped"
echo "$output" | grep -q "AppKit.*skipped"
