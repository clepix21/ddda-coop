r"""Checks a player grab run (suite_player_grab.ps1): each player's grab caught the other in its own world,
the hold was rebuilt in the other world, and no Arisen jumped (a "teleport", issue #1) while held.
Reads the "heights:" lines (test_log_heights=1, every 100 ms) and the hold lines. Prints PASS/FAIL lines.
Usage: check_player_grab.py <dir>"""
import re
import sys

out = sys.argv[1]
JUMP = 150.0  # u between two samples 100 ms apart (running is ~50)


def secs(ts):
    h, m, s = ts.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def lines(name):
    with open(f"{out}/{name}", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


def arisen_track(ls):
    rows = []
    for ln in ls:
        m = re.match(r"(\d\d:\d\d:\d\d\.\d+) heights: stage (-?\d+) \| A0\^? \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\) (\S+)", ln)
        if m:
            rows.append((secs(m[1]), float(m[3]), float(m[4]), float(m[5]), m[6]))
    return rows


def held_spans(track, names=("Hagaijime", "Lifted")):
    """time spans where this world's Arisen is held (its action)"""
    spans, start = [], None
    for t, x, y, z, act in track:
        held = any(n in act for n in names) and "cPlActHagaijime" != act  # DmgHagaijime: held; Hagaijime: holding
        if held and start is None:
            start = t
        if not held and start is not None:
            spans.append((start, t))
            start = None
    if start is not None:
        spans.append((start, track[-1][0]))
    return spans


def jumps(track, spans, pad=1.5):
    out_ = []
    for (t0, x0, y0, z0, a0), (t1, x1, y1, z1, a1) in zip(track, track[1:]):
        if not any(s - pad <= t1 <= e + pad for s, e in spans):
            continue
        d = ((x1 - x0) ** 2 + (y1 - y0) ** 2 + (z1 - z0) ** 2) ** 0.5
        if d > JUMP and t1 - t0 < 0.5:
            out_.append((round(d), a0, a1))
    return out_


host, guest = lines("host.log"), lines("guest.log")
for who, mine, other in (("host", host, guest), ("guest", guest, host)):
    acts = [m[1] for ln in mine for m in [re.search(r"bridge: actname (arisen .*)", ln)] if m]
    caught = [a for a in acts if re.search(r"arisen -1 cPlAct(Hagaijime|Grab|Lift)", a)]
    check(bool(caught), f"{who}'s grab caught the other player in its own world ({len(caught)} of {len(acts)} samples: "
                        f"{[a.split()[2] for a in acts]})")
    rebuilt = [ln for ln in other if "holds player" in ln and "hold rebuilt here" in ln]
    check(bool(rebuilt), f"{who}'s hold rebuilt in the other world ({len(rebuilt)} times)")
for who, mine in (("host", host), ("guest", guest)):
    track = arisen_track(mine)
    spans = held_spans(track)
    j = jumps(track, spans)
    check(bool(track), f"{who}: Arisen places logged ({len(track)} samples, held {len(spans)} times "
                       f"for {sum(e - s for s, e in spans):.1f} s)")
    check(not j, f"{who}: no jump over {JUMP:.0f} u while held or just after ({len(j)}: {j[:5]})")
    # under the floor after being let go (the release took it 145 u down and it fell for ever, 2026-11-06)
    sank = []
    for s, e in spans:
        before = [y for t, x, y, z, a in track if s - 1.0 <= t < s]
        after = [y for t, x, y, z, a in track if e <= t <= e + 6.0]
        if before and after and min(after) < before[-1] - 300:
            sank.append((round(before[-1]), round(min(after))))
    check(spans and not sank, f"{who}: back on its feet after each hold, not under the floor ({len(spans)} holds; sank: {sank})")
