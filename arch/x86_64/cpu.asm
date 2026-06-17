; arch/x86_64/cpu.asm
; ============================================================================
; NEXUS — kernel GDT (with user segments + TSS) and descriptor-table loaders.
;
; gdt_init installs the kernel's GDT and reloads CS + data segments. The GDT now
; also carries ring-3 user segments and a 16-byte TSS descriptor (filled in at
; runtime by tss_init in C, then loaded with LTR), required for SYSCALL/SYSRET
; and for taking interrupts while in ring 3.
;
; Layout (selectors), arranged for SYSCALL/SYSRET:
;   0x08 kernel code64   (SYSCALL CS)        0x10 kernel data (SYSCALL SS = CS+8)
;   0x18 user   data     (SYSRET SS)         0x20 user code64 (SYSRET CS)
;   0x28 TSS (occupies 0x28 and 0x30)
; The GDT lives in .data so tss_init can patch the TSS descriptor.
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
    lea rax, [rel .reload_cs]   ; reload CS = 0x08 via far return
    push qword 0x08
    push rax
    o64 retf
.reload_cs:
    ret

; void idt_load(struct idtr *ptr)   ; SysV: ptr in RDI
idt_load:
    lidt [rdi]
    ret

section .data
align 16
global gdt64
global gdt64_tss
gdt64:
    dq 0x0000000000000000       ; 0x00 null
    dq 0x00AF9A000000FFFF       ; 0x08 kernel code64 (DPL0, L=1)
    dq 0x00CF92000000FFFF       ; 0x10 kernel data   (DPL0, RW)
    dq 0x00CFF2000000FFFF       ; 0x18 user data     (DPL3, RW)
    dq 0x00AFFA000000FFFF       ; 0x20 user code64   (DPL3, L=1)
gdt64_tss:
    dq 0x0000000000000000       ; 0x28 TSS low  (patched by tss_init)
    dq 0x0000000000000000       ; 0x30 TSS high (patched by tss_init)
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1    ; limit
    dq gdt64                     ; base
