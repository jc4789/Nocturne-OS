; Nocturne OS - interrupt entry stubs and context switching
bits 64
section .text

extern isr_dispatch

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
    mov rdi, rsp
    call isr_dispatch
global isr_exit
isr_exit:
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
    mov rdi, rsp
    call sched_user_return
    jmp isr_exit
