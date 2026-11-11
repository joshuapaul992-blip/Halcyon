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

; ---- exception / interrupt stubs (all 256 vectors) ----
; Exceptions that push an error code: 8, 10-14, 17, 21, 29, 30.
%macro ISR 1
global isr%1
isr%1:
%if %1 = 8 || (%1 >= 10 && %1 <= 14) || %1 = 17 || %1 = 21 || %1 = 29 || %1 = 30
%else
    push 0
%endif
    push %1
    jmp isr_common
%endmacro

%assign i 0
%rep 256
ISR i
%assign i i+1
%endrep

; Interrupt gates run with IF=0, so handlers never nest. The C handler may use
; SSE registers (the kernel is built with SSE), so the full FPU/SSE state is
; saved around it.
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
    mov rbx, rsp
    and rsp, -16
    sub rsp, 512
    fxsave [rsp]
    mov rdi, rbx
    call isr_dispatch
    fxrstor [rsp]
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

; ---- thread switch (core/thread.c) ----
; void ctx_switch(u64 *save_rsp, u64 new_rsp)
; Saves what a function call must preserve (callee-saved registers, RFLAGS,
; MXCSR and the x87 control word) on the old stack, switches stacks, restores.
global ctx_switch
ctx_switch:
    pushfq
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    stmxcsr [rsp]
    fnstcw [rsp + 4]
    mov [rdi], rsp
    mov rsp, rsi
    ldmxcsr [rsp]
    fldcw [rsp + 4]
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    popfq
    ret

; first instruction of a new thread (its initial frame returns here)
extern thread_start
global thread_entry
thread_entry:
    and rsp, -16
    call thread_start
.halt:
    hlt
    jmp .halt

section .data
global isr_table
isr_table:
%assign i 0
%rep 256
    dq isr %+ i
%assign i i+1
%endrep

section .bss
align 16
boot_stack:
    resb 65536
boot_stack_top:
