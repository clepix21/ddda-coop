"""Checks a pair stage run (suite_stage.ps1): the host goes through the iron gate (stage 220 -> 600)
and the guest's game follows to the same stage; afterwards the guest's Arisen follows the host's.
Prints PASS/FAIL lines. Usage: check_stage.py <run dir>"""
import re
import sys

out = sys.argv[1]
reload = "--reload" in sys.argv  # the host also reloaded without a jump (retry after death)


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
connected = [ln for ln in host if "net: guest connected" in ln]
check(bool(connected), "guest connected")
hjump = [ln for ln in host if "net: stage: host jumps from" in ln]
check(bool(hjump), f"host jumped to another stage ({hjump[0][13:] if hjump else 'none'})")
hstage = [int(m[1]) for ln in host if (m := re.search(r"net: stage: host now in (-?\d+)", ln))]
gstage = [(t(ln), int(m[1])) for ln in guest if (m := re.search(r"net: stage: guest now in (-?\d+)", ln))]
final_host = hstage[-1] if hstage else None
check(final_host is not None and bool(gstage) and gstage[-1][1] == final_host, f"guest ends in the host's stage (host {final_host}, guest {[s for _, s in gstage]})")
gjump = [ln for ln in guest if "net: stage: guest in" in ln and "jumps to" in ln]
print("info  " + (gjump[0][13:] if gjump else "guest made no jump request of its own (its puppet's gate press, or none)"))
# the gate jump's target stage: when each copy got there
target = int(m[1]) if hjump and (m := re.search(r"to (\d+) \(point", hjump[0])) else None
t_jump = t(hjump[0]) if hjump else 0
host_new = [t(ln) for ln in host if f"net: stage: host now in {target} " in ln and t(ln) >= t_jump][:1] or [0]
guest_new = [ts for ts, s in gstage if s == target and ts >= t_jump - 1][:1] or [0]
late = bool(connected) and bool(hjump) and t(connected[0]) > t(hjump[0])
if late:  # the guest connected after the host's jump: it catches up through the 10 s rule
    start = [t(ln) for ln in guest if ln[13:].startswith("net: guest, ") or ln[13:].startswith("net: guest over")][:1] or [0]
    lag = guest_new[0] - start[0]
    check(bool(gstage) and 0 < lag < 30, f"late guest reached the host's stage {lag:.1f} s after connecting")
else:
    lag = guest_new[0] - host_new[0]
    check(bool(gstage) and abs(lag) < 20, f"guest reached the stage {lag:+.1f} s after the host")
err = [(t(ln), float(m[1])) for ln in guest if (m := re.search(r"net: guest arisen error ([\d.]+) u before correction", ln))]
after = [e for ts, e in err if guest_new[0] and ts > guest_new[0] + 15]
med = sorted(after)[len(after) // 2] if after else None
check(med is not None and med < 50, f"guest's Arisen follows the host's in the new stage (median error {med if med is None else round(med, 1)} u over {len(after)} samples)")
check(any("guest: saves always blocked" in ln for ln in guest) and any("save-guard: ACTIVE" in ln for ln in guest), "guest started with block_saves=0 and its role turned the save guard on")
gsaves = [ln for ln in guest if "save-guard: BLOCKED FileWrite" in ln]
after_jump = [ln for ln in gsaves if guest_new[0] and t(ln) > guest_new[0] - 10]
check(bool(after_jump), f"the guest's area-change autosave was blocked ({len(gsaves)} writes blocked)")
if reload:
    hreload = [(t(ln), ln) for ln in host if "stage: load" in ln and "caller 0050F28A" not in ln and "caller" in ln]
    hreload = [x for x in hreload if hjump and x[0] > t(hjump[0])]  # after the gate: the retry
    check(bool(hreload), "host reloaded without a jump (retry)" + (f": {hreload[0][1][13:]}" if hreload else ""))
    greq = [(t(ln), ln) for ln in guest if "host reloaded" in ln]
    check(bool(hreload) and bool(greq) and 0 <= greq[0][0] - hreload[0][0] < 20, "guest asked for the same reload" + (f" {greq[0][0] - hreload[0][0]:.1f} s later: {greq[0][1][13:]}" if greq and hreload else ""))
    gload = [(t(ln), ln) for ln in guest if "stage: load" in ln and greq and t(ln) >= greq[0][0]]
    check(bool(gload), "guest's stage loaded again" + (f": {gload[0][1][13:]}" if gload else ""))
    errs = [e for ts, e in err if gload and ts > gload[0][0] + 15]
    m2 = sorted(errs)[len(errs) // 2] if errs else None
    check(m2 is not None and m2 < 50, f"guest's Arisen follows the host's after the reload (median error {m2 if m2 is None else round(m2, 1)} u over {len(errs)} samples)")
if reload:
    ahead = [ln[13:] for ln in guest if "stages differ for 10 s" in ln]
    check(not ahead, "guest did not leave ahead of the host while the host was in menus" + (f": {ahead[0]}" if ahead else ""))
