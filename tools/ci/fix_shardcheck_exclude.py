import io
p = "tools/ci/shard_check.sh"
s = io.open(p, encoding="utf-8", newline="").read()
old = 'EXCLUDE="smoke-aarch64 smoke-riscv64 smoke-agent-live smoke-selftest smoke-sfs-btree-smp4"'
assert s.count(old) == 1
new = ('# smoke-fs-liveness: DDR-777/BUG-1 diagnostic. It REBUILDS the kernel with\n'
       '# BSP_LIVENESS=1, whose per-iteration churn output fills the 4 KiB dmesg ring\n'
       '# and evicts smoke-dmesg\'s required marker (DDR-790). It must never run in the\n'
       '# shared-image shard matrix. Remove this exclusion when BUG-1 closes and the\n'
       '# target is deleted.\n'
       + old[:-1] + ' smoke-fs-liveness"')
s = s.replace(old, new, 1)
io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("smoke-fs-liveness excluded with reason")
