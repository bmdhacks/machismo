#!/bin/bash
# Test: machismo runs hello_write, captures stdout
set -e
cd "$(dirname "$0")/.."
MACHISMO_ROOT="${MACHISMO_ROOT:-$(pwd)}"
BUILD_DIR="${BUILD_DIR:-$MACHISMO_ROOT/build}"
[ -f tests/fixtures/hello_write ] || bash tests/fixtures/build_fixtures.sh
output=$("$BUILD_DIR/machismo" tests/fixtures/hello_write 2>/dev/null)
[ "$output" = "hello" ]
