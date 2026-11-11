; Halcyon OS - Master Boot Record
; Loads stage2 (64 sectors, LBA 1..64) to 0000:8000 using INT 13h extensions
; and jumps to it with DL = BIOS boot drive.
; Only bytes 0..439 are written to disk; the partition table is preserved.

bits 16
org 0x7C00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    jmp 0x0000:.flush
.flush:
    sti
    cld
    mov [boot_drive], dl

    mov si, msg_boot
    call print

    ; INT 13h extensions present?
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc no_ext
    cmp bx, 0xAA55
    jne no_ext

    ; read 2 x 32 sectors
    mov word [dap.count], 32
    mov word [dap.off], 0x8000
    mov dword [dap.lba], 1
    call read
    mov word [dap.count], 32
    mov word [dap.off], 0xC000
    mov dword [dap.lba], 33
    call read

    mov dl, [boot_drive]
    jmp 0x0000:0x8000

read:
    mov cx, 3
.retry:
    push cx
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    pop cx
    jnc .ok
    xor ah, ah              ; reset disk and retry
    mov dl, [boot_drive]
    int 0x13
    loop .retry
    jmp disk_err
.ok:
    ret

no_ext:
    mov si, msg_noext
    jmp fail
disk_err:
    mov si, msg_disk
fail:
    call print
.halt:
    hlt
    jmp .halt

print:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print
.done:
    ret

align 4
dap:
    db 16, 0
.count: dw 32
.off:   dw 0x8000
.seg:   dw 0
.lba:   dd 1
        dd 0

boot_drive: db 0
msg_boot:  db "Halcyon", 0
msg_noext: db " - no LBA BIOS support", 0
msg_disk:  db " - disk read error", 0

times 440-($-$$) db 0
