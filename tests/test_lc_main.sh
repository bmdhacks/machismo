#!/bin/bash
# Test: machismo handles LC_MAIN entry point correctly
set -e
cd "$(dirname "$0")/.."
[ -f tests/fixtures/with_lc_main ] || bash tests/fixtures/build_fixtures.sh
status=0
./machismo tests/fixtures/with_lc_main 2>/dev/null || status=$?
[ $status -eq 7 ]
