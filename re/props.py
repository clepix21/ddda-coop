"""List MtDTI property registrations in the function(s) containing given addresses.
Pattern (MSVC, x86): mov [esp+0x14], <name ptr>; mov [esp+0x18], <type>; ... lea r, [edi+<off>]
Usage: python -I props.py <exe> <objdump.exe> <addr> [...]
Prints: function start, parent-call target, then "offset type name" per property.
"""
import re
import struct
import subprocess
import sys

exe, objdump = sys.argv[1], sys.argv[2]
b = open(exe, 'rb').read()
pe = struct.unpack_from('<I', b, 0x3C)[0]
n = struct.unpack_from('<H', b, pe + 6)[0]
opt = struct.unpack_from('<H', b, pe + 20)[0]
base = struct.unpack_from('<I', b, pe + 24 + 28)[0]
secs = [struct.unpack_from('<8sIIII', b, pe + 24 + opt + 40 * i) for i in range(n)]


def va2off(v):
    for nm, vs, va, rs, ro in secs:
        if base + va <= v < base + va + rs:
            return ro + v - base - va


def cstr(v):
    o = va2off(v)
    return b[o:o + 80].split(b'\0')[0].decode(errors='replace') if o is not None else '?'


def func_start(a):
    # walk back to the int3 padding that precedes the function
    o = va2off(a)
    while o > 0 and not (b[o - 1] == 0xCC and b[o] != 0xCC):
        o -= 1
    return a - (va2off(a) - o)


for arg in sys.argv[3:]:
    a = int(arg, 16)
    start = func_start(a)
    # find the end: first "ret" followed by int3 padding
    o = va2off(start)
    end_o = b.find(b'\xCC\xCC', o)
    end = start + (end_o - o) + 1
    dis = subprocess.run([objdump, '-d', '--no-show-raw-insn', '-M', 'intel', f'--start-address=0x{start:x}',
                          f'--stop-address=0x{end:x}', exe], capture_output=True, text=True).stdout
    lines = [l.strip() for l in dis.splitlines() if re.match(r'\s*[0-9a-f]+:', l)]
    calls = [re.search(r'call\s+0x([0-9a-f]+)', l) for l in lines]
    calls = [int(c.group(1), 16) for c in calls if c]
    print(f'== function 0x{start:08X}-0x{end:08X}, first call 0x{calls[0]:08X}' if calls else f'== 0x{start:08X}')
    pending = {}
    regoff = {}
    for l in lines:
        m = re.search(r'lea\s+(\w+), \[edi \+ 0x([0-9a-f]+)\]', l)
        if m:
            regoff[m.group(1)] = int(m.group(2), 16)
        m = re.search(r'mov\s+dword ptr \[esp \+ 0x14\], 0x([0-9a-f]+)', l)
        if m:
            pending = {'name': cstr(int(m.group(1), 16))}
        m = re.search(r'mov\s+dword ptr \[esp \+ 0x18\], 0x([0-9a-f]+)', l)
        if m and pending:
            pending['type'] = int(m.group(1), 16)
        m = re.search(r'mov\s+dword ptr \[esp \+ 0x20\], (\w+)', l)
        if m and pending and m.group(1) in regoff:
            pending['off'] = regoff[m.group(1)]
        m = re.search(r'mov\s+dword ptr \[esp \+ 0x1c\], (\w+)', l)
        if m and pending and m.group(1) in regoff and 'off' not in pending:
            pending['off'] = regoff[m.group(1)]  # some sites store the field at +0x1c first
        if 'call' in l and pending:
            print(f"   +0x{pending.get('off', -1):04X}  type 0x{pending.get('type', -1):02X}  {pending['name']}")
            pending = {}
