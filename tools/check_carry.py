"""Checks a carry run (suite_carry.ps1, issue #4: carried jars and barrels): the host's Arisen really lifts an
object (RT), the guest's copy follows above the host's head while it is carried, and once the object broke in
the host's game the guest's copy is out of sight (no longer within 1500 u in its objnear lists).
Usage: check_carry.py <run dir>  (host.log, guest.log)"""
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


def secs(ts):
    h, m, s = ts.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


host, guest = lines("host.log"), lines("guest.log")
lifted = [ln for ln in host if re.search(r"actname arisen -1 cPlActLift", ln)]
check(bool(lifted), f"the host's Arisen lifted an object with RT ({len(lifted)} samples in cPlActLift*)")
carry = next((re.search(r"^(\S+) objects: this Arisen carries \S+ \(class (\S+) home \((-?\d+), (-?\d+), (-?\d+)\)\)", ln)
              for ln in host if "this Arisen carries" in ln), None)
check(carry is not None, "the host's game knew which object its Arisen carried")
if carry:
    t0, cls, hx, hy, hz = carry[1], carry[2], carry[3], carry[4], carry[5]
    home = f"home ({hx}, {hy}, {hz})"
    # the guest's copy, by class and home: moved by the host's game, up above where it stood
    moved = [re.search(r"moved by another player: from \(\S+, \S+, \S+\) to \((\S+), (\S+), (\S+)\)", ln) for ln in guest
             if "moved by another player: from" in ln and secs(ln[:12]) >= secs(t0[:12]) - 1]
    up = [m for m in moved if m and float(m[2]) > float(hy) + 80]
    check(bool(up), f"the guest's copy followed above the carrier's head ({len(up)} of {len(moved)} moves over y {int(hy) + 80})")
    gone = next((ln for ln in host if "is gone: its copies are put out of sight" in ln), None)
    if gone:
        tg = secs(gone[:12])
        later = [ln for ln in guest if "objects: near" in ln and f"class {cls} {home}" in ln and secs(ln[:12]) > tg + 2]
        check(not later, f"after it broke, the guest's copy is out of sight ({len(later)} objnear lines still near)")
    else:
        print("info  the object did not break (a whole object is synced as any moved one)")
check(not any("crash?" in ln for ln in host + guest), "no crash")
