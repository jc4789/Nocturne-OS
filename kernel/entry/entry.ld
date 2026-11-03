/* The entry image's code, linked at 0 into a flat file (Makefile): every reference in it is relative,
   so the same frames serve any address. The clocks' data comes right after, page aligned. */
SECTIONS
{
    . = 0;
    .entry_head : { KEEP(*(.entry_head)) }
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    __clock_data = ALIGN(4096);
    /DISCARD/ : { *(.note .note.*) *(.comment) *(.eh_frame) *(.data .data.*) *(.bss .bss.*) }
}
