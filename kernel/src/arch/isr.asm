; Nocturne OS - the ways into the kernel (interrupts, exceptions, system calls) and back out, and
; context switching
bits 64
section .text

extern isr_exception, isr_interrupt, syscall_dispatch, sched_user_return, sched_preempt_irq
extern percpu_offsets
; per-CPU variables (arch/percpu.h), read GS-relative
extern cpu_tss, irq_stack_top, preempt_count, sched_resched, syscall_user_rsp

; struct regs (arch/cpu.h), from the saved registers up. Below it one private word: 1 when this
; entry exchanged GS (isr_exit exchanges it back), or, on an entry from the kernel through an IST
; stack, the GS base to put back.
%define REGS_GPRS 15
%define REGS_VECTOR (REGS_GPRS * 8)
%define REGS_RIP ((REGS_GPRS + 2) * 8)
%define REGS_CS ((REGS_GPRS + 3) * 8)
%define REGS_RFLAGS ((REGS_GPRS + 4) * 8)
%define REGS_RSP ((REGS_GPRS + 5) * 8)
%define REGS_SS ((REGS_GPRS + 6) * 8)
%define FRAME_BYTES ((REGS_GPRS + 8) * 8) ; the private word and struct regs
%define REGS_SYSCALL 256                  ; arch/cpu.h
%define TSS_RSP0 4                        ; arch/gdt.c
%define USER_CS 0x23
%define USER_DS 0x1B
%define PREEMPT_IRQ_ONE (1 << 16)         ; arch/preempt.h
%define PREEMPT_IRQ_MASK (0xFF << 16)
%define RFLAGS_TF (1 << 8)
%define RFLAGS_IF (1 << 9)
%define RFLAGS_RF (1 << 16)

%macro SAVE_GPRS 0
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
%endmacro

%macro RESTORE_GPRS 0
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
%endmacro

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

; NMI, #DB, #MC: on their IST stacks (arch/idt.c), from anywhere
%macro ISR_IST 1
isr%1:
    push qword 0
    push qword %1
    jmp isr_ist
%endmacro

%assign i 0
%rep 256
%if i == 1 || i == 2 || i == 18
    ISR_IST i
%elif i == 8
; #DF: its saved CS and RIP are undefined, so always as from the kernel; it reports and stops
isr8:
    push qword 8
    jmp isr_ist_kernel
%elif i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17 || i == 21 || i == 29 || i == 30
    ISR_ERR i
%else
    ISR_NOERR i
%endif
%assign i i+1
%endrep

; The ordinary entry, on the task's stack (from the app: the TSS's rsp0) or the kernel's stack in
; use (from the kernel).
isr_common:
    SAVE_GPRS
    cld
    clac
    push qword 0
    test byte [rsp + 8 + REGS_CS], 3
    jnz .swap
    ; From the kernel GS is the kernel's, but for one place: the iretq back to the app faulted,
    ; with the app's GS in already.
    lea rax, [rel isr_iret]
    cmp [rsp + 8 + REGS_RIP], rax
    jne .kernel_gs
.swap:
    swapgs
    mov qword [rsp], 1
.kernel_gs:
    ; Do not let speculative GS-relative kernel loads cross the GS decision.
    lfence
; The frame saved and the kernel's GS in: an exception on this stack, an interrupt on this CPU's
; interrupt stack; then, back to the app, the switch the scheduler may want, and back into the
; kernel after an interrupt, the preemption it may want (arch/preempt.h).
isr_saved:
    lea rdi, [rsp + 8]
    mov rbx, rsp
    cmp qword [rdi + REGS_VECTOR], 32
    jb .exception
    add dword [gs:rel preempt_count], PREEMPT_IRQ_ONE
    mov eax, [gs:rel preempt_count]
    and eax, PREEMPT_IRQ_MASK
    cmp eax, PREEMPT_IRQ_ONE
    jne .nested
    mov rsp, [gs:rel irq_stack_top]
    jmp .interrupt
.nested:
    and rsp, -16
.interrupt:
    call isr_interrupt
    mov rsp, rbx
    sub dword [gs:rel preempt_count], PREEMPT_IRQ_ONE
    test byte [rsp + 8 + REGS_CS], 3
    jnz .back
    ; Back into the kernel, on the stack it came in on: preempted when a switch is due, the count is
    ; 0 (no handler, spinlock or preempt_disable below) and the code it interrupted had interrupts on.
    cmp dword [gs:rel preempt_count], 0
    jne isr_exit
    test qword [rsp + 8 + REGS_RFLAGS], RFLAGS_IF
    jz isr_exit
    cmp byte [gs:rel sched_resched], 0
    je isr_exit
    and rsp, -16
    call sched_preempt_irq
    mov rsp, rbx
    jmp isr_exit
.exception:
    and rsp, -16
    call isr_exception
    mov rsp, rbx
