#!/bin/bash
# Test: machismo extracts arm64 slice from fat (universal) binary
set -e
cd "$(dirname "$0")/.."
MACHISMO_ROOT="${MACHISMO_ROOT:-$(pwd)}"
BUILD_DIR="${BUILD_DIR:-$MACHISMO_ROOT/build}"
[ -f tests/fixtures/fat_binary ] || bash tests/fixtures/build_fixtures.sh

# The fat binary has x86_64 (exits 99) and arm64 (exits 42).
# On aarch64, machismo should select the arm64 slice and exit 42.
status=0
"$BUILD_DIR/machismo" tests/fixtures/fat_binary 2>/dev/null || status=$?
[ $status -eq 42 ]
