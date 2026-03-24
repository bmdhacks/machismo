#!/bin/bash
# Test: machismo can parse and run static arm64 Mach-O (exit42)
set -e
cd "$(dirname "$0")/.."
[ -f tests/fixtures/exit42 ] || bash tests/fixtures/build_fixtures.sh
status=0
./machismo tests/fixtures/exit42 2>/dev/null || status=$?
[ $status -eq 42 ]
