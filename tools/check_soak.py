"""Checks a soak run (suite_soak.ps1): three copies, a fight in the underground, guest 1 killed and
restarted. Prints PASS/FAIL lines. Usage: check_soak.py <run dir>"""
import os
import re
import sys

out = sys.argv[1]


def lines(name):
    path = f"{out}/{name}"
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


_t0 = None


def t(ln):
    global _t0
    h, m, s = ln[:12].split(":")
    v = int(h) * 3600 + int(m) * 60 + float(s)
    if _t0 is None:
        _t0 = v
    d = v - _t0
    return d + 86400 if d < -43200 else d - 86400 if d > 43200 else d


host, g1a, g1, g2 = lines("host.log"), lines("guest_first.log"), lines("guest.log"), lines("guest2.log")
t(host[0])
logs = {"host": host, "guest 1 (first run)": g1a, "guest 1": g1, "guest 2": g2}
hangs = [f"{n}: {ln[13:80]}" for n, log in logs.items() for ln in log if "hang:" in ln]
check(not hangs, "no stall of 30 s or more on any copy" + (f": {hangs[:2]}" if hangs else ""))
conn = [ln for ln in host if "net: guest connected" in ln]
check(len(conn) >= 3, f"host saw guests connect 3 times (guest 1, guest 2, guest 1 again): {len(conn)}")
silent = [ln for ln in host if re.search(r"net: (peer|guest \d+ \(.*\)) silent", ln)]
check(bool(silent), "host dropped the killed guest" + (f": {silent[0][13:]}" if silent else ""))
again = [ln for ln in host if re.search(r"net: guest \d's character arrived", ln)]
check(len(again) >= 3, f"guest characters arrived 3 times (the restarted guest's again): {len(again)}")
reloads = [ln for ln in host if "rejoin: idle -> saving" in ln]
check(not reloads, "no world reload for a guest coming back with the same character" + (f": {reloads[0][13:]}" if reloads else ""))
fight = [int(m[1]) for ln in host if (m := re.search(r"net: enemies: (\d+) live on the host", ln))]
check(bool(fight) and max(fight) > 0, f"the host's party met enemies (most live at once: {max(fight) if fight else None})")


def errors(log, after):
    return [(t(ln), float(m[1]), float(m[2]), float(m[3])) for ln in log
            if (m := re.search(r"net: guest arisen error ([\d.]+) u before correction, HP (-?[\d.]+) \(host (-?[\d.]+)\)", ln)) and t(ln) > after]


end = t(host[-1])
for name, log in (("guest 2 (never interrupted)", g2), ("guest 1 (after its restart)", g1)):
    late = errors(log, end - 60)
    med = sorted(e[1] for e in late)[len(late) // 2] if late else None
    check(med is not None and med < 50, f"{name}: its Arisen follows the host's in the last minute (median error {med if med is None else round(med, 1)} u, {len(late)} samples)")
    check(bool(late) and all(abs(e[2] - e[3]) <= 1 for e in late), f"{name}: its Arisen has the host's HP" + (f" ({late[-1][2]:g} / {late[-1][3]:g})" if late else ""))
alive = [float(m[1]) for ln in host if (m := re.search(r"partyhp arisen (-?[\d.]+)/", ln))]
check(bool(alive) and alive[-1] > 0, f"host's Arisen alive at the end (HP {alive[-1] if alive else None})")
