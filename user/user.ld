ENTRY(_start)
SECTIONS
{
    . = 0x400000;
    .text : { *(.text._start) *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    /* The unwind tables of C++ exceptions, by the names libunwind finds them
       by (a program is one image, with no loader to ask). .eh_frame_hdr is
       their sorted index, which ld.lld makes with --eh-frame-hdr. */
    .eh_frame_hdr : { *(.eh_frame_hdr) }
    __eh_frame_hdr_start = SIZEOF(.eh_frame_hdr) > 0 ? ADDR(.eh_frame_hdr) : 0;
    __eh_frame_hdr_end = SIZEOF(.eh_frame_hdr) > 0 ? ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr) : 0;
    .eh_frame : {
        __eh_frame_start = .;
        *(.eh_frame)
        __eh_frame_end = .;
    }
    /* What each function's catch and cleanup code is, which .eh_frame points to. */
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) }
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
    /DISCARD/ : { *(.note .note.*) *(.comment) }
}
