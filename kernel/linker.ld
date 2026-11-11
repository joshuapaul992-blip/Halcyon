OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)
SECTIONS
{
    . = 0x100000;
    _kernel_start = .;
    .text : { *(.header) *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    . = ALIGN(16);
    _file_end = .;
    _kernel_file_size = _file_end - 0x100000;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(4096);
    _bss_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
