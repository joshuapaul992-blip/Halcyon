/* Halcyon program entry: rdi = the command line ("NAME.EXE arg arg") */
    .text
    .globl _start
_start:
    xor %ebp, %ebp
    and $-16, %rsp
    call __hal_start        /* parses the command line, calls main, exits */
    hlt
    .section .note.GNU-stack,"",@progbits
