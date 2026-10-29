r"""Checks a skills run (suite_skills.ps1, shared world): every weapon action (skill, attack: numbers
with high bits) the host's Arisen starts is started by the guest's pawn showing the host within 1 s.
Prints PASS/FAIL lines. Usage: check_skills.py <dir>"""
import re
import sys
from datetime import datetime

out = sys.argv[1]


def lines(name):
    with open(f"{out}/{name}", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def t(ln):
    return datetime.strptime(ln[:12], "%H:%M:%S.%f")


host, guest = lines("host.log"), lines("guest.log")
mine = [(t(ln), int(m[1], 16)) for ln in host for m in [re.search(r"actions: arisen \S+ starts .*\((0x[0-9A-F]+)\)$", ln)] if m]
theirs = [(t(ln), int(m[1], 16)) for ln in guest for m in [re.search(r"actions: puppet \S+ starts .*\((0x[0-9A-F]+)\)$", ln)] if m]
weapon = [(at, n) for at, n in mine if 1 <= n >> 24 <= 0x0F]
print(("PASS  " if len(weapon) >= 5 else "FAIL  ") + f"the host started {len(weapon)} weapon actions")
for at, n in weapon:
    ok = any(n2 == n and 0 <= (at2 - at).total_seconds() <= 1.0 for at2, n2 in theirs)
    print(("PASS  " if ok else "FAIL  ") + f"guest's pawn of the host played 0x{n:X} (host at {at.time()})")
