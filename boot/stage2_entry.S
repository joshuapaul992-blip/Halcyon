/* Halcyon OS - stage2 entry, BIOS call thunk and long-mode switch.
 * Runs at 0000:8000 in real mode. C code (built with gcc -m16) runs in
 * "unreal" mode: DS/ES/FS/GS have 4 GiB limits so 32-bit pointers work. */

    .code16
    .section .entry, "ax"
    .global _start
_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $0x7C00, %esp
    ljmp $0, $1f
1:
    movzbl %dl, %edx
    movl %edx, boot_drive_asm

    /* Enable A20: BIOS first, then fast A20 */
    movw $0x2401, %ax
    int $0x15
    inb $0x92, %al
    testb $2, %al
    jnz 2f
    orb $2, %al
    andb $0xFE, %al
    outb %al, $0x92
2:
    calll unreal

    /* clear .bss */
    movl $__bss_start, %edi
    movl $__bss_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    cld
    addr32 rep stosb

    movl boot_drive_asm, %edx
    pushl %edx
    calll stage2_main
3:  cli
    hlt
    jmp 3b

/* void unreal(void): (re)load 4 GiB limits into DS/ES/FS/GS, enable ints */
    .global unreal
unreal:
    cli
    pushl %eax
    pushl %ebx
    pushw %ds
    pushw %es
    lgdtl gdt_desc
    movl %cr0, %eax
    orb $1, %al
    movl %eax, %cr0
    jmp 1f
1:  movw $0x10, %bx
    movw %bx, %ds
    movw %bx, %es
    movw %bx, %fs
    movw %bx, %gs
    andb $0xFE, %al
    movl %eax, %cr0
    jmp 2f
2:  popw %es
    popw %ds
    xorw %bx, %bx
    movw %bx, %fs
    movw %bx, %gs
    popl %ebx
    popl %eax
    sti
    retl

/* void bios_int(uint32_t intno): registers in/out via global `bregs` */
    .global bios_int
bios_int:
    movl 4(%esp), %eax
    movb %al, (int_insn + 1)
    jmp 1f
1:  pushal
    pushw %ds
    pushw %es
    movw (bregs + 26), %ax
    movw %ax, %es
    movl (bregs + 0),  %eax
    movl (bregs + 4),  %ebx
    movl (bregs + 8),  %ecx
    movl (bregs + 12), %edx
    movl (bregs + 16), %esi
    movl (bregs + 20), %edi
int_insn:
    int $0x00
    pushfl
    popl (bregs + 28)
    movl %eax, (bregs + 0)
    movl %ebx, (bregs + 4)
    movl %ecx, (bregs + 8)
    movl %edx, (bregs + 12)
    movl %esi, (bregs + 16)
    movl %edi, (bregs + 20)
    movw %es, %ax
    movw %ax, (bregs + 26)
    popw %es
    popw %ds
    popal
    calll unreal
    retl

/* void enter_long(uint32_t pml4, uint32_t entry, uint32_t bootinfo) */
    .global enter_long
enter_long:
    cli
    movl 4(%esp), %eax
    movl 8(%esp), %ebx
    movl 12(%esp), %esi
    movl %eax, %cr3
    movl %cr4, %eax
    orl $0x20, %eax              /* PAE */
    movl %eax, %cr4
    movl $0xC0000080, %ecx       /* EFER */
    rdmsr
    orl $0x100, %eax             /* LME */
    wrmsr
    lgdtl gdt_desc
    movl %cr0, %eax
    orl $0x80000001, %eax        /* PG | PE */
    movl %eax, %cr0
    ljmpl $0x08, $long_entry

    .code64
long_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movq $0x7C00, %rsp
    movl %esi, %edi
    movl %ebx, %eax
    jmp *%rax

    .code16
    .section .data
    .align 8
gdt:
    .quad 0
    .quad 0x00AF9A000000FFFF     /* 0x08: 64-bit code */
    .quad 0x00CF92000000FFFF     /* 0x10: flat 32-bit data */
    .quad 0x00CF9A000000FFFF     /* 0x18: flat 32-bit code */
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt
boot_drive_asm:
    .long 0
