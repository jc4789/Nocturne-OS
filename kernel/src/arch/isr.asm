; Nocturne OS - interrupt entry stubs and context switching
bits 64
section .text

extern isr_dispatch

; Keep the public struct regs layout unchanged. A private word below it records
; whether this entry exchanged GS. Derive frame offsets from the saved GPRs.
%define REGS_GPRS 15
%define REGS_VECTOR (REGS_GPRS * 8)
%define REGS_CS ((REGS_GPRS + 3) * 8)
%define MSR_GS_BASE 0xC0000101

%macro ISR_NOERR 1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
    push qword %1
    jmp isr_common
%endmacro

%assign i 0
%rep 256
%if i == 8 || i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17 || i == 21 || i == 29 || i == 30
    ISR_ERR i
%else
    ISR_NOERR i
%endif
%assign i i+1
%endrep

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    push qword 0
    test byte [rsp + 8 + REGS_CS], 3
    jnz .user_gs
    ; Maskable IRQs cannot enter the interrupt-gate swapgs/iret window (IF=0).
    ; Kernel-origin exceptions, including NMI/#DB/#MC, can. Inspect the actual
    ; base for these entries rather than trusting the saved CS. FSGSBASE is
    ; disabled and user GDT descriptors have zero base; a high base is ours.
    cmp qword [rsp + 8 + REGS_VECTOR], 32
    jae .kernel_gs
    mov ecx, MSR_GS_BASE
    rdmsr
    test edx, edx
    js .kernel_gs
.user_gs:
    swapgs
    mov qword [rsp], 1
.kernel_gs:
    ; Do not let speculative GS-relative kernel loads cross the base decision.
    lfence
    lea rdi, [rsp + 8]
    mov rbx, rsp
    and rsp, -16
    call isr_dispatch
    mov rsp, rbx
global isr_exit
isr_exit:
    cmp qword [rsp], 0
    lea rsp, [rsp + 8]
    je .restore
    swapgs
.restore:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr%+i
%assign i i+1
%endrep

section .text

; void context_switch(uint64_t *old_rsp, uint64_t new_rsp)
global context_switch
extern sched_post_switch
context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    ; 旧stackを離れ、callee-save contextの保存完了後にだけ所有移譲をACKする。
    ; 新stackの保存レジスタはまだpopしていない。rbxはC ABI callee-save。
    mov rbx, rsp
    and rsp, -16
    call sched_post_switch
    mov rsp, rbx
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; First code run by a brand new kernel thread: r12 = function, r13 = argument
extern kthread_start
global kthread_trampoline
kthread_trampoline:
    mov rdi, r12
    mov rsi, r13
    call kthread_start
    ud2

; First code run by a brand new user task: the stack holds a struct regs
global user_trampoline
extern sched_user_return
user_trampoline:
    push qword 1
    lea rdi, [rsp + 8]
    mov rbx, rsp
    and rsp, -16
    call sched_user_return
    mov rsp, rbx
    jmp isr_exit

; bool wrmsr_checked(uint32_t msr, uint64_t value): false where the WRMSR raised #GP, which
; isr_dispatch resumes at wrmsr_checked_refused (arch/cpu.h).
global wrmsr_checked, wrmsr_checked_write, wrmsr_checked_refused
wrmsr_checked:
    mov ecx, edi
    mov eax, esi
    mov rdx, rsi
    shr rdx, 32
wrmsr_checked_write:
    wrmsr
    mov eax, 1
    ret
wrmsr_checked_refused:
    xor eax, eax
    ret

; size_t user_copy(void *dst, const void *src, size_t n): the bytes left, 0 when all went. A page
; fault on the user side that the app's memory does not resolve resumes at user_copy_fault
; (arch/idt.c), with rcx the bytes rep movsb had left.
global user_copy, user_copy_start, user_copy_end, user_copy_fault
user_copy:
    mov rcx, rdx
user_copy_start:
    rep movsb
user_copy_end:
    xor eax, eax
    ret
user_copy_fault:
    mov rax, rcx
    ret
