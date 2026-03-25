#!/bin/bash
# Test: does cmake build succeed?
set -e
cd "$(dirname "$0")/.."
MACHISMO_ROOT="${MACHISMO_ROOT:-$(pwd)}"
BUILD_DIR="${BUILD_DIR:-$MACHISMO_ROOT/build}"
cmake --build "$BUILD_DIR" 2>&1
