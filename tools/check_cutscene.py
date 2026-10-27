"""Checks a cutscene run (suite_cutscene.ps1, the owner's save before the Olra cutscene on the
Cassardis pier): the host walks onto the pier and its cutscene plays; the guest's game must play it
too, at the same time, and its Arisen must follow the host's afterwards. A cutscene shows as bit 31
of sGameSys +0xB883C (second word of the "scene flags" log lines; dumps 2026-10-27 11:06).
Prints PASS/FAIL lines. Usage: check_cutscene.py <dir>"""
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
    d = v - _t_first
    return d + 86400 if d < -43200 else d - 86400 if d > 43200 else d


def scenes(log, who):
    """(start, end) times of each cutscene in a log."""
    spans, start = [], None
    for ln in log:
        m = re.search(rf"world: {who} scene flags \w+ (\w+) ", ln)
        if not m:
            continue
        on = int(m.group(1), 16) & 0x80000000
        if on and start is None:
            start = t(ln)
        elif not on and start is not None:
            spans.append((start, t(ln)))
            start = None
    if start is not None:
        spans.append((start, None))
    return spans


host, guest = lines("host.log"), lines("guest.log")
t(host[0])
hs, gs = scenes(host, "host"), scenes(guest, "guest")
check(len(hs) == 1 and hs[0][1] is not None, f"host played one cutscene: {[(round(a, 1), b and round(b, 1)) for a, b in hs]}")
check(len(gs) == 1 and gs[0][1] is not None, f"guest played one cutscene: {[(round(a, 1), b and round(b, 1)) for a, b in gs]}")
if hs and gs and hs[0][1] and gs[0][1]:
    ds, de = gs[0][0] - hs[0][0], gs[0][1] - hs[0][1]
    check(abs(ds) < 2, f"guest's cutscene started with the host's ({ds:+.2f} s)")
    check(abs(de) < 2, f"guest's cutscene ended with the host's ({de:+.2f} s)")
else:
    check(False, "guest's cutscene started with the host's")
    check(False, "guest's cutscene ended with the host's")
loads = [ln for ln in guest if "stage: load" in ln and hs and t(ln) > hs[0][0]]
check(not loads, "no stage load on the guest during or after the cutscene")
end = hs[0][1] if hs and hs[0][1] else 1e9
errs = [float(m.group(1)) for ln in guest if t(ln) > end + 5
        for m in [re.search(r"guest arisen error ([\d.]+) u", ln)] if m]
errs.sort()
med = errs[len(errs) // 2] if errs else None
check(med is not None and med < 1.0, f"guest's Arisen follows the host's after the cutscene (median error {med} u, {len(errs)} samples)")
