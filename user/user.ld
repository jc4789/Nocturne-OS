ENTRY(_start)
SECTIONS
{
    . = 0x400000;
    .text : { *(.text._start) *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    .preinit_array ALIGN(8) : {
        __preinit_array_start = .;
        KEEP(*(.preinit_array))
        __preinit_array_end = .;
    }
    .init_array ALIGN(8) : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    }
    .fini_array ALIGN(8) : {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    }
    .bss : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
