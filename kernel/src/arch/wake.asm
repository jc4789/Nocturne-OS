; Real-mode entry for a CPU that starts from nothing: the BSP when the firmware wakes the machine
; from S3 (the FACS waking vector), and each AP that the kernel starts itself with INIT-SIPI.
; arch/wake.c copies tramp_start..tramp_end to a page below 1 MiB and fills the data block at
; TRAMP_DATA in that page. Real mode -> 32-bit protected mode -> long mode through a page table
; in low memory that maps the page itself and the kernel's upper half, then a jump to `entry`
; with rsp = `stack` and rdi = `arg`.
bits 16
section .text

%define TRAMP_DATA 0x200        ; keep in step with arch/wake.c
%define TD_GDTR    (TRAMP_DATA + 32)
%define TD_FAR32   (TRAMP_DATA + 40)
%define TD_FAR64   (TRAMP_DATA + 48)
%define TD_CR3     (TRAMP_DATA + 56)
%define TD_EFER    (TRAMP_DATA + 64)
%define TD_ENTRY   (TRAMP_DATA + 72)
%define TD_STACK   (TRAMP_DATA + 80)
%define TD_ARG     (TRAMP_DATA + 88)

global tramp_start, tramp_pm32, tramp_lm64, tramp_end
tramp_start:
    cli
    cld
    mov ax, cs                  ; the waking vector and SIPI both enter at offset 0 of the page
    mov ds, ax
    xor ebx, ebx
    mov bx, ax
    shl ebx, 4                  ; ebx = physical address of the page
    o32 lgdt [TD_GDTR]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword far [TD_FAR32]

bits 32
tramp_pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr4
    or eax, 1 << 5              ; PAE
    mov cr4, eax
    mov eax, [ebx + TD_CR3]
    mov cr3, eax
    mov ecx, 0xC0000080         ; EFER: LME, and NXE when the kernel's tables use NX
    mov eax, [ebx + TD_EFER]
    xor edx, edx
    wrmsr
    mov eax, cr0
    or eax, 0x80000001          ; PG | PE
    mov cr0, eax
    jmp far [ebx + TD_FAR64]

bits 64
tramp_lm64:
    mov ebx, ebx
    mov rsp, [rbx + TD_STACK]
    mov rdi, [rbx + TD_ARG]
    mov rax, [rbx + TD_ENTRY]
    jmp rax
tramp_end:

; ---- the BSP across S3 ----
section .bss
global wake_gdtr, wake_saved_rsp
wake_gdtr:      resb 16
wake_saved_rsp: resq 1

section .text
extern wake_restore_cpu

; uint64_t wake_suspend(void (*enter)(void)): saves the callee-saved registers and the stack, then
; calls enter(), which puts the machine to sleep. Returns 0 when enter() comes back (sleep failed)
; and 1 when the firmware woke the machine and wake_resume64 restored this CPU.
global wake_suspend, wake_resume64
wake_suspend:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    pushfq
    mov [rel wake_saved_rsp], rsp
    call rdi
    xor eax, eax
    jmp wake_out
wake_resume64:                  ; from tramp_lm64: low page table, the trampoline's GDT
    lgdt [rel wake_gdtr]
    push 0x08
    lea rax, [rel wake_kernel_cs]
    push rax
    retfq
wake_kernel_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov rsp, [rel wake_saved_rsp]
    call wake_restore_cpu       ; page tables, control registers, MSRs, TSS, IDT
    mov eax, 1
wake_out:
    popfq
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret
