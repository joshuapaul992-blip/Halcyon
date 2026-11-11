; Halcyon OS - kernel entry point and interrupt stubs (x86_64)
bits 64

extern kmain
extern isr_dispatch
extern _kernel_file_size
extern _bss_end

section .header
global kernel_header
kernel_header:
    dd 0x4E524B48          ; "HKRN"
    dd 1
    dq 0x100000            ; load address
    dq _kernel_file_size
    dq _bss_end
    dq _start

section .text
global _start
_start:
    cli
    cld
    mov rsp, boot_stack_top
    xor rbp, rbp
    ; enable SSE: CR0.EM=0, CR0.MP=1, CR4.OSFXSR|OSXMMEXCPT
    mov rax, cr0
    and ax, 0xFFFB
    or ax, 0x0002
    mov cr0, rax
    mov rax, cr4
    or ax, 0x0600
    mov cr4, rax
    fninit
    call kmain
.hang:
    cli
    hlt
    jmp .hang

; ---- exception / interrupt stubs ----
%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0
    push %1
    jmp isr_common
%endmacro
%macro ISR_ERR 1
global isr%1
isr%1:
    push %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
ISR_NOERR 32   ; spurious / generic

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    mov rbx, rsp
    and rsp, -16
    call isr_dispatch
    mov rsp, rbx
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

section .data
global isr_table
isr_table:
%assign i 0
%rep 33
    dq isr %+ i
%assign i i+1
%endrep

section .bss
align 16
boot_stack:
    resb 65536
boot_stack_top:
