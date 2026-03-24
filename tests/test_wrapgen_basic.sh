#!/bin/bash
# Test: wrapgen parses aarch64 ELF and emits valid C
set -e
cd "$(dirname "$0")/.."

tmpdir=$(mktemp -d)
trap "rm -rf $tmpdir" EXIT

./wrapgen /usr/lib64/libz.so "$tmpdir/wrapper.c" "$tmpdir/wrapper.h"

# Output file should exist and contain C code
[ -f "$tmpdir/wrapper.c" ]
grep -q "elfcalls" "$tmpdir/wrapper.c"
grep -q "dlsym_fatal" "$tmpdir/wrapper.c"
grep -q "dlopen_fatal" "$tmpdir/wrapper.c"
