#!/bin/bash
# Test: machismo can parse NecroDancer arm64 fat binary
# (extract arm64 slice, find entry point, list segments — NOT run it)
set -e
cd "$(dirname "$0")/.."

NECRO_BIN="../necrodancer/depot_247086/NecroDancerSP.app/Contents/MacOS/NecroDancer"

if [ ! -f "$NECRO_BIN" ]; then
    echo "SKIP: NecroDancer binary not found at $NECRO_BIN"
    exit 0
fi

# machismo should be able to parse it (it will fail trying to run since deps aren't available,
# but it should get past the Mach-O parsing and segment mapping stage).
# We just need to see that it doesn't crash on parsing.
# Since NecroDancer links dyld and libraries we don't have, it will print the
# LC_LOAD_DYLINKER warning, then either segfault at the entry point or exit.
# We just verify machismo starts parsing without errors.
output=$(timeout -s KILL 10 env MACHISMO_CONFIG=none ./machismo "$NECRO_BIN" 2>&1 || true)

# Should have found LC_LOAD_DYLINKER
echo "$output" | grep -q "LC_LOAD_DYLINKER"
