"""Timeline and checks from a coop.log written by the mod's probe.

Usage: python -I analyze_log.py <coop.log> [--window NAME START END]... [--expect EXPR]...
  START/END are seconds after the DLL loaded (same clock as bot.txt).
  EXPR: "<window> <arisen|pawn> <op> <distance>" (op is < or >), or "press <arisen|pawn> <hexbits> <count>"
  meaning at least <count> PRESS lines with exactly those bits, or "pressmax <who> <hexbits> <n>":
  at most <n> PRESS lines with any of those bits. Pawn presses count only while possession is on.
  Exit code 1 if any expectation fails.
Prints per-second positions of the Arisen and the main pawn, PRESS events, possession events,
then for each window the distance each character moved and the pawn's heading.
"""
import math
import re
import sys
from datetime import datetime

path = sys.argv[1]
windows = []
expects = []
cmc = '0'
args = sys.argv[2:]
while args:
    if args[0] == '--window':
        windows.append((args[1], float(args[2]), float(args[3])))
        args = args[4:]
    elif args[0] == '--expect':
        expects.append(args[1])
        args = args[2:]
    elif args[0] == '--cmc':  # the pawn to follow (mCmcId; default 0, the main pawn)
        cmc = args[1]
        args = args[2:]
    else:
        args = args[1:]

lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
ts = lambda l: datetime.strptime(l[:12], '%H:%M:%S.%f')
t0 = ts(lines[0])
rel = lambda l: (ts(l) - t0).total_seconds() % 86400  # a run may cross midnight (seen 2026-10-27)

pawn = arisen = None
for l in lines:
    m = re.search(rf'probe: new uCmc (\w+) cmcId {cmc} ', l)
    if m and not pawn:
        pawn = m.group(1)
    m = re.search(r'probe: new uPlayer (\w+)', l)
    if m and not arisen:
        arisen = m.group(1)

pos = {'pawn': [], 'arisen': []}
who = None
for l in lines:
    m = re.search(r'probe: (uPlayer|uCmc) (\w+) mode (\d)', l)
    if m:
        who = 'pawn' if m.group(2) == pawn else 'arisen' if m.group(2) == arisen else None
        mode = int(m.group(3))
        continue
    m = re.search(r'probe:   \+30 .*\| \+40 (\S+) (\S+) (\S+)', l)
    if m and who:
        pos[who].append((rel(l), float(m.group(1)), float(m.group(3)), mode))
        who = None
    if re.search(r'possess: (ON|OFF|auto)|redirect:|PRESS|BLOCKED|bot: t=', l):
        print(f'{rel(l):8.2f}  {l[13:]}')


def at(series, t):
    best = None
    for p in series:
        if p[0] <= t:
            best = p
    return best


print('\nper-second positions (t, arisen x z, pawn x z mode)')
for p in pos['pawn']:
    a = at(pos['arisen'], p[0] + 0.1)
    print(f'{p[0]:8.2f}  arisen ({a[1]:9.2f}, {a[2]:9.2f})   pawn ({p[1]:9.2f}, {p[2]:9.2f}) mode {p[3]}' if a else '')

moved = {}
for name, s, e in windows:
    print(f'\nwindow {name} [{s}, {e}] s')
    for who_ in ('arisen', 'pawn'):
        a, b = at(pos[who_], s), at(pos[who_], e)
        if not a or not b:
            print(f'  {who_}: no samples')
            continue
        dx, dz = b[1] - a[1], b[2] - a[2]
        d = math.hypot(dx, dz)
        moved[(name, who_)] = d
        head = f'heading ({dx / d:+.2f}, {dz / d:+.2f})' if d > 1 else ''
        print(f'  {who_}: moved {d:8.1f} {head}')

# Pawn presses only count while possession is ON (before that, the pawn AI presses too).
presses = []
possessed = False
for l in lines:
    if 'possess: ON' in l:
        possessed = True
    elif 'possess: OFF' in l:
        possessed = False
    elif 'probe: PRESS' in l:
        w, bits = re.search(r'PRESS (\w+) (\w+)', l).groups()
        if w == 'arisen' or possessed:
            presses.append((w, bits))
failed = 0
print()
for ex in expects:
    f = ex.split()
    if f[0] == 'pressmax':  # at most n PRESS lines whose bits include any of <hexbits>
        n = sum(1 for w, bits in presses if w == f[1] and int(bits, 16) & int(f[2], 16))
        ok = n <= int(f[3])
        got = f'{n} presses'
    elif f[0] == 'nopress':
        n = sum(1 for w, _ in presses if w == f[1])
        ok = n == 0
        got = f'{n} presses'
    elif f[0] == 'press':
        n = sum(1 for w, bits in presses if w == f[1] and int(bits, 16) == int(f[2], 16))
        ok = n >= int(f[3])
        got = f'{n} presses'
    else:
        d = moved.get((f[0], f[1]))
        ok = d is not None and (d < float(f[3]) if f[2] == '<' else d > float(f[3]))
        got = 'no samples' if d is None else f'{d:.1f}'
    failed += not ok
    print(f'{"PASS" if ok else "FAIL"}  {ex}  (got {got})')
sys.exit(1 if failed else 0)