.back:
    test byte [rsp + 8 + REGS_CS], 3
    jz isr_exit
    lea rdi, [rsp + 8]
    and rsp, -16
    call sched_user_return
    mov rsp, rbx
global isr_exit
isr_exit:
    cmp qword [rsp], 0
    lea rsp, [rsp + 8]
    je .restore
    swapgs
.restore:
    RESTORE_GPRS
    add rsp, 16
global isr_iret
isr_iret:
    iretq

; NMI, #DB and #MC, on their own stacks. From the app: GS is the app's and this stack is only
; this exception's, so the frame goes to the task's stack and on as an ordinary entry (whatever
; it does there may sleep).
isr_ist:
    test byte [rsp + 24], 3 ; the saved CS, above the vector, the error code and RIP
    jz isr_ist_kernel
    swapgs
    SAVE_GPRS
    cld
    clac
    push qword 1
    mov rsi, rsp
    mov rdi, [gs:rel cpu_tss + TSS_RSP0]
    sub rdi, FRAME_BYTES
    mov rsp, rdi
    mov ecx, FRAME_BYTES / 8
    rep movsq
    jmp isr_saved
; From the kernel the GS base in use may be either: the app's in the few instructions around a
; swapgs (SYSCALL's entry, the way out to the app) where these can still come. Keep it, put this
; CPU's own in (its index from RDPID, arch/percpu.h), and put it back on the way out. Nothing
; here sleeps or switches tasks.
isr_ist_kernel:
    SAVE_GPRS
    cld
    clac
    rdgsbase rax
    push rax
    rdpid rax
    mov rdx, [rel percpu_offsets]
    mov rax, [rdx + rax * 8]
    wrgsbase rax
    lea rdi, [rsp + 8]
    mov rbx, rsp
    and rsp, -16
    call isr_exception
    mov rsp, rbx
    pop rax
    wrgsbase rax
    RESTORE_GPRS
    add rsp, 16
    iretq

; SYSCALL (arch/idt.c sets it up): rcx holds the app's rip, r11 its rflags, interrupts are off and
; the stack is still the app's. The frame is the same as an interrupt's, on the task's stack.
global syscall_entry
syscall_entry:
    swapgs
    mov [gs:rel syscall_user_rsp], rsp
    mov rsp, [gs:rel cpu_tss + TSS_RSP0]
    push qword USER_DS
    push qword [gs:rel syscall_user_rsp]
    push r11
    push qword USER_CS
    push rcx
    push qword 0
    push qword REGS_SYSCALL
    SAVE_GPRS
    push qword 1
    lea rdi, [rsp + 8]
    mov rbx, rsp
    and rsp, -16
    call syscall_dispatch
    lea rdi, [rbx + 8]
    call sched_user_return
    mov rsp, rbx
    ; SYSRET returns to rcx with rflags r11: when the frame says so and SYSRET can (rip an app's
    ; address, which SYSRET would otherwise fault on in the kernel; no single step, which only
    ; IRET carries out as one), else IRET.
    mov rcx, [rsp + 8 + REGS_RIP]
    mov r11, [rsp + 8 + REGS_RFLAGS]
    mov rax, rcx
    shr rax, 47
    jnz isr_exit
    test r11, RFLAGS_TF | RFLAGS_RF
    jnz isr_exit
    cmp qword [rsp + 8 + REGS_CS], USER_CS
    jne isr_exit
    cmp qword [rsp + 8 + REGS_SS], USER_DS
    jne isr_exit
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    add rsp, 8 ; r11: the rflags above
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    add rsp, 8 ; rcx: the rip above
    pop rbx
    pop rax
    mov rcx, [rsp + 16]
    mov r11, [rsp + 32]
    mov rsp, [rsp + 40]
    swapgs
    o64 sysret

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
user_trampoline:
    push qword 1
    lea rdi, [rsp + 8]
    mov rbx, rsp
    and rsp, -16
    call sched_user_return
    mov rsp, rbx
    jmp isr_exit

; bool wrmsr_checked(uint32_t msr, uint64_t value): false where the WRMSR raised #GP, which
; isr_exception resumes at wrmsr_checked_refused (arch/cpu.h).
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

; size_t user_copy(void *dst, const void *src, size_t n): the bytes left, 0 when all went. The one
; place the kernel touches the app's memory (SMAP opened around it). A page fault on the user side
; that the app's memory does not resolve resumes at user_copy_fault (arch/idt.c), with rcx the
; bytes rep movsb had left.
global user_copy, user_copy_start, user_copy_end, user_copy_fault
user_copy:
    mov rcx, rdx
    stac
user_copy_start:
    rep movsb
user_copy_end:
    clac
    xor eax, eax
    ret
user_copy_fault:
    clac
    mov rax, rcx
    ret
