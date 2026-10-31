; Real-mode entry for a CPU that starts from nothing: each AP the kernel starts with INIT-SIPI, at
; boot and after a sleep, and the BSP when the firmware wakes the machine from S3 (the FACS waking
; vector). arch/wake.c copies tramp_start..tramp_end to a page below 1 MiB and fills the data
; block at TRAMP_DATA in that page. Real mode -> 32-bit protected mode -> long mode through a page
; table in low memory that maps the page itself and the kernel's upper half; x2APIC mode; then, for
; the CPU the data block is for (arch/wake.h), a jump to `entry` with rsp = `stack` and
; rdi = `arg`: the block's own for the BSP the firmware wakes, else those of the CPU's row in the
; table, found by its x2APIC ID.
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
%define TD_COUNT   (TRAMP_DATA + 96)
%define TD_FLAGS   (TRAMP_DATA + 100)
%define TD_TABLE   (TRAMP_DATA + 104)
%define TDF_ANY    1            ; the one CPU coming in takes the block's stack and arg
%define ROW_STACK  0            ; struct wake_cpu
%define ROW_ARG    8
%define ROW_ID     16
%define ROW_SIZE   24

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
tramp_lm64:                     ; rbx: the page (a hibernation's last copy enters here too)
    mov ebx, ebx
    mov ecx, 0x1B               ; IA32_APIC_BASE: x2APIC mode, which INIT keeps (APM 16.10), from
    rdmsr                       ; wherever the firmware left it: enabled first, then with EXTD too
    bt eax, 10                  ; (APM 16.9.1); it keeps its x2APIC ID
    jc .x2
    bts eax, 11
    wrmsr
    bts eax, 10
    wrmsr
.x2:
    test dword [rbx + TD_FLAGS], TDF_ANY
    jnz .any
    mov ecx, 0x802              ; the x2APIC ID: a CPU with no row (a STARTUP that was not its own)
    rdmsr                       ; goes no further
    mov rsi, [rbx + TD_TABLE]
    mov ecx, [rbx + TD_COUNT]
.row:
    test ecx, ecx
    jz .other
    cmp eax, [rsi + ROW_ID]
    je .own
    add rsi, ROW_SIZE
    dec ecx
    jmp .row
.own:
    mov rsp, [rsi + ROW_STACK]
    mov rdi, [rsi + ROW_ARG]
    jmp .enter
.any:
    mov rsp, [rbx + TD_STACK]
    mov rdi, [rbx + TD_ARG]
.enter:
    mov rax, [rbx + TD_ENTRY]
    jmp rax
.other:                         ; nothing written: halted until an INIT
    cli
    hlt
    jmp .other
tramp_end:

; ---- the BSP across S3 ----
section .bss
global wake_saved_rsp
wake_saved_rsp: resq 1

section .text
extern wake_resume_cpu

; uint64_t wake_suspend(void (*enter)(void)): saves the callee-saved registers and the stack, then
; calls enter(), which puts the machine to sleep. Returns 0 when enter() comes back (sleep failed)
; and 1 when the firmware woke the machine and wake_resume_cpu set this CPU up again.
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
wake_resume64:                  ; from tramp_lm64: the wake page's tables and GDT, no IDT
    mov rsp, [rel wake_saved_rsp]
    call wake_resume_cpu        ; the CPU's state (cpu_setup), then the kernel's tables
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
