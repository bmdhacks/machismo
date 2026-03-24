#!/bin/bash
# Test: wrapgen output contains expected symbol names from libz
set -e
cd "$(dirname "$0")/.."

tmpdir=$(mktemp -d)
trap "rm -rf $tmpdir" EXIT

./wrapgen /usr/lib64/libz.so "$tmpdir/wrapper.c" "$tmpdir/wrapper.h"

# libz should have these well-known symbols
grep -q "compress" "$tmpdir/wrapper.c"
grep -q "uncompress" "$tmpdir/wrapper.c"
grep -q "inflate" "$tmpdir/wrapper.c"
grep -q "deflate" "$tmpdir/wrapper.c"
grep -q "adler32" "$tmpdir/wrapper.c"
