#!/bin/bash
# Test: resolver applies rebase fixups (ASLR pointer corrections)
set -e
cd "$(dirname "$0")/.."
MACHISMO_ROOT="${MACHISMO_ROOT:-$(pwd)}"
BUILD_DIR="${BUILD_DIR:-$MACHISMO_ROOT/build}"

NECRO_BIN="../necrodancer/depot_247086/NecroDancerSP.app/Contents/MacOS/NecroDancer"

if [ ! -f "$NECRO_BIN" ]; then
    echo "SKIP: NecroDancer binary not found"
    exit 0
fi

output=$(MACHISMO_DYLIB_MAP=dylib_map.conf LD_LIBRARY_PATH="$BUILD_DIR" "$BUILD_DIR/machismo" "$NECRO_BIN" 2>&1 || true)

# Should have applied a significant number of rebases
# NecroDancer has ~23000 rebase fixups
rebase_count=$(echo "$output" | grep -oP '\d+ rebases' | grep -oP '^\d+' | head -1)
[ "$rebase_count" -gt 1000 ]
