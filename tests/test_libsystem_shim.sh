#!/bin/bash
# Test: libsystem_shim.so can be loaded and key Apple-specific symbols resolve
set -e
cd "$(dirname "$0")/.."

[ -f libsystem_shim.so ] || { echo "libsystem_shim.so not built"; exit 1; }

# Test that the .so loads without errors
LD_LIBRARY_PATH=. python3 -c "
import ctypes, sys

lib = ctypes.CDLL('./libsystem_shim.so')

# mach_absolute_time should return a nonzero nanosecond timestamp
lib.mach_absolute_time.restype = ctypes.c_uint64
t1 = lib.mach_absolute_time()
t2 = lib.mach_absolute_time()
assert t1 > 0, f'mach_absolute_time returned 0'
assert t2 >= t1, f'mach_absolute_time went backwards: {t2} < {t1}'

# mach_timebase_info should return {1, 1}
class TimebaseInfo(ctypes.Structure):
    _fields_ = [('numer', ctypes.c_uint32), ('denom', ctypes.c_uint32)]
info = TimebaseInfo()
lib.mach_timebase_info(ctypes.byref(info))
assert info.numer == 1, f'timebase numer={info.numer}, expected 1'
assert info.denom == 1, f'timebase denom={info.denom}, expected 1'

# __error should return a valid pointer (errno location)
lib.__error.restype = ctypes.POINTER(ctypes.c_int)
p = lib.__error()
assert p, '__error returned NULL'

# __sincosf_stret should compute sin/cos correctly
class Float2(ctypes.Structure):
    _fields_ = [('sinval', ctypes.c_float), ('cosval', ctypes.c_float)]
lib.__sincosf_stret.restype = Float2
result = lib.__sincosf_stret(ctypes.c_float(0.0))
assert abs(result.sinval - 0.0) < 1e-6, f'sin(0)={result.sinval}'
assert abs(result.cosval - 1.0) < 1e-6, f'cos(0)={result.cosval}'

# __exp10f
lib.__exp10f.restype = ctypes.c_float
val = lib.__exp10f(ctypes.c_float(2.0))
assert abs(val - 100.0) < 0.01, f'__exp10f(2)={val}, expected 100'

# _NSGetExecutablePath should return something via /proc/self/exe
buf = ctypes.create_string_buffer(1024)
bufsize = ctypes.c_uint32(1024)
ret = lib._NSGetExecutablePath(buf, ctypes.byref(bufsize))
assert ret == 0, f'_NSGetExecutablePath returned {ret}'
path = buf.value.decode()
assert len(path) > 0, '_NSGetExecutablePath returned empty path'

# stdio globals should be non-null after constructor runs
# (ctypes can't easily check FILE* globals, but we can check they exist)
# Just verify the symbols exist
assert hasattr(lib, '__stdinp'), 'missing __stdinp'
assert hasattr(lib, '__stdoutp'), 'missing __stdoutp'
assert hasattr(lib, '__stderrp'), 'missing __stderrp'

# memset_pattern4
buf = ctypes.create_string_buffer(16)
pattern = (ctypes.c_uint8 * 4)(0xDE, 0xAD, 0xBE, 0xEF)
lib.memset_pattern4(buf, pattern, 16)
assert buf.raw == b'\\xDE\\xAD\\xBE\\xEF' * 4, f'memset_pattern4 failed: {buf.raw.hex()}'

print('All libsystem_shim symbol tests passed')
"
