"""Lists the exe addresses (0x00401000..0x01A0C000) used in the mod's code, comments and strings left out.
Usage: python -I list_addrs.py <src dir> [out file]"""
import glob
import os
import re
import sys

STR = re.compile(r'"(?:\\.|[^"\\\n])*"')


def strip(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    out = []
    for line in s.split('\n'):
        line = STR.sub('""', line)
        i = line.find('//')
        out.append(line if i < 0 else line[:i])
    return out


def main():
    tot = {}
    for f in sorted(glob.glob(os.path.join(sys.argv[1], '*.cpp')) + glob.glob(os.path.join(sys.argv[1], '*.h'))):
        lines = strip(open(f, encoding='utf-8', errors='replace').read())
        for i, l in enumerate(lines):
            for m in re.finditer(r'0x0*([0-9A-Fa-f]{6,8})\b', l):
                v = int(m.group(1), 16)
                if 0x00401000 <= v < 0x01A0C000:
                    tot.setdefault(v, []).append(f'{os.path.basename(f)}:{i + 1}')
    rows = [f'{v:08X} ' + ' '.join(tot[v]) for v in sorted(tot)]
    if len(sys.argv) > 2:
        open(sys.argv[2], 'w').write('\n'.join(rows) + '\n')
    print(len(rows))
    print('\n'.join(rows))


main()
