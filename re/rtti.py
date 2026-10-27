"""MSVC x86 RTTI and string-xref scan for DDDA.exe.
Usage: python -I rtti.py <exe> <class-substring>... [--str name ...]
Prints, per matching class: TypeDescriptor VA, vtable VA(s) and base classes.
With --str: VAs of the string and every .text location that pushes/moves its address.
"""
import re
import struct
import sys

exe = sys.argv[1]
args = sys.argv[2:]
strs = args[args.index('--str') + 1:] if '--str' in args else []
classes = args[:args.index('--str')] if '--str' in args else args
b = open(exe, 'rb').read()
pe = struct.unpack_from('<I', b, 0x3C)[0]
n = struct.unpack_from('<H', b, pe + 6)[0]
opt = struct.unpack_from('<H', b, pe + 20)[0]
base = struct.unpack_from('<I', b, pe + 24 + 28)[0]
secs = []
for i in range(n):
    nm, vs, va, rs, ro = struct.unpack_from('<8sIIII', b, pe + 24 + opt + 40 * i)
    secs.append((nm.rstrip(b'\0').decode(), va, vs, ro, rs))


def off2va(o):
    for nm, va, vs, ro, rs in secs:
        if ro <= o < ro + rs:
            return base + va + (o - ro)


def va2off(v):
    for nm, va, vs, ro, rs in secs:
        if base + va <= v < base + va + rs:
            return ro + v - base - va


def sec(name):
    s = next(x for x in secs if x[0] == name)
    return s[3], s[3] + s[4]


def dw(v):
    return struct.pack('<I', v)


rd0, rd1 = sec('.rdata')
tx0, tx1 = sec('.text')
dt0, dt1 = sec('.data')

# TypeDescriptor: pVFTable, spare, name ".?AV...@@"
for m in re.finditer(rb'\.\?A[VU]([A-Za-z0-9_@?$<>,]+)@@\0', b):
    name = m.group(1).decode()
    demangled = '::'.join(reversed(name.split('@')))
    if not any(c == demangled or c == name.split('@')[0] for c in classes):
        continue
    td = off2va(m.start() - 8)
    print(f'{demangled}: TypeDescriptor 0x{td:08X}')
    # COL: sig(0), offset, cdOffset, pTypeDescriptor, pClassHierarchy
    for cm in re.finditer(re.escape(dw(td)), b[rd0:rd1]):
        col_off = rd0 + cm.start() - 12
        sig, off, cd = struct.unpack_from('<III', b, col_off)
        if sig != 0:
            continue
        col = off2va(col_off)
        chd = struct.unpack_from('<I', b, col_off + 16)[0]
        # vtable = address right after a pointer to the COL
        for vm in re.finditer(re.escape(dw(col)), b[rd0:rd1]):
            vt = off2va(rd0 + vm.start() + 4)
            first = struct.unpack_from('<I', b, rd0 + vm.start() + 4)[0]
            print(f'   COL 0x{col:08X} (this-offset {off}) vtable 0x{vt:08X} vfunc[0] 0x{first:08X}')
        co = va2off(chd)
        if co is None:
            continue
        _, _, nb, pba = struct.unpack_from('<IIII', b, co)
        bo = va2off(pba)
        chain = []
        for k in range(nb):
            bcd = struct.unpack_from('<I', b, bo + 4 * k)[0]
            ptd = struct.unpack_from('<I', b, va2off(bcd))[0]
            nm = b[va2off(ptd) + 8:va2off(ptd) + 200].split(b'\0')[0].decode()
            chain.append(nm[4:-2])
        print('   bases:', ' -> '.join(chain))
        break

for s in strs:
    for m in re.finditer(re.escape(s.encode()) + b'\0', b):
        if m.start() and b[m.start() - 1] != 0:
            continue
        sva = off2va(m.start())
        refs = [off2va(tx0 + r.start()) for r in re.finditer(re.escape(dw(sva)), b[tx0:tx1])]
        drefs = [off2va(rd0 + r.start()) for r in re.finditer(re.escape(dw(sva)), b[rd0:rd1])]
        drefs += [off2va(dt0 + r.start()) for r in re.finditer(re.escape(dw(sva)), b[dt0:dt1])]
        print(f'"{s}" @0x{sva:08X}: code refs {[hex(x) for x in refs[:12]]} data refs {[hex(x) for x in drefs[:12]]}')
