; Real-mode entry for a CPU that starts from nothing: the BSP when the firmware wakes the machine
; from S3 (the FACS waking vector), and each AP that the kernel starts itself with INIT-SIPI.
; arch/wake.c copies tramp_start..tramp_end to a page below 1 MiB and fills the data block at
; TRAMP_DATA in that page. Real mode -> 32-bit protected mode -> long mode through a page table
; in low memory that maps the page itself and the kernel's upper half, then, for the CPU the data
; block is for (arch/wake.h), a jump to `entry` with rsp = `stack` and rdi = `arg`: the block's
; own for the BSP the firmware wakes, else those of the CPU's row in the table.
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
%define TD_ID_REG  (TRAMP_DATA + 96)
%define TD_COUNT   (TRAMP_DATA + 104)
%define TD_ID_MASK (TRAMP_DATA + 108)
%define TD_BASE    (TRAMP_DATA + 112)
%define TD_BASE_MASK (TRAMP_DATA + 120)
%define TD_FLAGS   (TRAMP_DATA + 128)
%define TD_TABLE   (TRAMP_DATA + 136)
%define TDF_X2     1            ; the kernel is in x2APIC mode
%define TDF_XAIDN  2            ; AMD's 8-bit xAPIC ID is to be turned on (arch/apic.c)
%define TDF_ANY    4            ; the one CPU coming in takes the block's stack and arg
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
tramp_lm64:
    mov ebx, ebx
    test dword [rbx + TD_FLAGS], TDF_ANY
    jnz .any
    mov ecx, 0x1B               ; IA32_APIC_BASE: a STARTUP may have reached other CPUs (arch/wake.h),
                                ; and this one goes on only with a row for its APIC ID
    rdmsr
    test dword [rbx + TD_FLAGS], TDF_X2
    jz .xapic
    bt eax, 10                  ; the kernel's x2APIC mode, which this APIC takes from xAPIC mode,
    jc .x2                      ; keeping its x2APIC ID (SDM 11.12.5.1); a disabled one takes no
    bts eax, 10                 ; STARTUP (SDM 11.4.3, 42300 2.4.8.1.15)
    wrmsr
.x2:
    mov ecx, 0x802              ; the x2APIC ID
    rdmsr
    jmp .find
.xapic:
    bt eax, 10                  ; the kernel's xAPIC mode, which an APIC in x2APIC mode takes later,
    jc .x2                      ; where it may (arch/apic.c), lapic_ap_init
    shl rdx, 32
    or rax, rdx
    and rax, [rbx + TD_BASE_MASK]
    cmp rax, [rbx + TD_BASE]    ; enabled, at the kernel's xAPIC page
    jne .other
    mov rsi, [rbx + TD_ID_REG]
    test dword [rbx + TD_FLAGS], TDF_XAIDN
    jz .xid
    or dword [rsi + 0x410 - 0x20], 4 ; the 8-bit ID, which INIT turned off (APM 16.3.2)
.xid:
    mov eax, [rsi]
    shr eax, 24
    and eax, [rbx + TD_ID_MASK]
.find:                          ; eax: its APIC ID
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

; ---- the last step of restoring a hibernation image ----
; Copied to a page below 1 MiB that neither the restoring kernel nor the image uses, and entered on
; a page table that maps all RAM one to one (arch/wake.c, wake_hibernate_jump), interrupts off:
;   rdi = the first page of the copy list: { next page, count, count x { dst, src } }, physical;
;         src 0 means a page of zeros
;   rsi = the image's wake page, which the image prepared for wake_resume64 before its snapshot
; Every pair is copied, then the image's own wake page table (the first 2 MiB one to one and the
; image's kernel half) is loaded and the image comes back through tramp_lm64, as from S3.
bits 64
section .text
global hib_copy_start, hib_copy_end
hib_copy_start:
    mov r12, rdi
    mov r15, rsi
    cld
.page:
    test r12, r12
    jz .done
    mov r13, [r12 + 8]
    lea r14, [r12 + 16]
.pair:
    test r13, r13
    jz .next
    mov rdi, [r14]
    mov rsi, [r14 + 8]
    mov ecx, 512
    test rsi, rsi
    jz .zero
    rep movsq
    jmp .advance
.zero:
    xor eax, eax
    rep stosq
.advance:
    add r14, 16
    dec r13
    jmp .pair
.next:
    mov r12, [r12]
    jmp .page
.done:
    mov rax, cr4                ; global TLB entries of the restoring kernel's half must go
    and rax, ~(1 << 7)
    mov cr4, rax
    lea rax, [r15 + 0x1000]
    mov cr3, rax
    mov ebx, r15d
    lea rax, [rbx + (tramp_lm64 - tramp_start)]
    jmp rax
hib_copy_end:
