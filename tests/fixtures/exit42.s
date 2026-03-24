; exit42.s - simplest arm64 Mach-O test: exit(42) via Linux syscall
.section __TEXT,__text
.globl _main
.p2align 2
_main:
    mov x0, #42
    mov x8, #93        ; __NR_exit on aarch64 Linux
    svc #0
