r"""Checks a revive run (suite_revive.ps1, shared world): the guest takes a lethal blow and goes down
instead of dying, the host holds B next to it and it gets up; a second lethal blow and nobody helps:
after 60 s it comes back next to the host. Prints PASS/FAIL lines. Usage: check_revive.py <dir>"""
import re
import sys

out = sys.argv[1]


def lines(name):
    with open(f"{out}/{name}", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


host, guest = lines("host.log"), lines("guest.log")
downs = [ln for ln in guest if "revive: down (" in ln]
check(len(downs) >= 2, f"guest went down instead of dying ({len(downs)} times)")
check(not any("every player is down" in ln for ln in guest), "no false 'every player is down'")
check(any("is down" in ln for ln in host), "host saw the guest down")
check(any("revive: helped player 1 up" in ln for ln in host) and any("revive: up again (helped)" in ln for ln in guest),
      "host held B next to it: the guest got up")
back = [ln for ln in guest if "revive: up again (next to another player)" in ln]
check(bool(back), "nobody helped: after 60 s the guest came back next to the host")
wh = [(float(m[1]), float(m[2])) for ln in guest for m in [re.search(r"bridge: where (-?\d+) -?\d+ (-?\d+)", ln)] if m]
hw = [(float(m[1]), float(m[2])) for ln in host for m in [re.search(r"bridge: where (-?\d+) -?\d+ (-?\d+)", ln)] if m]
if wh and hw:
    d = ((wh[-1][0] - hw[-1][0]) ** 2 + (wh[-1][1] - hw[-1][1]) ** 2) ** 0.5
    check(d < 200, f"the guest stands next to the host at the end ({d:.0f} u)")
else:
    check(False, "the guest stands next to the host at the end")
