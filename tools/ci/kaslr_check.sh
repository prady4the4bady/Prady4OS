#!/usr/bin/env bash
# tools/ci/kaslr_check.sh -- smoke-kaslr arms B, C, D across the boots' serial
# logs (DDR-1179 Amendment 3). Arms A and E are EXTRA_SENTINELs of each boot.
#   B  every boot: "PRADYOS_KASLR_B applied=1 src=rdseed|rdrand" (hardware, not tsc)
#   C  at least 2 distinct "[kaslr] slot=" values across the boots
#   D  every boot's "[kaslr] D fault_rip=" resolves (sym_at.sh, build/kernel.elf)
#      to probe_kaslr_fault
set -u
elf=build/kernel.elf
fail=0
slots=()
for log in "$@"; do
    b=$(grep -a -o 'PRADYOS_KASLR_B applied=[0-9]* src=[a-z]*' "$log" | head -1)
    case "$b" in
        "PRADYOS_KASLR_B applied=1 src=rdseed"|"PRADYOS_KASLR_B applied=1 src=rdrand") ;;
        *) echo "[kaslr] FAIL arm B $log: '${b:-<missing>}'"; fail=1 ;;
    esac
    s=$(grep -a -o '\[kaslr\] slot=[0-9]*' "$log" | head -1 | sed 's/.*slot=//')
    [ -n "$s" ] || { echo "[kaslr] FAIL arm C $log: no slot line"; fail=1; }
    slots+=("${s:-none}")
    rip=$(grep -a -o '\[kaslr\] D fault_hit=1 fault_rip=0x[0-9A-Fa-f]*' "$log" | head -1 | sed 's/.*fault_rip=//')
    if [ -z "$rip" ]; then
        echo "[kaslr] FAIL arm D $log: no fault_hit=1 line"; fail=1
    else
        sym=$(bash tools/ci/sym_at.sh "$rip" "$elf" 2>&1)
        case "$sym" in
            "sym_at: probe_kaslr_fault "*) ;;
            *) echo "[kaslr] FAIL arm D $log: rip=$rip -> '$sym'"; fail=1 ;;
        esac
    fi
done
distinct=$(printf '%s\n' "${slots[@]}" | sort -u | grep -vc '^none$')
if [ "$distinct" -lt 2 ]; then
    echo "[kaslr] FAIL arm C: ${#slots[@]} boots, $distinct distinct slot(s)"
    fail=1
fi
[ "$fail" -eq 0 ] && echo "[kaslr] PASS arms B C D: ${#slots[@]} boots, $distinct distinct slots"
exit "$fail"
