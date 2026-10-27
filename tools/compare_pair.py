"""Compare host and guest probe logs on wall-clock time (each log's own t=0 differs).

Usage: python -I compare_pair.py <host.log> <guest.log> [--max-error U] [--min-moved U]
For every guest sample of the Arisen and the main pawn, finds the host sample closest in wall
time and reports the position error. Also reports how far each character moved on the host
(so a test where nothing moved cannot pass) and the lag at which errors are smallest.
Exit 1 if, for either character, the error at the best lag exceeds --max-error (default 5)
in more than 10 % of samples, or the host moved it less than --min-moved (default 100).
"""
import re
import sys

host_path, guest_path = sys.argv[1], sys.argv[2]
args = sys.argv[3:]
max_err = float(args[args.index('--max-error') + 1]) if '--max-error' in args else 5.0
min_moved = float(args[args.index('--min-moved') + 1]) if '--min-moved' in args else 100.0
# --cmc N: the pawn to compare (mCmcId; 0 = the main pawn, 1 and 2 = hired pawns, which guests take first)
cmc = args[args.index('--cmc') + 1] if '--cmc' in args else '0'


def samples(path):
    lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
    first = {}
    for l in lines:
        m = re.search(rf'probe: new (uCmc) (\w+) cmcId {cmc} ', l) or re.search(r'probe: new (uPlayer) (\w+)', l)
        if m and m.group(1) not in first:
            first[m.group(1)] = m.group(2)
    out = {'arisen': [], 'pawn': []}
    who = None
    for l in lines:
        m = re.search(r'probe: (uPlayer|uCmc) (\w+) mode', l)
        if m:
            who = {first.get('uPlayer'): 'arisen', first.get('uCmc'): 'pawn'}.get(m.group(2))
            continue
        m = re.search(r'probe:   \+30 .*\| \+40 (\S+) (\S+) (\S+)', l)
        if m and who:
            hh, mm, ss = l[:12].split(':')
            t = int(hh) * 3600 + int(mm) * 60 + float(ss)  # seconds since midnight (same PC clock)
            out[who].append((t, float(m.group(1)), float(m.group(3))))
            who = None
    return out


H, G = samples(host_path), samples(guest_path)
failed = False
for who in ('arisen', 'pawn'):
    h, g = H[who], G[who]
    if not h or not g:
        print(f'FAIL  {who}: no samples (host {len(h)}, guest {len(g)})')
        failed = True
        continue
    moved = sum(((b[1] - a[1]) ** 2 + (b[2] - a[2]) ** 2) ** 0.5 for a, b in zip(h, h[1:]))
    best = None
    for lag10 in range(0, 21):  # guest may trail the host by 0..2 s
        lag = lag10 / 10
        errs = []
        for t, x, z in g:
            ref = min(h, key=lambda s: abs(s[0] - (t - lag)))
            if abs(ref[0] - (t - lag)) < 0.6:
                errs.append(((x - ref[1]) ** 2 + (z - ref[2]) ** 2) ** 0.5)
        if errs:
            bad = sum(e > max_err for e in errs) / len(errs)
            if best is None or bad < best[1]:
                best = (lag, bad, max(errs), sorted(errs)[len(errs) // 2], len(errs))
    lag, bad, worst, median, n = best
    ok = bad <= 0.10 and moved >= min_moved
    failed |= not ok
    print(f'{"PASS" if ok else "FAIL"}  {who}: host moved {moved:.0f} u; at guest lag {lag:.1f} s, '
          f'{n} pairs, median error {median:.2f} u, worst {worst:.1f} u, {bad * 100:.0f} % over {max_err} u')
sys.exit(1 if failed else 0)
