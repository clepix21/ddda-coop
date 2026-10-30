"""How far behind the other players are drawn, from the "shows another player" lines (one per player and
second): error before correction, the state's age and the player's speed. Moving = over 200 u/s, under
400 u error (a bigger error is a jump: a load, another area).

Usage: python -I check_player_lag.py <log> [<log>...]
"""
import re
import statistics
import sys

LINE = re.compile(r"party (\d) shows another player, error ([\d.]+) u before correction, HP [-\d.]+ "
                  r"\(state (\d+) ms old, (\d+) u/s\)")

for path in sys.argv[1:]:
    rows = {}
    for ln in open(path, encoding='utf-8', errors='replace'):
        m = LINE.search(ln)
        if m:
            rows.setdefault(int(m[1]), []).append((float(m[2]), int(m[3]), int(m[4])))
    for party, r in sorted(rows.items()):
        moving = [x for x in r if x[2] > 200 and x[0] < 400]
        jumps = sum(1 for x in r if x[0] >= 400)
        if not moving:
            print(f"{path}: party {party}: {len(r)} samples, none moving, {jumps} over 400 u")
            continue
        errs = sorted(x[0] for x in moving)
        print(f"{path}: party {party}: {len(moving)} moving samples, error median {statistics.median(errs):.0f} u, "
              f"p90 {errs[int(len(errs) * 0.9)]:.0f} u, state age median {statistics.median(x[1] for x in moving):.0f} ms, "
              f"speed median {statistics.median(x[2] for x in moving):.0f} u/s; {jumps} samples over 400 u")
