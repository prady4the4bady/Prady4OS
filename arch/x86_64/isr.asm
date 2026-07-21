; arch/x86_64/isr.asm
; ============================================================================
; NEXUS — CPU exception ISR stubs and common dispatch trampoline (Phase 2a).
;
; One stub per vector 0..47 (0..31 CPU exceptions, 32..47 the PIC-remapped
; hardware IRQs 0x20..0x2F). Vectors that the CPU pushes an error code for use
; ISR_ERR (no dummy); the rest use ISR_NOERR (push a 0 so every frame has the
; same shape). Each stub pushes its vector number and jumps to isr_common,
; which saves all GP registers, hands a pointer to the frame to the C handler
; isr_dispatch(struct regs *), then restores and IRETQs (so recoverable faults
; like #BP, and all hardware IRQs, resume).
;
; The pushed frame must match `struct regs` in kernel/idt.c exactly. From the
; handler's RSP upward, low address first:
;   r15 r14 r13 r12 r11 r10 r9 r8 rbp rdi rsi rdx rcx rbx rax  (GP, this file)
;   vector  err_code                                           (stub)
;   rip cs rflags rsp ss                                       (CPU)
;
; Note: we do not force 16-byte stack alignment before the C call. The kernel
; is built -mgeneral-regs-only (no SSE), so misalignment is harmless here.
; ============================================================================

BITS 64

section .text
extern isr_dispatch

%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push qword 0                ; dummy error code
    push qword %1               ; vector
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    ; CPU already pushed a real error code
    push qword %1               ; vector
    jmp isr_common
%endmacro

; Vectors 8,10,11,12,13,14,17,21,29,30 push a CPU error code; the rest do not.
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; Hardware IRQ vectors 32..47 (PIC remapped to 0x20..0x2F). No error codes.
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

; APIC timer vector (DDR-714): outside the PIC's 32..47 range so the two timer
; sources can never alias. No error code.
ISR_NOERR 48
; AP wake IPI (DDR-SMP-3c-alpha): breaks an AP out of hlt; handler just EOIs.
ISR_NOERR 49
; MSI-X device vectors (DDR-714C1/C2, DDR-771): per-device message-signaled
; interrupts, delivered straight to the LAPIC (no PIC/IOAPIC).
; 54 = virtio-net, 55 = virtio-input, 56..63 = virtio-blk 0..7 (DDR-771 relocated
; the block window to 56..63 so >4 disks fit clear of net/input). 50..53 unused.
ISR_NOERR 50
ISR_NOERR 51
ISR_NOERR 52
ISR_NOERR 53
ISR_NOERR 54
ISR_NOERR 55
ISR_NOERR 56
ISR_NOERR 57
ISR_NOERR 58
ISR_NOERR 59
ISR_NOERR 60
ISR_NOERR 61
ISR_NOERR 62
ISR_NOERR 63

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

    ; DDR-SMP-3a: if we interrupted ring 3, the user's GS base is active — swap
    ; to the kernel percpu base before any C runs. CS sits at frame offset 144
    ; (15 GPRs + vector + err). Kernel-interrupts-kernel keeps kernel GS as-is.
    test qword [rsp + 144], 3
    jz .gs_kernel_in
    swapgs
.gs_kernel_in:

    mov rdi, rsp                ; struct regs * -> first argument
    call isr_dispatch

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

    ; DDR-SMP-3a: mirror of the entry swap — returning to ring 3 parks the
    ; kernel percpu base back in KERNEL_GS_BASE. CS is at +24 here (vector +
    ; err below it). The test's flags are dead: IRETQ reloads RFLAGS.
    test qword [rsp + 24], 3
    jz .gs_kernel_out
    swapgs
.gs_kernel_out:
    add rsp, 16                 ; discard vector + error code
    iretq

; Address table consumed by idt_init() in kernel/idt.c (vectors 0..63, DDR-771).
section .rodata
global isr_stub_table
align 8
isr_stub_table:
%assign i 0
%rep 64
    dq isr_stub_ %+ i
%assign i i+1
%endrep
