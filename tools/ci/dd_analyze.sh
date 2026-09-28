#!/bin/bash
# DDR-1151: per-capture catalogue of forced double-dispatch captures
# usage: DIR=<capture dir> ELF=<the binary that produced them> dd_analyze.sh   (INV.18: resolve against its OWN elf)
: "${DIR:?}" "${ELF:?}"
cd "$(dirname "$0")/../.."
for f in $DIR/*.log*; do
  case $f in *.make) continue;; esac
  n=$(basename $f)
  hbs=$(grep -c '^\[hb\] t=' $f); lasthb=$(grep -o '^\[hb\] t=[0-9]*' $f | tail -1)
  sysf=$(grep -c 'SYSFSTAT OK' $f)
  echo "== $n lines=$(wc -l <$f) hb=$hbs $lasthb sysfstat=$sysf"
  grep -oE '\[schedcheck\].*' $f | sed -E 's/.*tid=([0-9]+) pid=([0-9]+).*rq_on=([0-9?]) disp=([0-9]+) saves=([0-9]+).*/  schedcheck tid=\1 pid=\2 rq_on=\3 d-s=\4-\5/' | sort | uniq -c
  for r in $(grep -oE 'apfreeze\] cpu=[0-9]+ .*rip=0x[0-9A-F]+' $f | grep -oE 'rip=0x[0-9A-F]+' | sort -u | cut -d= -f2); do echo "  apfreeze rip $r -> $(bash tools/ci/sym_at.sh $r $ELF 2>/dev/null)"; done
  grep -oE 'exception: [^,]*|#[A-Z]+ [a-z -]+ vector=0x[0-9a-f]+' $f | sort | uniq -c | sed 's/^/  /'
  for r in $(grep -oE '^RIP=0x[0-9A-F]+|RIP=0x[0-9A-F]+,' $f | grep -oE '0x[0-9A-F]+' | sort -u); do echo "  panic RIP $r -> $(bash tools/ci/sym_at.sh $r $ELF 2>/dev/null)"; done
  for r in $(grep -oE 'loser_rip=0x[0-9A-F]+' $f | cut -d= -f2 | sort -u); do [ $r = 0x0 ] || echo "  loser_rip $r vec=$(grep -oE 'loser_vec=[0-9]+' $f|tail -1) -> $(bash tools/ci/sym_at.sh $r $ELF 2>/dev/null)"; done
  echo "  last: $(tail -1 $f | cut -c1-110)"
done
