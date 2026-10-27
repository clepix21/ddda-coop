r"""Checks a free roam run (suite_roam.ps1): the guest leaves the host's party (LB + RB + Y), its own
character becomes its Arisen and it walks in its copy of the world while the host goes to another
area; then it comes back and must be in the host's area, following the host. Prints PASS/FAIL lines.
Usage: check_roam.py <dir>"""
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
made = [ln for ln in guest if "roam: roam save made:" in ln]
check(bool(made), "guest's roam save: its own character is the Arisen" + (f" ({made[0][13:]})" if made else ""))
alone = [t(ln) for ln in guest if "roam: exploring alone" in ln]
back = [t(ln) for ln in guest if "roam: back with the host" in ln]
check(bool(alone), "guest's world reloaded around its own character")
check(any("explores on its own" in ln for ln in host), "host: the guest's pawn went back to its AI")
host_jump = [t(ln) for ln in host if "net: stage: host now in 600" in ln]
check(bool(alone) and bool(host_jump) and host_jump[0] > alone[0], "host went to the underground while the guest explored")
pos = [(t(ln), float(m[1]), float(m[2]), int(m[3])) for ln in guest for m in [re.search(r"roam: exploring at \((-?\d+), (-?\d+)\), stage (-?\d+)", ln)] if m]
moved = max(((a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5 for a in pos for b in pos) if pos else 0
check(moved > 200, f"guest walked on its own while exploring ({moved:.0f} u)")
stay = [p for p in pos if host_jump and p[0] > host_jump[0] + 5]
check(bool(stay) and all(p[3] != 600 for p in stay), "guest stayed in its own area while the host changed area")
jumped_while = [ln for ln in guest if "jumps to" in ln and alone and back and alone[0] < t(ln) < back[0]]
check(not jumped_while, "no area following while exploring")
check(bool(back), "guest came back")
check(any("is back: it drives its pawn again" in ln for ln in host), "host: the guest drives its pawn again")
after = [ln for ln in guest if back and t(ln) > back[0] and "net: stage: guest now in 600" in ln]
check(bool(after), "after coming back the guest is in the host's area (600)" + (f" {t(after[0]) - back[0]:.1f} s later" if after else ""))
errs = sorted(float(m[1]) for ln in guest if back and t(ln) > back[0] + 25 for m in [re.search(r"guest arisen error ([\d.]+) u", ln)] if m)
med = errs[len(errs) // 2] if errs else None
check(med is not None and med < 5, f"then its Arisen follows the host's (median error {med} u, {len(errs)} samples)")
