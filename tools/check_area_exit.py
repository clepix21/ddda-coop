r"""Checks an area exit run (suite_area_exit.ps1): both worlds reach the underground (stage 600), and once
both are there no character stands far under or above its world's Arisen (issue #1: a pawn falling
through the ground and flying into the sky after leaving an area). Reads the "heights:" lines
(test_log_heights=1). Prints PASS/FAIL lines. Usage: check_area_exit.py <dir>"""
import re
import sys

out = sys.argv[1]
STAGE = 600
SETTLE = 8.0     # s after both worlds are in the new area
FAR_Y = 600.0    # u under or above the Arisen
NEAR_XZ = 3000.0 # only characters this close on the ground plane (the underground has floors)

ENTRY = re.compile(r"\| (A|p)(\d)(\*?)\^? \((-?[\d.]+|-?nan\S*|-?inf), (-?[\d.]+|-?nan\S*|-?inf), (-?[\d.]+|-?nan\S*|-?inf)\) (\S+)")


def secs(ts):
    h, m, s = ts.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def samples(name):
    rows = []
    with open(f"{out}/{name}", encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = re.match(r"(\d\d:\d\d:\d\d\.\d+) heights: stage (-?\d+)(.*)", ln)
            if not m:
                continue
            chars = {}
            for e in ENTRY.finditer(m[3]):
                try:
                    pos = tuple(float(v) for v in e.group(4, 5, 6))
                except ValueError:
                    pos = (float("nan"),) * 3
                chars[e[1] + e[2]] = (e[3] == "*", pos, e[7])
            rows.append((secs(m[1]), int(m[2]), chars))
    return rows


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


worlds = {"host": samples("host.log"), "guest": samples("guest.log")}
for w, rows in worlds.items():
    check(bool(rows), f"{w}: heights logged ({len(rows)} samples)")
arrive = {}
for w, rows in worlds.items():
    t = next((r[0] for r in rows if r[1] == STAGE), None)
    arrive[w] = t
    check(t is not None, f"{w}: reached stage {STAGE}" + (f" at {t:.0f} s of the day" if t else ""))
def parked(chars, k):
    """a pawn kept out of sight on purpose (an unused pawn, a player elsewhere): 3000 u under the Arisen"""
    if k not in chars or "A0" not in chars:
        return False
    return -3300 < chars[k][1][1] - chars["A0"][1][1] < -2700


if all(arrive.values()):
    start = max(arrive.values()) + SETTLE
    for w, rows in worlds.items():
        # pawns already out of sight before anyone left (the unused hired pawn of a two-player game)
        before = [c for t, stage, c in rows if t < min(arrive.values()) - 15 and stage != STAGE]
        unused = {k for k in (before[-1] if before else {}) if k != "A0" and parked(before[-1], k)}
        bad, nan, n = [], 0, 0
        for t, stage, chars in rows:
            if t < start or stage != STAGE or "A0" not in chars:
                continue
            ax, ay, az = chars["A0"][1]
            for k, (puppet, (x, y, z), act) in chars.items():
                if k == "A0" or (k in unused and parked(chars, k)):
                    continue
                n += 1
                if y != y:
                    nan += 1
                    continue
                if ((x - ax) ** 2 + (z - az) ** 2) ** 0.5 < NEAR_XZ and abs(y - ay) > FAR_Y:
                    bad.append((round(t - start, 1), k + ("*" if puppet else ""), round(y - ay), act))
        check(n > 0 and not bad and not nan,
              f"{w}: after both arrived, every pawn within {FAR_Y:.0f} u of the Arisen's height "
              f"({n} pawn samples, {len(bad)} off, {nan} NaN; kept out of sight as unused: {sorted(unused)})" + (f"; first {bad[:4]}" if bad else ""))
    # while the two worlds were in different areas (informative: a puppet waits out of sight there)
    lo, hi = min(arrive.values()), max(arrive.values())
    print(f"INFO  worlds apart for {hi - lo:.1f} s")
