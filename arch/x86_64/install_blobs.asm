; arch/x86_64/install_blobs.asm -- DDR-1143 sec.10.6/10.9: the three loader
; blobs SYS_INSTALL lays onto a disk. The kernel it writes alongside them is
; NOT embedded here: it is the pristine copy the loader left (kimg_get), so
; the installed kernel is byte-for-byte the one that booted.
;
; build/BOOTX64.EFI is linked with lld-link -Brepro (sec.10.7). Without that its
; PE timestamp would make kernel.bin's hash change on every rebuild.
section .rodata
align 16
global inst_stage1
global inst_stage1_end
inst_stage1:
    incbin "build/stage1.bin"
inst_stage1_end:
align 16
global inst_stage2
global inst_stage2_end
inst_stage2:
    incbin "build/stage2.bin"
inst_stage2_end:
align 16
global inst_efi
global inst_efi_end
inst_efi:
    incbin "build/BOOTX64.EFI"
inst_efi_end:
