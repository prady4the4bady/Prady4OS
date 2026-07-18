; arch/x86_64/ap_boot.asm — AP startup trampoline (DDR-714 stage B, ADR-029).
;
; Linked into the kernel as data and copied by the BSP to physical 0x8000 (the
; SIPI vector 0x08). Each AP starts here in 16-bit real mode (CS=0x0800, IP=0)
; and goes real -> PAE+LME -> long mode directly (Intel SDM vol. 3 §10.8.5),
; reusing the BSP's page tables. All addresses are absolute 0x8000-based
; constants, so the blob needs no relocation. The BSP fills the mailbox
; (CR3 / C entry / stack / cpu index) before sending the SIPI, and starts APs
; one at a time (the mailbox is shared).
;
; Fixed layout within the copied page:
;   +0x00  16-bit entry (SIPI target)
;   +0x40  64-bit entry
;   +0x80  GDT: null, 0x08=64-bit code, 0x10=data
;   +0x98  GDT descriptor (limit + 24-bit-reachable base 0x8080)
;   +0xA0  mailbox: cr3 u64, entry u64, stack u64, idx u32, efer_or u32

TRAMP_BASE   equ 0x8000
OFF_ENTRY64  equ 0x40
OFF_GDT      equ 0x80
OFF_GDTDESC  equ 0x98
OFF_MB_CR3   equ 0xA0
OFF_MB_ENTRY equ 0xA8
OFF_MB_STACK equ 0xB0
OFF_MB_IDX   equ 0xB8
OFF_MB_EFER  equ 0xBC                       ; DDR-757: LME | (NXE if the BSP has NX)

section .rodata
global ap_tramp_start
global ap_tramp_end
align 16
ap_tramp_start:

[bits 16]
    cli
    xor ax, ax
    mov ds, ax                              ; absolute 0x8xxx addressing below
    lgdt [TRAMP_BASE + OFF_GDTDESC]
    mov eax, cr4
    or  eax, 0x20                           ; CR4.PAE
    mov cr4, eax
    mov eax, [TRAMP_BASE + OFF_MB_CR3]
    mov cr3, eax                            ; the BSP's (kernel master) tables
    mov ecx, 0xC0000080                     ; IA32_EFER
    rdmsr
    ; DDR-757: arm LME *and* (when supported) NXE BEFORE paging turns on — the
    ; higher-half kernel data pages now carry NX, and touching them with NXE
    ; clear is a reserved-bit #PF. The BSP writes the exact OR-mask (0x100, or
    ; 0x900 with NX) into the mailbox, so a non-NX CPU never sets NXE.
    or  eax, [TRAMP_BASE + OFF_MB_EFER]
    wrmsr
    mov eax, cr0
    or  eax, 0x80000001                     ; CR0.PG | CR0.PE -> long mode
    mov cr0, eax
    jmp dword 0x08:(TRAMP_BASE + OFF_ENTRY64)

    times OFF_ENTRY64 - ($ - ap_tramp_start) db 0
[bits 64]
    mov ax, 0x10                            ; sane data segments
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [TRAMP_BASE + OFF_MB_STACK]
    mov edi, [TRAMP_BASE + OFF_MB_IDX]      ; SysV arg0 = cpu index
    mov rax, [TRAMP_BASE + OFF_MB_ENTRY]
    jmp rax                                 ; -> smp_ap_entry (never returns)

    times OFF_GDT - ($ - ap_tramp_start) db 0
    dq 0                                    ; null
    dq 0x00209A0000000000                   ; 0x08: 64-bit code (L=1, P, exec)
    dq 0x0000920000000000                   ; 0x10: data (P, writable)

    times OFF_GDTDESC - ($ - ap_tramp_start) db 0
    dw 23                                   ; GDT limit (3 entries - 1)
    dd TRAMP_BASE + OFF_GDT                 ; GDT base (fits real-mode lgdt)

    times OFF_MB_CR3 - ($ - ap_tramp_start) db 0
    dq 0                                    ; mailbox: cr3   (BSP fills)
    dq 0                                    ;          entry
    dq 0                                    ;          stack
    dd 0                                    ;          idx
    dd 0                                    ;          efer_or (DDR-757; BSP fills)
ap_tramp_end:
