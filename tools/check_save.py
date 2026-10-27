"""Checks a save sharing run (suite_save.ps1): the host's latest save (its autosave at the iron
gate, stage 600) reaches the guest, and the guest's game, loading late, reads it instead of its own
(which is in Gran Soren, stage 220). Prints PASS/FAIL lines. Usage: check_save.py <run dir>"""
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
    d = v - _t_first  # the shortest way round the clock: right in both directions within 12 h
    return d + 86400 if d < -43200 else d - 86400 if d > 43200 else d


host, guest = lines("host.log"), lines("guest.log")
sent = [(int(m[1]), ln) for ln in host if (m := re.search(r"net: save: sending save version (\d+)", ln))]
check(len(sent) >= 2, f"host sent its save, then again after its gate autosave ({len(sent)} versions: {[v for v, _ in sent]})")
latest = sent[-1][0] if sent else None
arrived = [(int(m[1]), t(ln)) for ln in guest if (m := re.search(r"the host's save version (\d+) arrived", ln))]
check(bool(arrived) and arrived[-1][0] == latest, f"guest got the host's latest save (versions arrived {[v for v, _ in arrived]})")
reads = [(t(ln), ln[13:]) for ln in guest if "FileRead(\"DDDA.sav\"" in ln and "the host's save" in ln]
check(bool(reads), "the guest's game read the host's save" + (f": {reads[-1][1]}" if reads else ""))
loads = [(t(ln), int(m[1])) for ln in guest if (m := re.search(r"stage: load (\d+) point \d+ .*caller 004FB46E", ln))]
check(bool(loads) and loads[-1][1] == 600, f"the guest's game loaded the host's place (stage {loads[-1][1] if loads else None}; its own save is in 220)")
err = [(t(ln), float(m[1])) for ln in guest if (m := re.search(r"net: guest arisen error ([\d.]+) u before correction", ln))]
after = [e for ts, e in err if loads and ts > loads[-1][0] + 15]
med = sorted(after)[len(after) // 2] if after else None
check(med is not None and med < 50, f"guest's Arisen follows the host's after loading (median error {med if med is None else round(med, 1)} u over {len(after)} samples)")
check(any("save owner check patched" in ln for ln in guest), "guest accepts a save owned by another account (owner check patched; untested with two real accounts)")
