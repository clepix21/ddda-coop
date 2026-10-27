"""Find vtables of MtDTI classes through their getDTI() thunk (mov eax, <dti>; ret) and diff them.
Usage: python -I vtables.py <exe> <dti.csv> <class>... [--slots N]
Prints each class's vtable VA and, slot by slot, which classes share or override the entry.
"""
import csv
import re
import struct
import sys

exe, dtic = sys.argv[1], sys.argv[2]
args = sys.argv[3:]
nslots = int(args[args.index('--slots') + 1]) if '--slots' in args else 120
names = [a for i, a in enumerate(args) if a != '--slots' and (i == 0 or args[i - 1] != '--slots')]
b = open(exe, 'rb').read()
pe = struct.unpack_from('<I', b, 0x3C)[0]
n = struct.unpack_from('<H', b, pe + 6)[0]
opt = struct.unpack_from('<H', b, pe + 20)[0]
base = struct.unpack_from('<I', b, pe + 24 + 28)[0]
secs = {}
for i in range(n):
    nm, vs, va, rs, ro = struct.unpack_from('<8sIIII', b, pe + 24 + opt + 40 * i)
    secs[nm.rstrip(b'\0').decode()] = (va, vs, ro, rs)


def off2va(o, s):
    va, vs, ro, rs = secs[s]
    return base + va + o - ro


dti = {r['name']: int(r['dti'], 16) for r in csv.DictReader(open(dtic))}
tx = secs['.text']
rd = secs['.rdata']
text = b[tx[2]:tx[2] + tx[3]]
rdata = b[rd[2]:rd[2] + rd[3]]
tables = {}
for name in names:
    d = dti[name]
    getters = [off2va(tx[2] + m.start(), '.text') for m in re.finditer(re.escape(b'\xB8' + struct.pack('<I', d) + b'\xC3'), text)]
    vts = []
    for g in getters:
        for m in re.finditer(re.escape(struct.pack('<I', g)), rdata):
            if m.start() % 4 == 0:
                vts.append((off2va(rd[2] + m.start(), '.rdata'), m.start()))
    # vtable start: walk back while the previous dword is a .text pointer
    starts = set()
    lo, hi = base + tx[0], base + tx[0] + tx[1]
    for slotva, ro in vts:
        o = ro
        while o >= 4 and lo <= struct.unpack_from('<I', rdata, o - 4)[0] < hi:
            o -= 4
        starts.add((off2va(rd[2] + o, '.rdata'), (ro - o) // 4, o))
    print(f'{name}: getDTI {[hex(g) for g in getters]} vtables {[(hex(s), f"getDTI slot {k}") for s, k, _ in starts]}')
    if starts:
        s, k, o = sorted(starts)[0]
        tables[name] = [struct.unpack_from('<I', rdata, o + 4 * i)[0] for i in range(nslots)]
if len(tables) > 1:
    print('\nslot  ' + '  '.join(f'{nm:>12}' for nm in tables))
    for i in range(nslots):
        row = [tables[nm][i] for nm in tables]
        if len(set(row)) > 1:
            print(f'{i:4}  ' + '  '.join(f'  0x{v:08X}' for v in row))
