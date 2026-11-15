"""Wraps the exe addresses of the mod's code in game::va(...), so each one goes through the table of the
running build (Steam or GOG). Comments and strings are left alone; so are the plain bounds of range checks.
Usage: python -I wrap_addrs.py <src dir> [--dry]"""
import glob
import os
import re
import sys

# not addresses of one thing: bounds of scans and vtable sanity checks, masks
KEEP = {0x00401000, 0x01700000, 0x01300000, 0x00FFFFFF, 0x01621000, 0x019AB1E0, 0x018C0000, 0x01A00000,
        0x018FA400, 0x018FA600,
        0x00434444,  # the net magic 'DDC', not an address
        0x0040FA31, 0x0040F8D9}  # save_guard.cpp: each build's own patch site
LIT = re.compile(r'(?<![\w.])0x0*([0-9A-Fa-f]{6,8})\b')


def code_spans(line):
    """(start, end) of the parts of a line that are code: no // comment, no string or char literal"""
    spans, i, start = [], 0, 0
    while i < len(line):
        c = line[i]
        if c == '/' and line.startswith('//', i):
            spans.append((start, i))
            return spans
        if c in '"\'':
            spans.append((start, i))
            q = c
            i += 1
            while i < len(line) and line[i] != q:
                i += 2 if line[i] == '\\' else 1
            i += 1
            start = i
            continue
        i += 1
    spans.append((start, len(line)))
    return spans


def main():
    dry = '--dry' in sys.argv
    total = 0
    for f in sorted(glob.glob(os.path.join(sys.argv[1], '*.cpp')) + glob.glob(os.path.join(sys.argv[1], '*.h'))):
        if os.path.basename(f) in ('addr_gog.h', 'build.cpp'):  # the table itself
            continue
        lines = open(f, encoding='utf-8', newline='').read().split('\n')
        in_block = False
        changed = 0
        for n, line in enumerate(lines):
            if in_block:
                if '*/' in line:
                    in_block = False
                continue
            if line.lstrip().startswith('/*') and '*/' not in line:
                in_block = True
                continue
            out, pos = [], 0
            for a, b in code_spans(line):
                for m in LIT.finditer(line, a, b):
                    v = int(m.group(1), 16)
                    if not (0x00401000 <= v < 0x01A0C000) or v in KEEP:
                        continue
                    before = line[max(0, m.start() - 9):m.start()]
                    if before.endswith('game::va(') or before.endswith('va('):
                        continue
                    out.append(line[pos:m.start()])
                    out.append(f'game::va({m.group(0)})')
                    pos = m.end()
                    changed += 1
            if pos:
                out.append(line[pos:])
                new = ''.join(out)
                if 'constexpr ' in new:  # game::va() runs when the DLL loads: a const, not a constant expression
                    new = new.replace('constexpr ', 'inline const ' if f.endswith('.h') else 'const ', 1)
                if dry:
                    print(f'{os.path.basename(f)}:{n + 1}: {new.strip()[:150]}')
                lines[n] = new
        if changed and not dry:
            open(f, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
        total += changed
    print('wrapped', total)


main()
