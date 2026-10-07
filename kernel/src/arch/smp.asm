bits 64
section .text

; Limine の stack に依存せず、kernel image 内の CPU 専用 stack に移る。
; void smp_switch_stack(uint64_t top, void (*entry)(void *), void *argument)
global smp_switch_stack
smp_switch_stack:
    cli
    mov rsp, rdi
    and rsp, -16
    xor ebp, ebp
    mov rdi, rdx
    call rsi
    ud2
