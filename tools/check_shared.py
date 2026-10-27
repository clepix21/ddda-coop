r"""Checks a shared world run (suite_shared.ps1, shared_world=1): each player plays its own character
in its own copy of the host's world and sees the other where that one really is while in the same
area; when the host changes area the guest stays. Prints PASS/FAIL lines. Usage: check_shared.py <dir>"""
import re
import sys

out = sys.argv[1]


def lines(name):
    with open(f"{out}/{name}", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


_t_first = None


def t(ln):
    global _t_first
    h, m, s = ln[:12].split(":")
    v = int(h) * 3600 + int(m) * 60 + float(s)
    if _t_first is None:
        _t_first = v
    d = v - _t_first
    return d + 86400 if d < -43200 else d - 86400 if d > 43200 else d


host, guest = lines("host.log"), lines("guest.log")
t(host[0])
built = [ln for ln in guest if "roam: shared world:" in ln and "Arisen" in ln]
check(bool(built) and "the host's Arisen on its pawn" in built[-1], "guest's world: its own character is the Arisen" + (f" ({built[-1][13:]})" if built else ""))
jump = [t(ln) for ln in host if "net: stage: host now in 600" in ln]
tj = jump[0] if jump else 1e9


def errs(log, until):
    return sorted(float(m[1]) for ln in log if t(ln) < until for m in [re.search(r"shared: party \d shows another player, error ([\d.]+) u", ln)] if m)


for name, log in (("host", host), ("guest", guest)):
    e = errs(log, tj)
    med = e[len(e) // 2] if e else None
    check(med is not None and med < 30 and len(e) > 20, f"{name} shows the other player where it is (median error {med} u, {len(e)} samples)")
me = [(t(ln), float(m[1]), float(m[2]), int(m[3])) for ln in guest for m in [re.search(r"shared: me at \((-?\d+), (-?\d+)\), stage (-?\d+)", ln)] if m]
moved = max(((a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5 for a in me for b in me) if me else 0
check(moved > 200, f"guest walked with its own character ({moved:.0f} u)")
check(bool(jump), "host went to the underground")
loads = [ln for ln in guest if "stage: load" in ln and t(ln) > tj]
stay = [p for p in me if p[0] > tj + 5]
check(not loads and bool(stay) and all(p[3] == 220 for p in stay), "guest stayed in Gran Soren (no area following)")
