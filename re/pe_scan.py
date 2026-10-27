import struct, sys, re
p = sys.argv[1]
b = open(p, 'rb').read()
pe = struct.unpack_from('<I', b, 0x3C)[0]
nsec = struct.unpack_from('<H', b, pe+6)[0]
opt = struct.unpack_from('<H', b, pe+20)[0]
ep = struct.unpack_from('<I', b, pe+24+16)[0]
base = struct.unpack_from('<I', b, pe+24+28)[0]
print(f'ImageBase 0x{base:X} EntryRVA 0x{ep:X}')
o = pe+24+opt
for i in range(nsec):
    n, vs, va, rs, ro = struct.unpack_from('<8sIIII', b, o+40*i)
    print(f'  {n.rstrip(b"\0").decode(errors="replace"):8} VA 0x{va:08X} VS 0x{vs:08X} raw 0x{rs:08X}')
pat = re.compile(rb'[\x20-\x7e]{4,}')
words = sys.argv[2:]
for w in words:
    hits = sorted({m.group().decode() for m in pat.finditer(b) if w.encode() in m.group()})
    print(f'--- "{w}": {len(hits)} strings'); [print('   ', h) for h in hits[:40]]
