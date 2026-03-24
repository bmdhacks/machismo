; trampoline_target.s — Mach-O with a static function that can be trampolined
; _fake_lib_func returns 42. After trampolining, native .so returns 99.
; The function must be at least 16 bytes (4 instructions) for the trampoline.
.section __TEXT,__text

.globl _fake_lib_func
.p2align 2
_fake_lib_func:
    mov w0, #42
    nop
    nop
    ret

.globl _main
.p2align 2
_main:
    stp x29, x30, [sp, #-16]!
    bl _fake_lib_func
    ; exit with return value
    mov x8, #93
    svc #0
