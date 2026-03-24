#!/bin/bash
# Test: trampoline patcher can redirect a Mach-O function to a native .so
set -e
cd "$(dirname "$0")/.."
[ -f tests/fixtures/trampoline_target ] || bash tests/fixtures/build_fixtures.sh

# Without trampoline: exits 42 (original static function)
# MACHISMO_CONFIG=none prevents picking up machismo.conf from CWD
status=0
MACHISMO_CONFIG=none ./machismo tests/fixtures/trampoline_target 2>/dev/null || status=$?
[ $status -eq 42 ]

# With trampoline: should exit 99 (native .so function called instead)
status=0
MACHISMO_CONFIG=none \
MACHISMO_TRAMPOLINE_LIB=tests/fixtures/libtest_native.so \
MACHISMO_TRAMPOLINE_PREFIX=_fake_lib_ \
    ./machismo tests/fixtures/trampoline_target 2>/dev/null || status=$?
[ $status -eq 99 ]
