#!/bin/bash
# Test: correct exit code for exit(0)
set -e
cd "$(dirname "$0")/.."
MACHISMO_ROOT="${MACHISMO_ROOT:-$(pwd)}"
BUILD_DIR="${BUILD_DIR:-$MACHISMO_ROOT/build}"
[ -f tests/fixtures/exit0 ] || bash tests/fixtures/build_fixtures.sh
"$BUILD_DIR/machismo" tests/fixtures/exit0 2>/dev/null
status=$?
[ $status -eq 0 ]
