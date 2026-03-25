#!/bin/bash
# Test: machismo handles LC_MAIN entry point correctly
set -e
cd "$(dirname "$0")/.."
MACHISMO_ROOT="${MACHISMO_ROOT:-$(pwd)}"
BUILD_DIR="${BUILD_DIR:-$MACHISMO_ROOT/build}"
[ -f tests/fixtures/with_lc_main ] || bash tests/fixtures/build_fixtures.sh
status=0
"$BUILD_DIR/machismo" tests/fixtures/with_lc_main 2>/dev/null || status=$?
[ $status -eq 7 ]
