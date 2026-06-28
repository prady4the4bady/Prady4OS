# DDR-702 — Ring-3 framebuffer surface (Layer 7, compositor foundation)

> Layer-7 slice following the GPU framebuffer (ADR-028). DDR before code, per the
> brief. Gives ring 3 a way to draw to the screen and present it — the surface
> every shell/compositor slice renders through.

## Context

ADR-028 brought up the VirtIO-GPU and presents a kernel-drawn framebuffer once at
boot. To build any userspace UI, a ring-3 process must be able to (a) learn the
screen geometry, (b) draw pixels, and (c) present them. This slice adds that
surface as three syscalls, without yet introducing Wayland/wlroots (those remain
large library ports; this is the raw surface they would sit on).

## Decisions

### D1 — Three append-only syscalls (43–45)
```
SYS_FB_INFO(struct fb_info __user *) -> 0 | -ENODEV    (w, h, stride, bpp)
SYS_FB_MAP()                         -> user VA | -ENODEV
SYS_FB_FLUSH()                       -> 0 | -ENODEV     (present the whole FB)
```
`fb_info = { u32 width, height, stride, bpp }`, copied out via `copyout`.

### D2 — Shared single framebuffer, mapped read/write into the caller
`SYS_FB_MAP` maps the existing GPU framebuffer's physical pages (the ADR-028 PMM
allocation) into the calling process's address space at a fixed per-process VA —
**`0x8700000000`**, inside the user range and below the anonymous mmap arena
(`VMM_MMAP_BASE = 0x8800000000`), so it never collides with `mmap`. Flags
`VMM_USER | VMM_RW | VMM_NX` (data, never executable — W^X / ADR-021). One shared
front buffer this slice; per-client buffers + compositing come with the shell.

### D3 — Present path works at runtime (IF=0 syscall context)
`SYS_FB_FLUSH` issues `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` for the whole
surface. The GPU control-queue wait (`gpu_cmd`) must therefore work both at boot
(interrupts enabled) and inside a syscall (IF cleared by SYSCALL `SFMASK`). So
`gpu_cmd` is refactored to **save the caller's RFLAGS, wait with `sti; hlt; cli`,
and restore RFLAGS** before returning — identical in effect to the boot path
(IF=1 in, IF=1 out) while letting a ring-3-initiated flush briefly enable
interrupts so QEMU's TCG backend processes the kick, then restore the syscall's
IF=0. No busy-spin (it starves the backend; ADR-028).

### D4 — Absent GPU degrades cleanly
With no GPU up (`virtio_gpu_fb()` returns 0), all three syscalls return `-ENODEV`;
a ring-3 program checks `SYS_FB_INFO` first and exits cleanly. So the surface is
inert on the 32 non-GPU gates and only live under `smoke-fb` (QEMU_GPU=1).

## Gate

`smoke-fb` (CI, `QEMU_GPU=1`): a ring-3 program (`user/fbtest.c`) calls
`SYS_FB_INFO`, `SYS_FB_MAP`, draws a deterministic pattern into the mapped surface,
`SYS_FB_FLUSH`es, and prints `PRADYOS_FB_DRAW_OK <w>x<h>`. Proves the full
userspace draw→present path end to end (headless QEMU still ACKs the 2D commands).

## Non-goals (later slices)
Double buffering / page-flip, damage rectangles, per-client surfaces + a
compositor, input (mouse/keyboard) routing, and the wlroots/Wayland protocol —
all deferred. This is the single raw surface those build on.
