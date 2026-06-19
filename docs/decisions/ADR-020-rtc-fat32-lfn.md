# ADR-020: CMOS RTC + FAT32 long names & timestamps

- **Status:** Accepted 2026-06-18
- **Phase:** 4 (slice 4j, part 2)

## Context

FAT32 directory entries carry create/write/access timestamps and (via VFAT)
long filenames. Until now the kernel had no wall-clock, so created files were
stamped 1980-00-00, and only 8.3 short names were read. Real interop needs both:
a clock to stamp writes, and long-name read/lookup so files written by other
systems (which almost always have long names) are accessible.

## Decision

- **CMOS/RTC driver** (`kernel/drivers/rtc/`): reads the battery-backed clock via
  ports 0x70/0x71, handling BCD-vs-binary and 12-vs-24-hour per status register B,
  with a stable double-read across the update-in-progress flag. Exposes
  `rtc_now()` (a wall-clock for CLOCK_REALTIME later) and `rtc_fat_datetime()`
  (packed FAT date+time). The 2-digit year register is interpreted as 21st
  century. This is the deferred Layer-3 RTC, pulled in now for filesystem
  timestamps.
- **FAT32 timestamps**: `create` stamps the new entry's create/write/access
  fields from the RTC; each `write` refreshes the write timestamp.
- **FAT32 VFAT long names (read)**: `dir_scan` now reconstructs the long name
  from the preceding LFN entries (UTF-16 → ASCII, `?` for non-ASCII) and matches
  a requested path component **case-insensitively** against the long name, with a
  fallback to the 8.3 short-name key. `readdir` returns the long name when
  present. All directory lookups (`open`, `walk_dir`, `resolve_parent`, `create`
  existence check, `unlink`) were switched from 8.3-key matching to
  name+length matching so long names work everywhere.

## Consequences / deferred

- **LFN read-only.** `create` still writes **8.3 short names** (long-name *write*
  — generating LFN entries + a unique short alias + checksum — is deferred). The
  kernel reads any long name but creates short ones.
- Non-ASCII long names are lossy (`?`); full UTF-8 awaits a Unicode layer.
- No LFN checksum validation against the 8.3 entry (orphan-LFN robustness) — the
  common, well-formed case is handled.
- RTC year heuristic (2000+) is fine until 2100; no NTP/timezone (UTC offset is
  a Layer-7 concern, see the UI brief's ambiance clock).

## Verification

QEMU q35: the kernel reads `/LongFileName.txt` (a VFAT long name written by
mtools) returning `"long name read works"`, while 8.3 (`/HELLO.TXT`) and nested
(`/DOCS/NOTE.TXT`) reads still pass; `rtc_now()` prints the live date
(`[rtc] 2026-6-19 …`), and kernel-created files now carry real timestamps
(visible via host `mdir`, no longer 1980). `make smoke-fs` asserts the long-name
line and the `[rtc] 20xx` date. `-Werror` clean.
