; Nocturne user entry point: kernel passes argc in rdi, argv in rsi.
bits 64
section .text._start progbits alloc exec nowrite align=16
global _start
extern __libc_start
_start:
    xor rbp, rbp
    and rsp, -16
    call __libc_start
.hang:
    jmp .hang
