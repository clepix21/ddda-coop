"""For every scalar the mod's code uses (>= 0x40), how often it stays the same in paired GOG instructions and
what it becomes otherwise. Usage: python -I scalar_stats.py <steam funcs> <gog funcs> <funcs_map.tsv> <src dir>"""
import collections, glob, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
def load(path):
    funcs = {}
    cur = None
    for line in open(path, encoding='utf-8'):
        if line.startswith('F '):
            cur = []
            funcs[int(line.split()[1], 16)] = cur
            continue
        p = line.rstrip('\n').split(' ', 3)
        cur.append((int(p[0], 16), p[2], p[3] if len(p) > 3 else ''))
    return funcs
want = collections.defaultdict(set)
for f in glob.glob(os.path.join(sys.argv[4], '*.cpp')) + glob.glob(os.path.join(sys.argv[4], '*.h')):
    for i, l in enumerate(open(f, encoding='utf-8', errors='replace')):
        for m in re.finditer(r'0x([0-9A-Fa-f]{2,6})\b', l.split('//')[0]):
            v = int(m.group(1), 16)
            if 0x40 <= v < 0x400000:
                want[v].add(f'{os.path.basename(f)}:{i + 1}')
sf, gf = load(sys.argv[1]), load(sys.argv[2])
same = collections.Counter(); chg = collections.defaultdict(collections.Counter); where = collections.defaultdict(list)
for l in open(sys.argv[3]):
    a, b, _ = l.split('\t')
    x, y = sf.get(int(a, 16)), gf.get(int(b, 16))
    if not x or not y or len(x) != len(y):
        continue
    for (ia, ma, oa), (ib, mb, ob) in zip(x, y):
        if ma != mb:
            continue
        sa = re.findall(r's:([0-9a-f]+)', oa); sb = re.findall(r's:([0-9a-f]+)', ob)
        if len(sa) != len(sb):
            continue
        for p, q in zip(sa, sb):
            p, q = int(p, 16), int(q, 16)
            if p in want:
                if p == q: same[p] += 1
                else:
                    chg[p][q] += 1; where[p].append(ia)
import sys as _s
mode = _s.argv[5] if len(_s.argv) > 5 else "chg"
for v in sorted(want):
    if mode == "unseen":
        if not chg[v] and not same[v] and v >= 0x100: print(f"{v:#x} {' '.join(sorted(want[v])[:4])}")
        continue
    if chg[v]:
        tot = same[v] + sum(chg[v].values())
        print(f'{v:#x} same {same[v]} changed {dict((hex(k), n) for k, n in chg[v].most_common(3))} '
              f'at {",".join("%X" % a for a in where[v][:5])} | {" ".join(sorted(want[v])[:5])}')
