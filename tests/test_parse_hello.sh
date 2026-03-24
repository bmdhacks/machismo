#!/bin/bash
# Test: machismo runs hello_write, captures stdout
set -e
cd "$(dirname "$0")/.."
[ -f tests/fixtures/hello_write ] || bash tests/fixtures/build_fixtures.sh
output=$(./machismo tests/fixtures/hello_write 2>/dev/null)
[ "$output" = "hello" ]
