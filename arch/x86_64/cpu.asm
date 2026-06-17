; arch/x86_64/cpu.asm
; ============================================================================
; NEXUS — kernel-owned GDT and descriptor-table loaders (Phase 2a).
;
; Until now the kernel ran on the bootloader's GDT. gdt_init installs the
; kernel's own flat 64-bit GDT and reloads CS (via a far return) and the data
; segments. idt_load just executes LIDT on a caller-supplied IDTR.
;
; No TSS yet: we handle exceptions in ring 0 with no privilege change and no
; IST, so the CPU uses the current RSP and no TSS is required. A TSS arrives
; with ring-3 support / IST stacks in a later slice.
; ============================================================================

BITS 64

section .text
global gdt_init
global idt_load

; void gdt_init(void)
gdt_init:
    lgdt [gdt64_ptr]
    mov ax, 0x10                ; kernel data selector
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    ; reload CS = 0x08 with a far return (no direct far-jmp-imm in long mode)
    lea rax, [rel .reload_cs]
    push qword 0x08
    push rax
    o64 retf
.reload_cs:
    ret

; void idt_load(struct idtr *ptr)   ; SysV: ptr in RDI
idt_load:
    lidt [rdi]
    ret

section .rodata
align 16
gdt64:
    dq 0x0000000000000000       ; 0x00 null
    dq 0x00AF9A000000FFFF       ; 0x08 code64: present, DPL0, exec/read, L=1, G=1
    dq 0x00CF92000000FFFF       ; 0x10 data64: present, DPL0, read/write, G=1
gdt64_ptr:
    dw (gdt64_ptr - gdt64) - 1  ; limit
    dq gdt64                     ; base
