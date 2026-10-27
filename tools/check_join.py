"""Checks a join-during-play run (suite_join.ps1): the guest's character reaches the host while the
host plays; the host saves, loads the merged save and reloads its world, after which its pawn 1 has
the guest's character (in tests the same frozen save, so the Arisen's max HP); the guest ends in game
with the host's merged save. Prints PASS/FAIL lines. Usage: check_join.py <run dir>"""
import re
import sys

out = sys.argv[1]


def lines(name):
    with open(f"{out}/{name}", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


def t(ln):
    h, m, s = ln[:12].split(":")
    return (int(h) * 3600 + int(m) * 60 + float(s)) % 86400


host, guest = lines("host.log"), lines("guest.log")
first_stage = next((t(ln) for ln in host if "stage: load" in ln), None)
arrived = [ln for ln in host if re.search(r"net: guest 1's character arrived: .* it replaces pawn 1$", ln)]
check(bool(arrived) and first_stage is not None and t(arrived[0]) > first_stage,
      "guest's character reached the host after the host was in game" + (f": {arrived[0][13:]}" if arrived else ""))
check(any("rejoin: idle -> saving" in ln for ln in host), "host asked its game for a save")
check(any("rejoin: merging -> loading" in ln for ln in host), "host asked its game to load the merged save")
reloaded = [ln for ln in host if "rejoin: world reloaded with characters" in ln and not ln.endswith(" 00000000")]
check(bool(reloaded), "host's world reloaded with the guest's character" + (f": {reloaded[0][13:]}" if reloaded else ""))
check(not any("gave up" in ln for ln in host + guest), "no rejoin step given up")


def parties(log):
    """(time, {who: (hp, max)}) per partyhp burst"""
    res = []
    for ln in log:
        m = re.search(r"partyhp (arisen|pawn \d) (-?[\d.]+)/(-?[\d.]+)", ln)
        if not m:
            continue
        if not res or t(ln) - res[-1][0] > 1:
            res.append((t(ln), {}))
        res[-1][1][m[1]] = (float(m[2]), float(m[3]))
    return res


hp = parties(host)
when = t(reloaded[0]) if reloaded else None
before = [p for tt, p in hp if when is not None and tt < when and "arisen" in p and "pawn 1" in p]
after = [p for tt, p in hp if when is not None and tt > when + 5 and "arisen" in p and "pawn 1" in p]
check(bool(before) and before[-1]["pawn 1"][1] != before[-1]["arisen"][1],
      "before: host's pawn 1 is its hired pawn" + (f" (max HP {before[-1]['pawn 1'][1]:.0f}, Arisen {before[-1]['arisen'][1]:.0f})" if before else ""))
check(bool(after) and after[-1]["pawn 1"][1] == after[-1]["arisen"][1],
      "after: host's pawn 1 has the guest's character" + (f" (max HP {after[-1]['pawn 1'][1]:.0f})" if after else ""))
gread = [ln for ln in guest if "FileRead(\"DDDA.sav\"" in ln and "the host's save" in ln]
check(bool(gread), "guest's game read the host's save" + (f": {gread[-1][13:]}" if gread else ""))
gp = parties(guest)
check(bool(gp) and "pawn 1" in gp[-1][1] and "arisen" in gp[-1][1] and gp[-1][1]["pawn 1"][1] == gp[-1][1]["arisen"][1],
      "guest's world shows its own character as pawn 1" + (f" (max HP {gp[-1][1]['pawn 1'][1]:.0f})" if gp and 'pawn 1' in gp[-1][1] else ""))
