"""Map the mod's Steam-exe addresses to the GOG exe by unique byte patterns.
Usage: python -I match.py <steam exe> <gog exe> <src dir> <out tsv>"""
import re, struct, sys, os, glob

def load(p):
    b = open(p, 'rb').read(); e = struct.unpack_from('<I', b, 0x3c)[0]
    n = struct.unpack_from('<H', b, e+6)[0]; opt = e+24
    so = opt + struct.unpack_from('<H', b, e+20)[0]; secs = []
    for i in range(n):
        nm, vs, va, rs, ro = struct.unpack_from('<8sIIII', b, so+40*i)
        secs.append((nm.rstrip(b'\0').decode(), 0x400000+va, vs, ro, rs))
    return b, secs

def sec_of(secs, va):
    for s in secs:
        if s[1] <= va < s[1]+s[2]: return s
def off(secs, va):
    s = sec_of(secs, va)
    if s and va - s[1] < s[4]: return s[3] + va - s[1]

def masked(b, o, n):
    """bytes with 4-byte absolute addresses (0x00400000..0x01A10000) as wildcards"""
    pat = list(b[o:o+n]); i = 0
    while i + 4 <= n:
        v = struct.unpack_from('<I', b, o+i)[0]
        if 0x00400000 <= v < 0x01A10000:
            for k in range(4): pat[i+k] = None
            i += 4
        elif b[o+i] in (0xE8, 0xE9) and i + 5 <= n:
            for k in range(1, 5): pat[i+k] = None
            i += 5
        elif b[o+i] == 0x0F and 0x80 <= b[o+i+1] <= 0x8F and i + 6 <= n:
            for k in range(2, 6): pat[i+k] = None
            i += 6
        else: i += 1
    return pat

def to_re(pat):
    return re.compile(b''.join(b'.' if c is None else re.escape(bytes([c])) for c in pat), re.S)

def find_code(sb, ss, gb, gs, va, gtext):
    o = off(ss, va)
    for n in (16, 24, 32, 48, 64, 96):
        pat = masked(sb, o, n)
        if sum(c is not None for c in pat) < 8: continue
        hits = [m.start() for m in to_re(pat).finditer(gb, gtext[3], gtext[3]+gtext[4])]
        if len(hits) == 1: return gtext[1] + hits[0] - gtext[3], n
        if not hits: return None, n
    return 'many', n

def main():
    sb, ss = load(sys.argv[1]); gb, gs = load(sys.argv[2])
    gtext = [s for s in gs if s[0] == '.text'][0]
    addrs = {}
    for f in glob.glob(os.path.join(sys.argv[3], '*.cpp')) + glob.glob(os.path.join(sys.argv[3], '*.h')):
        for m in re.finditer(r'0x0*([0-9A-Fa-f]{6,8})\b', open(f, encoding='utf-8', errors='replace').read()):
            v = int(m.group(1), 16)
            if 0x00401000 <= v < 0x01A0C000: addrs.setdefault(v, set()).add(os.path.basename(f))
    rows = []; stat = {}
    for va in sorted(addrs):
        s = sec_of(ss, va); sn = s[0] if s else '?'
        if sn == '.text':
            r, n = find_code(sb, ss, gb, gs, va, gtext)
            k = 'found' if isinstance(r, int) else ('several' if r == 'many' else 'missing')
            rows.append((va, sn, k, r if isinstance(r, int) else 0, ','.join(sorted(addrs[va]))))
        else:
            k = 'data'; rows.append((va, sn, k, 0, ','.join(sorted(addrs[va]))))
        stat[(sn, k)] = stat.get((sn, k), 0) + 1
    with open(sys.argv[4], 'w') as f:
        for va, sn, k, g, fl in rows: f.write(f'{va:08X}\t{sn}\t{k}\t{g:08X}\t{fl}\n')
    for k in sorted(stat): print(k, stat[k])
main()
