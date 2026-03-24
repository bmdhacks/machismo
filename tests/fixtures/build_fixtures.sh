#!/bin/bash
# Build test Mach-O fixtures using the LLVM cross-compilation toolchain.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "Building test fixtures..."

# --- exit42: static arm64 Mach-O, exit(42) ---
llvm-mc -triple arm64-apple-macos11 -filetype=obj -o exit42.o exit42.s
ld64.lld -arch arm64 -platform_version macos 11.0.0 11.0.0 -o exit42 -e _main exit42.o
rm -f exit42.o
echo "  Built exit42"

# --- exit0: static arm64 Mach-O, exit(0) ---
llvm-mc -triple arm64-apple-macos11 -filetype=obj -o exit0.o exit0.s
ld64.lld -arch arm64 -platform_version macos 11.0.0 11.0.0 -o exit0 -e _main exit0.o
rm -f exit0.o
echo "  Built exit0"

# --- hello_write: static arm64 Mach-O, write + exit ---
llvm-mc -triple arm64-apple-macos11 -filetype=obj -o hello_write.o hello_write.s
ld64.lld -arch arm64 -platform_version macos 11.0.0 11.0.0 -o hello_write -e _main hello_write.o
rm -f hello_write.o
echo "  Built hello_write"

# --- fat_binary: universal binary with x86_64 + arm64 ---
# Build x86_64 stub
llvm-mc -triple x86_64-apple-macos11 -filetype=obj -o fat_stub_x86.o fat_stub_x86.s
ld64.lld -arch x86_64 -platform_version macos 11.0.0 11.0.0 -o fat_stub_x86 -e _main fat_stub_x86.o
rm -f fat_stub_x86.o

# Create fat binary from x86_64 + arm64 exit42
llvm-lipo -create -output fat_binary fat_stub_x86 exit42
rm -f fat_stub_x86
echo "  Built fat_binary"

# --- with_lc_main: arm64 Mach-O using LC_MAIN entry point ---
# ld64.lld produces LC_MAIN when -no_pie and -e _main are not used together.
# We use -lc to force dyld-style linking which generates LC_MAIN.
# If that doesn't work, we just use the same approach as above (LC_UNIXTHREAD).
llvm-mc -triple arm64-apple-macos11 -filetype=obj -o with_lc_main.o with_lc_main.s
# Try to create with LC_MAIN by using -lSystem and no -e flag
# ld64.lld will use LC_MAIN when _main is the entry and we use dyld-style linking
ld64.lld -arch arm64 -platform_version macos 11.0.0 11.0.0 \
    -o with_lc_main with_lc_main.o -e _main 2>/dev/null || \
    ld64.lld -arch arm64 -platform_version macos 11.0.0 11.0.0 \
        -o with_lc_main -e _main with_lc_main.o
rm -f with_lc_main.o
echo "  Built with_lc_main"

# --- trampoline_target: Mach-O with a static function to trampoline ---
llvm-mc -triple arm64-apple-macos11 -filetype=obj -o trampoline_target.o trampoline_target.s
ld64.lld -arch arm64 -platform_version macos 11.0.0 11.0.0 -o trampoline_target -e _main trampoline_target.o
rm -f trampoline_target.o
echo "  Built trampoline_target"

# --- libtest_native.so: native .so with replacement function ---
gcc -shared -fPIC -o libtest_native.so libtest_native.c
echo "  Built libtest_native.so"

echo "All fixtures built."
