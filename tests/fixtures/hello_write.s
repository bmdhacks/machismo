; hello_write.s - write "hello\n" to stdout, then exit(0)
.section __TEXT,__text
.globl _main
.p2align 2
_main:
    ; write(1, msg, 6)
    mov x0, #1         ; fd = stdout
    adrp x1, msg@PAGE
    add x1, x1, msg@PAGEOFF
    mov x2, #6         ; length
    mov x8, #64        ; __NR_write on aarch64 Linux
    svc #0

    ; exit(0)
    mov x0, #0
    mov x8, #93        ; __NR_exit
    svc #0

.section __TEXT,__cstring
msg:
    .asciz "hello\n"
