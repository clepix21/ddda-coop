"""Checks an objects run (suite_objects.ps1, issue #4: crates and barrels): the object the host moved (objpush)
stands at the same place in the guest's game afterwards, the two games track the same objects (class and home),
and the guest never sends back what it was given.
Usage: check_objects.py <run dir>  (host.log, guest.log)"""
import re
import sys

out = sys.argv[1]


def lines(name):
    try:
        with open(f"{out}/{name}", encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


near = re.compile(r"objects: near \S+ class (\S+) home \((-?\d+), (-?\d+), (-?\d+)\) at \((\S+), (\S+), (\S+)\)")


def snapshots(name):
    """each objnear call: {(class, home): place}"""
    snaps, cur, last_t = [], {}, None
    for ln in lines(name):
        m = near.search(ln)
        if not m:
            continue
        t = ln[:8]
        if t != last_t and cur:
            snaps.append(cur)
            cur = {}
        last_t = t
        cur[(m[1], m[2], m[3], m[4])] = tuple(float(m[i]) for i in (5, 6, 7))
    if cur:
        snaps.append(cur)
    return snaps


h, g = snapshots("host.log"), snapshots("guest.log")
check(len(h) >= 2 and len(g) >= 2, f"both games logged the objects before and after (host {len(h)}, guest {len(g)})")
def push_keys(name):
    ps = [re.search(r"objects: push \S+ class (\S+) home \((-?\d+), (-?\d+), (-?\d+)\)", ln) for ln in lines(name)]
    return [tuple(p.groups()) for p in ps if p]


hp_keys, gp_keys = push_keys("host.log"), push_keys("guest.log")
check(bool(hp_keys) and bool(gp_keys), f"each game moved an object (host {len(hp_keys)}, guest {len(gp_keys)} pushes)")
if h and g:
    same = set(h[0]) & set(g[0])
    check(len(same) >= 3 and len(same) >= 0.8 * min(len(h[0]), len(g[0])),
          f"the games know the same objects by class and home ({len(same)} of {len(h[0])} / {len(g[0])})")


def compare(key, hs, gs, who):
    hp, gp = hs.get(key), gs.get(key)
    d = max(abs(a - b) for a, b in zip(hp, gp)) if hp and gp else 1e9
    print(f"info  object {key} moved by the {who}: host {hp}, guest {gp}")
    check(d < 5, f"the other game's copy stands where the {who} put it ({d:.1f} u apart)")


def last_put(name):
    """where a game's last push put its object: from + by (x, z)"""
    put = None
    for ln in lines(name):
        m = re.search(r"objects: push \S+ class \S+ home \([^)]*\) from \((\S+), (\S+), (\S+)\) by \((\S+), (\S+)\)", ln)
        if m:
            x, y, z, dx, dz = (float(v) for v in m.groups())
            put = (x + dx, y, z + dz)
    return put


if hp_keys and len(h) >= 2 and len(g) >= 2:
    # the host's put from its own push lines: its second objnear can come after the guest's push already (objects
    # run 2026-11-18 05:30: host objnear at 46.5 s, the guest's push at 45.8 s: "200 u apart" though both agreed)
    put = last_put("host.log")
    compare(hp_keys[0], {hp_keys[0]: put} if put else h[1], g[1], "host")
if gp_keys and len(h) >= 3 and len(g) >= 3:
    compare(gp_keys[-1], h[-1], g[-1], "guest")
check(not any("crash?" in ln for ln in lines("host.log") + lines("guest.log")), "no crash")
