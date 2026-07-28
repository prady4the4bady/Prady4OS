# DDR-797 — the serial-flood emitter: a "wild" pointer that is the load base

**Status:** diagnosed and fixed in this slice.
**Date:** 2026-07-29
**Closes:** DDR-791 finding 1 (serial flooding, emitter unidentified).
**Completes:** BUG-1, together with DDR-796 (CMOS/RTC SMP race).
**Corrects:** DDR-791's A/B conclusion, which was **invalid** — see "The earlier
experiment was broken".

## What was happening

83% of every boot's serial output was a 3,188-byte binary blob repeated ~16–22
times: 80,637 non-printable bytes out of 97,564.

The blob is `user/syscallfuzz.c`'s own ELF image, dumped to the console by the
kernel — legitimately, on the probe's own instruction.

`user/user.ld` line 13:

```
. = 0x8000000000;      /* user image base, PML4 slot 1 */
```

`user/syscallfuzz.c`'s `WILD[]`, commented *"NULL, kernel VA, non-canonical,
**unmapped user**, a low-but-plausible junk address"*:

```c
static const unsigned long WILD[] = {
    0UL, 0xdeadbeefUL, 0xffffffff80000000UL,
    0x8000000000UL,          /* <-- this is the LOAD BASE, not unmapped */
    0xffffffffffffffffUL, 0x41414141UL
};
```

`0x8000000000` is not an unmapped user address. It is the base of the probe's own
read+execute segment — the *most* mapped address in the process.

The probe issues `nsi(num, p, p, p)` — the same wild value as all three
arguments. For `SYS_WRITE` with `p = 0x8000000000`:

| argument | value | result |
|---|---|---|
| `fd` | `(int)0x8000000000` = **0** | truncates to fd 0, which resolves to the console |
| `buf` | `0x8000000000` | the image base — **mapped and readable** |
| `count` | `0x8000000000` | ~512 GB |

`fd_write_user` then does exactly what `write(2)` should: copy a chunk in,
`kwrite` it to the console, repeat, and stop when `copyin` faults at the end of
the mapping. That walks the probe's whole image onto the serial port and returns
a short count.

**The kernel is not at fault.** Every layer behaved correctly; the probe asked
for it.

## The probe's own comment states the false premise

```
* NB: SYS_READ (5) is deliberately EXCLUDED — ... SYS_WRITE stays: wild bufs
* fault at copyin (-EFAULT), so nothing is emitted.
```

That reasoning is sound *if* every `WILD` entry is unmapped. One is not, so the
premise fails and the conclusion with it. The comment is why the flood survived
this long: it reads as a considered exemption rather than an assumption to check.

## The earlier experiment was broken

DDR-791 reported three A/B arms with **byte-identical** counts (80,640 each) and
concluded the fuzz probe was not the emitter. That conclusion was wrong, and the
identical counts were the tell: the user ELF is embedded into `user_image.o` by
`incbin`, so editing `user/syscallfuzz.c` and running `make image` **rebuilt
nothing** — all three arms booted the same image.

Re-run with `build/syscallfuzz.elf`, `build/user_image.o`, `kernel.bin` and the
image all removed first, and with the artefact SHA printed per arm to prove the
build actually changed:

| arm | serial total | non-printable | kernel.bin sha |
|---|---|---|---|
| baseline | 97,564 | 80,637 (83%) | `7186a052…` |
| `FUZZ.ELF` spawn removed | **11,450** | **99 (0.9%)** | `b9aedef8…` |
| only bad-NSI numbers | 11,560 | 102 | `91427fd8…` |
| only wild pointers | 183,592 | 163,818 | `12330d14…` |

The wild-pointer branch is the emitter, and doubling its share doubles the flood.

**Lesson, recorded because it cost two wrong conclusions:** an A/B whose arms
produce byte-identical output has not proven equivalence — it has failed to
rebuild. Print an artefact hash per arm.

## Fix

Replace the load base in `WILD[]` with an address that is genuinely unmapped,
and say why in the source so it is not "corrected" back:

```c
0x0000600000000000UL   /* canonical, user half, nothing maps here */
```

This preserves the probe's actual purpose — proving the uaccess path returns
`-EFAULT` rather than faulting the kernel — while removing an entry that tested
the opposite of what it claimed.

### Considered and rejected

* **Remove `SYS_WRITE` from `SAFE[]`.** Loses real coverage: `write` is exactly
  the syscall where a bad buffer must produce `-EFAULT` and not a kernel fault.
  The problem was never that `write` is tested; it is that the pointer was not
  wild.
* **Make `fd` 0 non-writable in the kernel.** A tty is legitimately read-write
  in POSIX, musl programs use fd 0 as a terminal, and this would change syscall
  semantics to work around a bad test constant. Wrong layer.
* **Rate-limit or gate console output behind a flag.** Suppresses the symptom
  and leaves a probe that silently dumps its own address space.

## Why this mattered beyond the noise

DDR-796 fixed the CMOS/RTC SMP race and BUG-1 still occurred in 1/7 runs, because
the flood delays boot: the AETHER daemon's agent spawn slipped to ~byte 98,130,
right at the edge of the metrics probe's 120-second window, so the probe
occasionally timed out first. Removing 86% of the serial traffic removes that
delay. The two defects were independent causes of one symptom.

## Gate

`make smoke-serialflood` — boots normally and asserts the serial capture stays
under **32 KiB**. Measured after the fix: ~11.5 KiB. The pre-fix boot was 97.5 KiB,
so the ceiling discriminates by a factor of three in each direction rather than
sitting at a hair's breadth.
