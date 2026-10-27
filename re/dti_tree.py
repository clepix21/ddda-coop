"""Rebuild the MtDTI class tree of an MT Framework x86 exe from its static DTI constructors.
Pattern: [push size(6A ib | 68 id)] 68 <parent> 68 <name> B9 <dti> E8 <rel32 -> ctor>
Usage: python -I dti_tree.py <exe> <ctor VA hex> <root class>... [--csv out.csv]
Prints the subtree under each root, and the ancestor chain of each root.
"""
import re
import struct
import sys

exe, ctor = sys.argv[1], int(sys.argv[2], 16)
rest = sys.argv[3:]
csv = rest[rest.index('--csv') + 1] if '--csv' in rest else None
roots = [r for r in rest if r != '--csv' and r != csv]
b = open(exe, 'rb').read()
pe = struct.unpack_from('<I', b, 0x3C)[0]
n = struct.unpack_from('<H', b, pe + 6)[0]
opt = struct.unpack_from('<H', b, pe + 20)[0]
base = struct.unpack_from('<I', b, pe + 24 + 28)[0]
secs = [struct.unpack_from('<8sIIII', b, pe + 24 + opt + 40 * i) for i in range(n)]


def off2va(o):
    for nm, vs, va, rs, ro in secs:
        if ro <= o < ro + rs:
            return base + va + o - ro


def va2off(v):
    for nm, vs, va, rs, ro in secs:
        if base + va <= v < base + va + rs:
            return ro + v - base - va


def cstr(v):
    o = va2off(v)
    return b[o:o + 120].split(b'\0')[0].decode(errors='replace') if o is not None else None


classes = {}  # dti -> (name, parent, size, ctor site)
for m in re.finditer(rb'\x68(....)\xB9(....)\xE8(....)', b, re.S):
    o = m.start()
    site = off2va(o)
    if site is None:
        continue
    rel = struct.unpack('<i', m.group(3))[0]
    if site + 10 + 5 + rel != ctor:
        continue
    name = cstr(struct.unpack('<I', m.group(1))[0])
    dti = struct.unpack('<I', m.group(2))[0]
    parent = size = None
    if b[o - 5] == 0x68:
        parent = struct.unpack_from('<I', b, o - 4)[0]
        if b[o - 10] == 0x68:
            size = struct.unpack_from('<I', b, o - 9)[0]
        elif b[o - 7] == 0x6A:
            size = b[o - 6]
    classes[dti] = (name, parent, size, site)

print(f'{len(classes)} DTI constructors found')
kids = {}
for d, (nm, p, sz, s) in classes.items():
    kids.setdefault(p, []).append(d)
byname = {}
for d, c in classes.items():
    byname.setdefault(c[0], []).append(d)


def show(d, depth=0, maxdepth=6):
    nm, p, sz, s = classes[d]
    print(f"{'  ' * depth}{nm}  dti=0x{d:08X} size=0x{(sz or 0):X}")
    if depth < maxdepth:
        for k in sorted(kids.get(d, []), key=lambda x: classes[x][0] or ''):
            show(k, depth + 1, maxdepth)


for r in roots:
    for d in byname.get(r, []):
        chain, cur = [], classes[d][1]
        while cur in classes:
            chain.append(classes[cur][0])
            cur = classes[cur][1]
        print(f'\n## {r}: ancestors {" <- ".join(chain)}')
        show(d)

if csv:
    with open(csv, 'w') as f:
        f.write('dti,name,parent_dti,parent_name,size,ctor_site\n')
        for d, (nm, p, sz, s) in sorted(classes.items()):
            pn = classes[p][0] if p in classes else ''
            f.write(f'0x{d:08X},{nm},0x{(p or 0):08X},{pn},0x{(sz or 0):X},0x{s:08X}\n')
