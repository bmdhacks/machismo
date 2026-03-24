#!/bin/bash
# Test: correct exit code for exit(0)
set -e
cd "$(dirname "$0")/.."
[ -f tests/fixtures/exit0 ] || bash tests/fixtures/build_fixtures.sh
./machismo tests/fixtures/exit0 2>/dev/null
status=$?
[ $status -eq 0 ]
