"""Checks a join-during-play run (suite_join.ps1): the guest's character reaches the host while the
host plays; the host saves, loads the merged save and reloads its world, after which its pawn 1 has
the guest's character (in tests the same frozen save, so the Arisen's max HP); the guest ends in game
with the host's merged save. With --three: guest 1 was in game from the start and guest 2 joins during play; guest 1 must reload
the host's new save. Prints PASS/FAIL lines. Usage: check_join.py <run dir> [--three]"""
import re
import sys

out = sys.argv[1]
three = "--three" in sys.argv[2:]
k, pawn = (2, 2) if three else (1, 1)  # the guest that joins during play, and the pawn it gets


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
arrived = [ln for ln in host if re.search(rf"net: guest {k}'s character arrived: .* it replaces pawn {pawn}$", ln)]
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
pk = f"pawn {pawn}"
before = [p for tt, p in hp if when is not None and tt < when and "arisen" in p and pk in p]
after = [p for tt, p in hp if when is not None and tt > when + 5 and "arisen" in p and pk in p]
check(bool(before) and before[-1][pk][1] != before[-1]["arisen"][1],
      f"before: host's {pk} is its hired pawn" + (f" (max HP {before[-1][pk][1]:.0f}, Arisen {before[-1]['arisen'][1]:.0f})" if before else ""))
check(bool(after) and after[-1][pk][1] == after[-1]["arisen"][1],
      f"after: host's {pk} has the guest's character" + (f" (max HP {after[-1][pk][1]:.0f})" if after else ""))


def shows_all(log, name):
    gp = parties(log)
    keys = ["pawn 1", "pawn 2"] if three else ["pawn 1"]
    last = gp[-1][1] if gp else {}
    ok = "arisen" in last and all(x in last and last[x][1] == last["arisen"][1] for x in keys)
    check(ok, f"{name}'s world shows the guests' characters as {', '.join(keys)}" +
          (f" (max HP {[round(last.get(x, (0, -1))[1]) for x in keys]})" if last else ""))


if three:
    g2 = lines("guest2.log")
    check(any("rejoin: the guest's world shows" in ln for ln in guest), "guest 1 (in game) saw that the host's world changed")
    check(any("rejoin: world reloaded with characters" in ln and not ln.endswith(" 00000000") for ln in guest),
          "guest 1 reloaded the host's new save during play")
    shows_all(host, "host")
    shows_all(guest, "guest 1")
    shows_all(g2, "guest 2")
    sys.exit(0)
gread = [ln for ln in guest if "FileRead(\"DDDA.sav\"" in ln and "the host's save" in ln]
check(bool(gread), "guest's game read the host's save" + (f": {gread[-1][13:]}" if gread else ""))
gp = parties(guest)
check(bool(gp) and "pawn 1" in gp[-1][1] and "arisen" in gp[-1][1] and gp[-1][1]["pawn 1"][1] == gp[-1][1]["arisen"][1],
      "guest's world shows its own character as pawn 1" + (f" (max HP {gp[-1][1]['pawn 1'][1]:.0f})" if gp and 'pawn 1' in gp[-1][1] else ""))
