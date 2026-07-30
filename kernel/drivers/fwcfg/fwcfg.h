/* kernel/drivers/fwcfg/fwcfg.h — DDR-804: per-boot probe selection.
 *
 * Reads a comma-separated probe list the harness passes as
 *   -fw_cfg name=opt/org.pradyos/probes,string=<csv>
 * so an opt-in probe can exist in exactly one gate instead of every gate.
 *
 * Fails closed: if the fw_cfg device is absent or its signature does not match,
 * the probe set is empty and every opt-in probe stays off. An unselected probe
 * fails its OWN gate (loud, local); a wrongly-selected one would perturb
 * unrelated gates (silent, remote), so "off" is the safe direction.
 */
#pragma once

/* Read the probe list once, at boot. Safe to call before any probe spawns and
 * safe to call when no fw_cfg device exists. Bounded: no retry, no wait. */
void fwcfg_init(void);

/* Is `name` present in the boot-time probe list? Returns 0 when fwcfg_init()
 * has not run, when the device was absent, or when the name is not listed.
 * Matches whole comma-separated fields only, so "net" does not match "privnet". */
int probe_enabled(const char *name);
