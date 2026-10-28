r"""Checks a special deaths run (suite_revive_special.ps1, shared world): the guest's game death action
(cPlActDead, as an instant death) puts it down and the host helps it up; a lethal fall puts it down
where it last stood and after 60 s alone it comes back next to the host; the game's own death never
runs. Prints PASS/FAIL lines. Usage: check_revive_special.py <dir>"""
import re
import sys

out = sys.argv[1]


def lines(name):
    with open(f"{out}/{name}", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


host, guest = lines("host.log"), lines("guest.log")
check(any("bridge: actc 0 cPlActDead = number" in ln and "0xFFFFFFFF" not in ln for ln in guest), "the death action was found and asked for")
check(any("revive: down (cPlActDead)" in ln for ln in guest), "the death action put the guest down instead")
check(any("is down" in ln for ln in host), "host saw the guest down")
check(any("revive: helped player 1 up" in ln for ln in host) and any("revive: up again (helped)" in ln for ln in guest),
      "host held B next to it: the guest got up")
check(any("revive: down (a fall), back where it last stood" in ln for ln in guest), "the lethal fall put the guest down instead")
check(any("revive: put back where it last stood" in ln for ln in guest), "it was put back where it last stood")
wh = [tuple(float(v) for v in m.groups()) for ln in guest for m in [re.search(r"bridge: where (-?\d+) (-?\d+) (-?\d+)", ln)] if m]
lift = [float(m[1]) for ln in guest for m in [re.search(r"bridge: liftme \d+: the Arisen at height (-?\d+)", ln)] if m]
if len(wh) >= 4 and lift:
    ground, after = wh[1][1], wh[3][1]  # before the lift; 9.5 s after it
    check(abs(after - ground) < 300, f"after the fall it is back on the ground (height {after:.0f}, before {ground:.0f}, lifted to {lift[0]:.0f})")
else:
    check(False, f"after the fall it is back on the ground (where lines: {len(wh)})")
check(any("revive: up again (next to another player)" in ln for ln in guest), "nobody helped: after 60 s it came back next to the host")
check(not any("every player is down" in ln for ln in guest), "the game's own death never ran")
hw = [(float(m[1]), float(m[2])) for ln in host for m in [re.search(r"bridge: where (-?\d+) -?\d+ (-?\d+)", ln)] if m]
if wh and hw:
    d = ((wh[-1][0] - hw[-1][0]) ** 2 + (wh[-1][2] - hw[-1][1]) ** 2) ** 0.5
    check(d < 200, f"the guest stands next to the host at the end ({d:.0f} u)")
else:
    check(False, "the guest stands next to the host at the end")
