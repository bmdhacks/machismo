; with_lc_main.s - arm64 Mach-O using LC_MAIN entry point
; When linked with -lSystem (or with a crt stub), ld64.lld produces LC_MAIN
; instead of LC_UNIXTHREAD.
.section __TEXT,__text
.globl _main
.p2align 2
_main:
    mov x0, #7
    mov x8, #93        ; __NR_exit
    svc #0
