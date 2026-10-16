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
    /* ELF thread-local storage: libc copies this image below each thread's
       FS-based control block (x86-64 variant II). */
    .tdata : { *(.tdata .tdata.*) }
    .tbss : { *(.tbss .tbss.*) *(.tcommon) }
    __tls_image = ADDR(.tdata);
    __tls_image_end = ADDR(.tdata) + SIZEOF(.tdata);
    __tls_memsz = ADDR(.tbss) + SIZEOF(.tbss) - ADDR(.tdata);
    __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss));
    .bss : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
