#!/bin/bash
# Test: does make succeed?
set -e
cd "$(dirname "$0")/.."
make clean >/dev/null 2>&1
make 2>&1
