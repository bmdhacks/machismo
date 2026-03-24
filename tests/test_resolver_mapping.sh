#!/bin/bash
# Test: resolver reads dylib_map.conf and maps libraries correctly
set -e
cd "$(dirname "$0")/.."

NECRO_BIN="../necrodancer/depot_247086/NecroDancerSP.app/Contents/MacOS/NecroDancer"

if [ ! -f "$NECRO_BIN" ]; then
    echo "SKIP: NecroDancer binary not found"
    exit 0
fi

output=$(MACHISMO_DYLIB_MAP=dylib_map.conf ./machismo "$NECRO_BIN" 2>&1 || true)

# Should load the mapping config
echo "$output" | grep -q "loaded.*dylib mappings"

# Should successfully load libraries that exist on this system
echo "$output" | grep -q "libz.1.dylib.*loaded"
echo "$output" | grep -q "libfreetype.*loaded"
echo "$output" | grep -q "libluajit.*loaded"

# Should stub Steam and Galaxy
echo "$output" | grep -q "libsteam_api.*stubbed"
echo "$output" | grep -q "libGalaxy.*stubbed"

# Should skip macOS frameworks
echo "$output" | grep -q "CoreFoundation.*skipped"
echo "$output" | grep -q "AppKit.*skipped"
