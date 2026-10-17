#!/usr/bin/env python3
"""DDR-1147 sec.1.5: check PRADYOS_THEME lines in a serial capture against
user/theme_palette.h (the GENERATED palette), so the expected accents are never
a second hand copy of the values.

  theme_check.py <log> demo     every ambiance printed by the boot demo cycle
                                matches THEME_AC[mode][amb]; all four present
  theme_check.py <log> modes    both modes appear, and every THEME line matches

Every PRADYOS_THEME line in the capture is checked in both forms: a line that
does not match its own (mode, ambiance) cell is a failure wherever it appears.
"""
import re, sys

TOKENS = ('DAWN', 'DAY', 'DUSK', 'NIGHT')


def table(path='user/theme_palette.h'):
    txt = open(path).read()
    body = txt[txt.index('THEME_AC[2][4][3]'):]
    rows = re.findall(r'\{\s*(\{0x[0-9A-F]{2},0x[0-9A-F]{2},0x[0-9A-F]{2}\}(?:,\s*\{[^}]*\}){3})\s*\}', body)
    if len(rows) != 2:
        sys.exit(f'theme_check: cannot parse THEME_AC from {path}')
    out = []
    for r in rows:
        cells = re.findall(r'0x([0-9A-F]{2}),0x([0-9A-F]{2}),0x([0-9A-F]{2})', r)
        out.append([''.join(c) for c in cells])
    return out


def main(argv):
    if len(argv) != 2 or argv[1] not in ('demo', 'modes'):
        sys.exit(__doc__)
    tab = table()
    lines = re.findall(r'PRADYOS_THEME mode=([01]) amb=([A-Z]+) name=\S+ ac=([0-9A-F]{6})',
                       open(argv[0], errors='replace').read())
    if not lines:
        sys.exit('theme_check: FAIL -- no PRADYOS_THEME line in the capture')
    bad = [(m, a, ac) for m, a, ac in lines if tab[int(m)][TOKENS.index(a)] != ac]
    for m, a, ac in bad:
        print(f'theme_check: FAIL -- mode={m} amb={a} painted {ac}, '
              f'palette says {tab[int(m)][TOKENS.index(a)]}')
    if bad:
        sys.exit(1)
    if argv[1] == 'demo':
        seen = {a for _, a, _ in lines}
        if seen != set(TOKENS):
            sys.exit(f'theme_check: FAIL -- demo cycle covered {sorted(seen)}, not all four')
    else:
        modes = {m for m, _, _ in lines}
        if modes != {'0', '1'}:
            sys.exit(f'theme_check: FAIL -- only mode(s) {sorted(modes)} painted; '
                     'a toggle that does not re-target the accent passes nothing here')
    print(f'theme_check: PASS -- {len(lines)} PRADYOS_THEME line(s) match theme_palette.h')


if __name__ == '__main__':
    main(sys.argv[1:])
