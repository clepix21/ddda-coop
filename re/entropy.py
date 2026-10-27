import math, struct, sys
b = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', b, 0x3C)[0]; n = struct.unpack_from('<H', b, pe+6)[0]; opt = struct.unpack_from('<H', b, pe+20)[0]
for i in range(n):
    name, vs, va, rs, ro = struct.unpack_from('<8sIIII', b, pe+24+opt+40*i)
    d = b[ro:ro+rs]
    c = [0]*256
    for x in d: c[x] += 1
    e = -sum(k/len(d)*math.log2(k/len(d)) for k in c if k)
    print(f'{name.rstrip(b"\0").decode():8} entropy {e:.3f}  push-ebp/mov-ebp-esp: {d.count(bytes([0x55,0x8B,0xEC]))}  int3 pad: {d.count(bytes([0xCC]*4))}')
