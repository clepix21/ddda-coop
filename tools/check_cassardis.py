"""Checks a stage 100 pair run (suite_cassardis.ps1, issue #4): the guest pairs its enemies with the
host's, without far "kind, closest" snaps, keeps its pairs, and its enemies stand where the host's do.
Prints PASS/FAIL lines, and info lines for the numbers behind them.
Usage: check_cassardis.py <run dir>  (host.log, guest.log, host_enemies.txt, guest_enemies.txt)"""
import math
import re
import sys

out = sys.argv[1]


def lines(name):
    try:
        with open(f"{out}/{name}", encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def dump(name):
    """slot -> (class, position, hp, follows, dead)"""
    rows = {}
    for ln in lines(name):
        m = re.match(r"(\d+) (\S+) \S+ (\S+) (\S+) (\S+) size \S+ hp@\S+ (\S+) .* follows (-?\d+) dead (-?\d+)$", ln)
        if m:
            rows[int(m[1])] = (m[2], tuple(float(m[i]) for i in (3, 4, 5)), float(m[6]), int(m[7]), int(m[8]))
    return rows


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


guest = lines("guest.log")
pairs = [re.search(r"enemy #(\d+) (\S+) paired with host #(\d+) \((.+?) ([\d.]+) u apart\)", ln) for ln in guest]
pairs = [(int(m[1]), m[2], int(m[3]), m[4], float(m[5])) for m in pairs if m]
check(len(pairs) >= 3, f"guest paired its enemies with the host's ({len(pairs)} pairs)")
far = [p for p in pairs if p[3] == "kind, closest" and p[4] > 400]
print(f"info  pairs by kind: {sum(p[3] == 'spawns' for p in pairs)} spawns, {sum(p[3] == 'positions' for p in pairs)} positions, "
      f"{sum(p[3] == 'kind, closest' for p in pairs)} kind-closest (over 400 u: {', '.join(f'{p[4]:.0f}' for p in far) or 'none'})")
check(len(far) <= 1, f"no far snaps: pairs made over 400 u apart ({len(far)})")
again = sum("is another enemy now" in ln for ln in guest)
# pairs per local enemy: the same enemy paired again and again means its pair kept breaking
per = {}
for ln in guest:
    m = re.search(r"enemy: new #(\d+) ", ln)
    if m:
        per[int(m[1])] = per.get(int(m[1]), 0) * 0  # a new enemy in that slot
    m = re.search(r"enemy #(\d+) \S+ paired with host", ln)
    if m:
        per[int(m[1])] = per.get(int(m[1]), 0) + 1
churn = max(per.values() or [0])
print(f"info  host slot reused under a pair: {again}; most pairs for one local enemy: {churn}")
check(churn <= 3, f"pairs hold: no enemy paired more than 3 times ({churn})")
stats = [re.search(r"enemies: (\d+) from the host, (\d+) local updates applied, (\d+) new pairs, (\d+) unpaired updates, worst error ([\d.]+)", ln) for ln in guest]
stats = [(int(m[1]), int(m[2]), int(m[3]), int(m[4]), float(m[5])) for m in stats if m]
active = [s for s in stats if s[1] > 0]
print(f"info  worst error per 5 s window: {', '.join(f'{s[4]:.0f}' for s in active)}")
later = [s[4] for s in active[1:]]
# goblins in a fight move ~60 u between two host pages: a window may reach 200 u; the issue's errors were
# 1000-12000 u (far pairs, places 10000 u off across a cell edge)
over = [x for x in later if x >= 500]
print(f"info  windows over 150 u after the first: {sum(x >= 150 for x in later)} of {len(later)}")
check(len(active) >= 3 and not over, f"guest enemies follow the host, never 500 u off (windows over 500 u: {len(over)} of {len(later)})")
# both games crossed a cell edge of the open world (x 40000), each at its own moment: the case of issue #4
crossed = {}
for name in ("host.log", "guest.log"):
    for ln in lines(name):
        m = re.match(r"(\d+):(\d+):([\d.]+) net: field cell origin \(-40000, -130000\) -> \(-30000, -130000\)", ln)
        if m:
            crossed[name] = int(m[1]) * 3600 + int(m[2]) * 60 + float(m[3])
            break
check(len(crossed) == 2, "both games crossed a cell edge (" + ", ".join(f"{k.split('.')[0]} at {v % 3600:.1f} s" for k, v in crossed.items()) + ")")
# the other player's pawn while the two games stood in different cells, and after
late = []
for name in ("guest.log", "host.log"):  # each log's first samples: the pawn put on its player at the connection
    errs = [float(m[1]) for ln in lines(name) if (m := re.search(r"shows another player, error ([\d.]+) u before correction", ln))]
    late += errs[10:]
check(bool(late) and max(late) < 300, f"the players' pawns stay on their players (worst {max(late or [0]):.0f} u over {len(late)} samples)")
h, g = dump("host_enemies.txt"), dump("guest_enemies.txt")
# the end dumps are taken seconds apart on each side (each script runs from its own start): the places of
# enemies in a fight differ by then, so reported only
far_end = []
for gi, (gc, gp, ghp, k, gdead) in sorted(g.items()):
    if k >= 0 and k in h:
        d = math.dist(gp, h[k][1])
        far_end.append(f"#{gi}->{k} {d:.0f} u")
print(f"info  end dumps (seconds apart): host {len(h)}, guest {len(g)}; pairs {', '.join(far_end) or 'none'}")
