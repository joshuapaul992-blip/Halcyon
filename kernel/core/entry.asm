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
    cld                             ; interrupt gates keep the interrupted DF (user mode can set it)
    mov rbx, rsp
    and rsp, -16
    sub rsp, 512
    fxsave [rsp]
    ldmxcsr [rel kernel_mxcsr]      ; the kernel's own SSE / x87 control state,
    fldcw [rel kernel_fcw]          ; not the interrupted program's
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

; ---- system calls (SYSCALL) ----
; In: rax = number, rdi rsi rdx r10 r8 r9 = arguments; rcx = user RIP,
; r11 = user RFLAGS, interrupts off (FMASK). The user stack pointer is parked
; in a global while switching stacks (one CPU, interrupts still off).
extern g_kstack_top
extern syscall_dispatch
extern proc_slice_check
global syscall_entry
syscall_entry:
    mov [rel user_rsp_tmp], rsp
    mov rsp, [rel g_kstack_top]
    push qword [rel user_rsp_tmp]   ; struct sysframe, read bottom-up
    push r11
    push rcx
    push rax
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    sub rsp, 512                    ; the user's FPU / SSE state
    fxsave [rsp]
    ldmxcsr [rel kernel_mxcsr]
    fldcw [rel kernel_fcw]
    sti
    call syscall_dispatch           ; may block, switch threads, or never return (exit)
    cli
    call proc_slice_check           ; ended? slice used up? timer armed? (interrupts off)
    fxrstor [rsp]
    add rsp, 512
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    pop rax                         ; return value (written by the dispatcher)
    pop rcx
    pop r11
    pop rsp
    o64 sysret

; void user_enter(u64 rip, u64 rsp, u64 arg) - first entry of a user thread
global user_enter
user_enter:
    cli
    push 0x1B                       ; SS: user data | RPL 3
    push rsi                        ; RSP
    push 0x202                      ; RFLAGS: IF
    push 0x23                       ; CS: user code | RPL 3
    push rdi                        ; RIP
    mov rdi, rdx                    ; first argument to the program's entry
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    fxrstor [rel fx_clean]          ; clean x87 / SSE state: no registers from earlier threads
    iretq

section .data
kernel_mxcsr: dd 0x1F80
kernel_fcw:   dw 0x037F
align 16
fx_clean:                           ; FXSAVE image: FCW 0x37F, MXCSR 0x1F80, all registers zero
    dw 0x037F
    times 22 db 0
    dd 0x1F80
    dd 0x0000FFFF                   ; MXCSR_MASK
    times 512 - 32 db 0
global isr_table
isr_table:
%assign i 0
%rep 256
    dq isr %+ i
%assign i i+1
%endrep

section .bss
alignb 16
user_rsp_tmp: resq 2
global boot_stack, boot_stack_top
boot_stack:
    resb 65536
boot_stack_top:
