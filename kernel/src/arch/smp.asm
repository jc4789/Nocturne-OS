bits 64
section .text

; void cpu_switch_stack(uint64_t top, void (*entry)(void *), void *argument) (arch/cpu.h)
global cpu_switch_stack
cpu_switch_stack:
    cli
    mov rsp, rdi
    and rsp, -16
    xor ebp, ebp
    mov rdi, rdx
    call rsi
    ud2
