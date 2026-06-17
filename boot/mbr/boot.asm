; boot/mbr/boot.asm
; ============================================================================
; PRADYOS-BOOT — Stage 1 (legacy BIOS MBR), Phase 1 first slice.
;
; The BIOS loads this 512-byte sector to physical 0x0000:0x7C00 in 16-bit real
; mode and jumps to it (boot signature 0xAA55 required at offset 510). This
; slice does the minimum to prove the boot pipeline end-to-end: it writes the
; boot sentinel to the COM1 serial port (so the headless QEMU smoke test can
; grep it) and to the BIOS console via INT 10h (so a human watching sees it),
; then halts.
;
; NOT YET DONE (later Phase 1 slices): A20 enable, protected-mode transition,
; INT 15h E820 memory map, CPUID vendor/topology detection, loading the kernel
; ELF to high memory, and the hardware-info handoff struct. Those arrive once
; Phase 2a produces a kernel to load.
;
; References (verify against these, do not trust from memory):
;   - OSDev Wiki, "Serial Ports": 16550 UART register map relative to the base
;     I/O port (COM1 = 0x3F8): +0 THR/RBR (and divisor LSB when DLAB=1),
;     +1 IER (divisor MSB when DLAB=1), +2 FCR, +3 LCR (bit 7 = DLAB),
;     +4 MCR, +5 LSR (bit 5 = THRE, transmit holding register empty).
;   - OSDev Wiki, "Rolling Your Own Bootloader" (real-mode entry, 0x7C00, 0xAA55).
;   - Ralf Brown's Interrupt List, INT 10h/AH=0Eh (teletype output).
;
; Build: nasm -f bin boot.asm -o boot.bin   (output is exactly 512 bytes)
; ============================================================================

BITS 16
ORG 0x7C00

COM1 equ 0x3F8

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00          ; stack grows down from just below our load address
    sti

    call serial_init

    mov si, msg
.print:
    lodsb                   ; AL = DS:[SI], SI++
    test al, al
    jz .done
    call serial_putc        ; preserves AL
    call bios_putc          ; uses AL
    jmp .print
.done:
    cli
.hang:
    hlt
    jmp .hang

; --- COM1 init: 115200 baud, 8N1, FIFO on. -----------------------------------
serial_init:
    mov dx, COM1 + 1        ; IER: disable all UART interrupts
    mov al, 0x00
    out dx, al
    mov dx, COM1 + 3        ; LCR: set DLAB to access the baud divisor
    mov al, 0x80
    out dx, al
    mov dx, COM1 + 0        ; divisor LSB = 1 -> 115200 baud
    mov al, 0x01
    out dx, al
    mov dx, COM1 + 1        ; divisor MSB = 0
    mov al, 0x00
    out dx, al
    mov dx, COM1 + 3        ; LCR: 8 bits, no parity, 1 stop; clears DLAB
    mov al, 0x03
    out dx, al
    mov dx, COM1 + 2        ; FCR: enable FIFO, clear RX/TX, 14-byte threshold
    mov al, 0xC7
    out dx, al
    mov dx, COM1 + 4        ; MCR: DTR + RTS + OUT2
    mov al, 0x0B
    out dx, al
    ret

; --- Send one byte over COM1 (AL). Preserves AX and DX. ----------------------
serial_putc:
    push dx
    push ax
    mov ah, al              ; stash the char in AH
.sp_wait:
    mov dx, COM1 + 5        ; LSR
    in al, dx
    test al, 0x20           ; THRE: transmit holding register empty?
    jz .sp_wait
    mov dx, COM1 + 0        ; THR
    mov al, ah
    out dx, al
    pop ax
    pop dx
    ret

; --- Print one byte to the BIOS console (AL) via teletype. Preserves AX, BX. -
bios_putc:
    push bx
    push ax
    mov ah, 0x0E            ; INT 10h teletype output
    mov bx, 0x0007          ; BH=0 (page 0), BL=0x07 (light grey, text modes)
    int 0x10
    pop ax
    pop bx
    ret

msg db "PRADYOS BOOT OK", 13, 10, 0

; --- Pad to 510 bytes, then the 2-byte boot signature. -----------------------
times 510 - ($ - $$) db 0
dw 0xAA55
