"""Checks a party run (suite_party.ps1): two guests, each with its own character (from its own save),
replace the host's hired pawns 1 and 2 in a merged save that the host and both guests load; the
host's autosave is un-merged. Prints PASS/FAIL lines. Usage: check_party.py <run dir>"""
import re
import sys

out = sys.argv[1]


def lines(name):
    with open(f"{out}/{name}", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


import os

host, g1, g2 = lines("host.log"), lines("guest.log"), lines("guest2.log")
four = os.path.exists(f"{out}/guest3.log")  # suite_party.ps1 -Four: a third guest takes the main pawn
g3 = lines("guest3.log") if four else []
for k, pawn in ((1, 1), (2, 2)) + (((3, 0),) if four else ()):
    arr = [ln[13:] for ln in host if re.search(rf"net: guest {k}'s character arrived: .* it replaces pawn {pawn}$", ln)]
    check(bool(arr), f"guest {k}'s character reached the host for pawn {pawn}" + (f": {arr[0]}" if arr else ""))
merged = [ln[13:] for ln in host if "merged save built" in ln and "pawn 1 = " in ln and "pawn 2 = " in ln and (not four or "pawn 0 = " in ln)]
check(bool(merged), "host built a save with every guest's character" + (f": {merged[-1]}" if merged else ""))
check(any("the merged save (guests' characters as pawns)" in ln for ln in host), "host's game read the merged save")
guests = (("guest 1", g1), ("guest 2", g2)) + ((("guest 3", g3),) if four else ())
for name, g in guests:
    check(any("the guest's own save read" in ln for ln in g) and any("the guest's character:" in ln for ln in g),
          f"{name} read its own save and took its character from it")
    check(any("FileRead(\"DDDA.sav\"" in ln and "the host's save" in ln for ln in g), f"{name}'s game read the host's (merged) save")


def party(log):
    """last partyhp lines: {who: (hp, max)}"""
    out = {}
    for ln in log:
        m = re.search(r"partyhp (arisen|pawn \d) (-?[\d.]+)/(-?[\d.]+)", ln)
        if m:
            out[m[1]] = (float(m[2]), float(m[3]))
    return out


arisen_max = party(host).get("arisen", (0, 0))[1]
pawns = (0, 1, 2) if four else (1, 2)
for name, log in (("host", host),) + guests:
    p = party(log)
    ok = p and all(p.get(f"pawn {i}", (0, 0))[1] == arisen_max for i in pawns)
    check(bool(ok), f"on {name}, pawns {pawns} have the guests' characters' max HP ({arisen_max:g}): {p}")
unm = [ln[13:] for ln in host if f"write un-merged: {len(pawns)} pawn(s)" in ln]
check(bool(unm), "host's autosave was un-merged (its own pawns put back)" + (f": {unm[0]}" if unm else ""))
