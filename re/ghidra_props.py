"""List MtDTI properties (name, offset, type) from Ghidra decompiler output of createProperty.
Usage: python -I ghidra_props.py <decomp.txt> [exe]   (output of DecompRefs.java)
Handles both decompiler shapes seen in DDDA.exe:
  local_30 = "name"; ... (param_1 + 0xOFF)
  *puVar1 = 0xTTTTTTTTNNNNNNNN; puVar1[1] = CONCAT44(param_1 + 0xOFF, ...)   (needs the exe)
"""
import re
import struct
import sys

exe = open(sys.argv[2], 'rb').read() if len(sys.argv) > 2 else None


def cstr(va):
    pe = struct.unpack_from('<I', exe, 0x3C)[0]
    n = struct.unpack_from('<H', exe, pe + 6)[0]
    opt = struct.unpack_from('<H', exe, pe + 20)[0]
    for i in range(n):
        nm, vs, rva, rs, ro = struct.unpack_from('<8sIIII', exe, pe + 24 + opt + 40 * i)
        if rva <= va - 0x400000 < rva + rs:
            o = ro + va - 0x400000 - rva
            return exe[o:o + 64].split(b'\x00')[0].decode(errors='replace')
    return '?'


txt = open(sys.argv[1], encoding='utf-8', errors='replace').read()
for block in txt.split('\n==== ')[1:]:
    lines = block.splitlines()
    print('##', lines[0][:70])
    last = None
    for i, l in enumerate(lines):
        mo = re.search(r'\(param_1 \+ (0x[0-9a-f]+)\)', l)
        if mo:
            last = mo.group(1)
        m = re.search(r'local_30 = "([^"]*)";', l)
        if m:
            off = last
            for l2 in lines[i + 1:i + 4]:
                mo2 = re.search(r'\(param_1 \+ (0x[0-9a-f]+)\)', l2)
                if mo2:
                    off = mo2.group(1)
            typ = next((re.search(r'uStack_2c = (\S+);', x).group(1) for x in lines[i + 1:i + 3] if 'uStack_2c' in x), None)
            print(f'   {m.group(1):26} +{off} type {typ}')
            last = None
        m = re.search(r'\*puVar1 = 0x([0-9a-f]+);', l)
        if m and exe:
            v = int(m.group(1), 16)
            name, typ = cstr(v & 0xFFFFFFFF), v >> 32
            off = None
            for l2 in lines[i + 1:i + 3]:
                mo2 = re.search(r'CONCAT44\(param_1 \+ (0x[0-9a-f]+),', l2)
                if mo2:
                    off = mo2.group(1)
            print(f'   {name:26} +{off} type 0x{typ:x}')
