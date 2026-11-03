; The entry image's code, built apart (kernel/entry, Makefile). sys/entry.c copies it into frames.
bits 64
section .rodata
global entry_code, entry_code_end
align 16
entry_code:
    incbin "entry_code.bin"
entry_code_end:
