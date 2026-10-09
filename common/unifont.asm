; Unifont Japanese 18.0.01, OFL-1.1; see third_party/unifont/NOTICE.txt.
; Generated binary: 65536 width bytes, then 65536 * 16 * 2 bitmap bytes.
section .rodata align=16
global unifont_widths
global unifont_rows
unifont_widths:
    incbin "common/unifont.bin", 0, 65536
unifont_rows:
    incbin "common/unifont.bin", 65536
section .note.GNU-stack noalloc noexec nowrite progbits
