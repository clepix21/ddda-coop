"""Checks an inn run (suite_inn.ps1, inn save): the host talks to the innkeeper and rests (a talk,
then a jump in time to 6:00 within the same stage). The guest's clock must follow, and its Arisen
must stand where the host's does with the same HP. Prints PASS/FAIL lines. Usage: check_inn.py <dir>"""
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
    """Seconds since the first time seen; a run may cross midnight (seen 2026-10-27)."""
    global _t_first
    h, m, s = ln[:12].split(":")
    v = int(h) * 3600 + int(m) * 60 + float(s)
    if _t_first is None:
        _t_first = v
    d = v - _t_first  # the shortest way round the clock: right in both directions within 12 h
    return d + 86400 if d < -43200 else d - 86400 if d > 43200 else d


host, guest = lines("host.log"), lines("guest.log")
talk = [ln for ln in host if re.search(r"world: host event state 256 -> 7", ln)]
check(bool(talk), "host talked (event state 256 -> 7)")
rest = [(t(ln), ln) for ln in host if re.search(r"world: host event state 7 -> 256 \(time day \d+ 06:0", ln)]
check(bool(rest), "host rested until 6:00" + (f": {rest[0][1][13:]}" if rest else ""))
loads = [ln for ln in host if "stage: load" in ln and talk and t(ln) > t(talk[0])]  # after the talk began
check(not loads, "the rest did not load a stage (same stage, a jump in time)")
setl = [(t(ln), ln) for ln in guest if "world: guest time" in ln and "set to the host's" in ln]
t_talk = t(talk[0]) if talk else 0
after = [x for x in setl if talk and x[0] >= t_talk]  # the time jumps during the rest, before it ends
check(bool(after) and re.search(r"set to the host's day \d+ 06:0", after[0][1]) is not None, "guest's clock jumped to the host's 6:00" + (f": {after[0][1][13:]}" if after else ""))
# afterwards the two clocks agree (sampled every 5 s on the guest)
samples = [(t(ln), int(m[1]) * 86400000 + int(m[2]), int(m[3]) * 86400000 + int(m[4])) for ln in guest if (m := re.search(r"world: time guest day (-?\d+) (-?\d+) ms, host day (-?\d+) (-?\d+) ms", ln))]
later = [abs(g - h) for ts, g, h in samples if rest and ts > rest[0][0] + 5]
check(bool(later) and max(later) <= 60000, f"guest's clock then agrees with the host's (worst gap {max(later) / 1000 if later else None} game s over {len(later)} samples)")
pos = [(t(ln), float(m[1]), float(m[2]), float(m[3])) for ln in guest if (m := re.search(r"net: guest arisen error ([\d.]+) u before correction, HP (-?[\d.]+) \(host (-?[\d.]+)\)", ln))]
late = [p for p in pos if rest and p[0] > rest[0][0] + 10]
med = sorted(p[1] for p in late)[len(late) // 2] if late else None
check(med is not None and med < 50, f"guest's Arisen stands where the host's does after the rest (median error {med if med is None else round(med, 1)} u)")
check(bool(late) and all(abs(p[2] - p[3]) <= 1 for p in late), "guest's Arisen has the host's HP after the rest" + (f" ({late[-1][2]:g} / {late[-1][3]:g})" if late else ""))
# the guest's game goes through the same talk: the host's B starts it on the guest's Arisen puppet,
# the host's pad drives its lines and menus, and it ends with the host's
gstart = [t(ln) for ln in guest if "world: guest event state 256 -> 7" in ln]
gend = [t(ln) for ln in guest if "world: guest event state 7 -> 256" in ln]
d0 = gstart[0] - t_talk if gstart and talk else None
check(d0 is not None and abs(d0) <= 2, f"guest's game started the same talk (after {d0 if d0 is None else round(d0, 2)} s)")
d1 = gend[0] - rest[0][0] if gend and rest else None
check(d1 is not None and -1 <= d1 <= 15, f"guest's talk ended soon after the host's (after {d1 if d1 is None else round(d1, 2)} s)")
gev = [ln[13:] for ln in guest if "world: guest event state" in ln]
print("info  guest event states: " + "; ".join(gev))
