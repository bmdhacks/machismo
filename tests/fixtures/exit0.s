; exit0.s - exit(0) via Linux syscall
.section __TEXT,__text
.globl _main
.p2align 2
_main:
    mov x0, #0
    mov x8, #93        ; __NR_exit
    svc #0
