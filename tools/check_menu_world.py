r"""Checks a menu world run (suite_menu_world.ps1): the host's pause menu did not stop its world (Nexus 2026-11-08:
"It would be nice if the inventory didn't pause the game for whoever opens it"): the pawn showing the guest kept
moving in the host's world while the menu was open, the host's Arisen did not walk with the stick meanwhile, and
the menu shut. Usage: check_menu_world.py <dir>"""
import re
import sys

out = sys.argv[1]


def secs(ts):
    h, m, s = ts.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


with open(f"{out}/host.log", encoding="utf-8", errors="replace") as f:
    host = f.read().splitlines()
opened = [secs(ln[:12]) for ln in host if "pause menu in co-op: the world's line 0x39 kept on" in ln]
shut = [secs(ln[:12]) for ln in host if "scene: pause menu shut" in ln]
check(bool(opened), f"host's pause menu opened with the world kept on ({len(opened)} lines)")
check(bool(shut) and opened and shut[0] > opened[0], f"host's pause menu shut ({len(shut)})")
if opened and shut:
    t0, t1 = opened[0], shut[0]
    me, puppet = [], []
    for ln in host:
        m = re.match(r"(\S+) heights: stage \S+ \| A0\^? \((-?\d+), (-?\d+), (-?\d+)\) (\S+)", ln)
        if not m or not t0 + 0.5 <= secs(m[1]) <= t1:
            continue
        me.append((int(m[2]), int(m[4]), m[5]))
        p = re.search(r"\| p\d\* \((-?\d+), (-?\d+), (-?\d+)\) (\S+)", ln)
        if p:
            puppet.append((int(p[1]), int(p[3])))

    def span(pts):
        return max(((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5 for a in pts for b in pts) if pts else 0

    check(span(puppet) > 150, f"the guest's pawn moved in the host's world during the menu ({span(puppet):.0f} u, {len(puppet)} samples)")
    walks = sorted({a for x, z, a in me if a != "cPlActWait"})
    check(me and span([(x, z) for x, z, a in me]) < 30, f"the host's Arisen stayed put during the menu ({span([(x, z) for x, z, a in me]):.0f} u; actions {walks})")
