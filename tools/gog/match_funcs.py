"""Pairs the functions of two builds of DDDA.exe from Ghidra exports (ExportFuncs.java), then
instruction by instruction, to map every Steam address to the GOG one and to list the scalars
(field offsets, constants) that differ.
Usage: python -I match_funcs.py <steam funcs.txt> <gog funcs.txt> <out dir>
Writes: insn_map.tsv (steam insn addr -> gog insn addr, every instruction of a paired function),
        ref_map.tsv (steam address operand -> gog address operand, with votes: globals, vtables...),
        scalar_diff.tsv (steam scalar, gog scalar, count, sample steam insn addrs),
        funcs_map.tsv (steam entry, gog entry, how it was paired)."""
import collections
import os
import sys

LO, HI = 0x00400000, 0x01A20000


def load(path):
    funcs = []
    cur = None
    with open(path, encoding='utf-8') as f:
        for line in f:
            if line.startswith('F '):
                _, e, n = line.split()
                cur = [int(e, 16), []]
                funcs.append(cur)
                continue
            parts = line.rstrip('\n').split(' ', 3)
            if len(parts) < 3 or cur is None:
                continue
            addr, ln, mn = int(parts[0], 16), int(parts[1]), parts[2]
            ops = []
            if len(parts) == 4 and parts[3]:
                for op in parts[3].split('|'):
                    ops.append(tuple(o for o in op.split(',') if o))
            cur[1].append((addr, ln, mn, tuple(ops)))
    return funcs


def shape(ins):
    """mnemonics and registers only: what stays when code moves and offsets change"""
    out = []
    for _, ln, mn, ops in ins:
        o = []
        for op in ops:
            o.append(tuple(x if x.startswith('r:') else x[0] for x in op))
        out.append((mn, tuple(o)))
    return hash(tuple(out))


def exact(ins):
    """shape plus small scalars: tells apart functions of the same shape"""
    out = []
    for _, ln, mn, ops in ins:
        o = []
        for op in ops:
            for x in op:
                if x.startswith('s:'):
                    v = int(x[2:], 16)
                    o.append(x if not (LO <= v < HI) else 's')
                elif x.startswith('a:'):
                    o.append('a')
                else:
                    o.append(x)
        out.append((mn, tuple(o)))
    return hash(tuple(out))


def main():
    sf, gf = load(sys.argv[1]), load(sys.argv[2])
    out = sys.argv[3]
    sf = [f for f in sf if f[1]]
    gf = [f for f in gf if f[1]]
    sf.sort()
    gf.sort()
    print('functions', len(sf), len(gf))
    pairs = {}  # steam index -> gog index
    for keyf, label in ((exact, 'exact'), (shape, 'shape')):
        sk = collections.defaultdict(list)
        gk = collections.defaultdict(list)
        for i, f in enumerate(sf):
            if i not in pairs:
                sk[keyf(f[1])].append(i)
        used = set(pairs.values())
        for j, f in enumerate(gf):
            if j not in used:
                gk[keyf(f[1])].append(j)
        for k, si in sk.items():
            gi = gk.get(k)
            if gi and len(si) == len(gi) == 1:
                pairs[si[0]] = (gi[0], label)
        print(label, 'paired', len(pairs))
    # same-shape groups: pair in address order between anchors (the builds keep the order)
    anchors = sorted((i, j) for i, (j, _) in pairs.items())
    sk = collections.defaultdict(list)
    for i, f in enumerate(sf):
        if i not in pairs:
            sk[shape(f[1])].append(i)
    used = set(j for j, _ in pairs.values())
    gk = collections.defaultdict(list)
    for j, f in enumerate(gf):
        if j not in used:
            gk[shape(f[1])].append(j)
    import bisect
    ai = [a for a, _ in anchors]
    for k, si in sk.items():
        gi = gk.get(k)
        if not gi:
            continue
        # group by the anchor interval each lies in
        def interval_s(i):
            p = bisect.bisect_left(ai, i)
            return p
        byint = collections.defaultdict(list)
        for i in si:
            byint[interval_s(i)].append(i)
        gj = sorted(gi)
        for p, lst in byint.items():
            lo_g = anchors[p - 1][1] if p > 0 else -1
            hi_g = anchors[p][1] if p < len(anchors) else 10 ** 9
            cand = [j for j in gj if lo_g < j < hi_g and j not in used]
            if len(cand) == len(lst):
                for i, j in zip(sorted(lst), cand):
                    pairs[i] = (j, 'order')
                    used.add(j)
    print('order paired', len(pairs))
    insn = {}
    refs = collections.defaultdict(collections.Counter)
    sdiff = collections.defaultdict(list)
    with open(os.path.join(out, 'funcs_map.tsv'), 'w') as fm:
        for i, (j, how) in sorted(pairs.items()):
            fm.write(f'{sf[i][0]:08X}\t{gf[j][0]:08X}\t{how}\n')
            a, b = sf[i][1], gf[j][1]
            if len(a) != len(b):
                continue
            for x, y in zip(a, b):
                insn[x[0]] = y[0]
                for oa, ob in zip(x[3], y[3]):
                    for va, vb in zip(oa, ob):
                        if va[0] != vb[0]:
                            continue
                        if va.startswith('a:') or va.startswith('s:'):
                            p, q = int(va[2:], 16), int(vb[2:], 16)
                            if LO <= p < HI and LO <= q < HI:
                                refs[p][q] += 1
                            elif va.startswith('s:') and p != q:
                                sdiff[(p, q)].append(x[0])
    with open(os.path.join(out, 'insn_map.tsv'), 'w') as f:
        for k in sorted(insn):
            f.write(f'{k:08X}\t{insn[k]:08X}\n')
    with open(os.path.join(out, 'ref_map.tsv'), 'w') as f:
        for k in sorted(refs):
            (v, n), = refs[k].most_common(1)
            tot = sum(refs[k].values())
            f.write(f'{k:08X}\t{v:08X}\t{n}\t{tot}\n')
    with open(os.path.join(out, 'scalar_diff.tsv'), 'w') as f:
        for (p, q), l in sorted(sdiff.items(), key=lambda t: -len(t[1])):
            f.write(f'{p:X}\t{q:X}\t{len(l)}\t' + ','.join(f'{a:08X}' for a in l[:12]) + '\n')
    print('instructions mapped', len(insn), 'refs', len(refs), 'scalar diffs', len(sdiff))


main()
