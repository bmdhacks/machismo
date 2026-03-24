// fat_stub_x86.s - x86_64 stub that exits with 99 (should not be selected on arm64)
.section __TEXT,__text
.globl _main
.p2align 2
_main:
    movl $60, %eax
    movl $99, %edi
    syscall
