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
    # or its own Arisen holding in the places log (the rough run samples the action after the hold)
    held_rows = sum(1 for ln in mine if re.search(r" heights: .*\| A0\^? \([^)]*\) cPlActHagaijime", ln))
    check(bool(caught) or held_rows > 5, f"{who}'s grab caught the other player in its own world ({len(caught)} of {len(acts)} samples: "
                        f"{[a.split()[2] for a in acts]}; {held_rows} places logged holding)")
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

# the held player jumped in its own world while the other still held it (Nexus 2026-11-08: "the guest turns
# into a frog on a journey of endless jumps"), and no fall from a hold put it down (03:20 rough run)
for who, mine, other in (("host", host, guest), ("guest", guest, host)):
    holding = []
    start = None
    for t, x, y, z, act in arisen_track(mine):
        if act.startswith("cPlActHagaijime") and start is None:
            start = t
        elif not act.startswith("cPlActHagaijime") and start is not None:
            holding.append((start, t))
            start = None
    # not after the hold ended in the jumper's own world (it struggled free: the game's own escape)
    freed = [secs(ln[:12]) for ln in other if re.search(r"let go of player \d here", ln)]
    # a jump that starts in the hold: the tail of one begun before the grab is not (a grab catches a player
    # in mid-air at once, by design: rough run 2026-11-14 17:43:59, JumpBegin 0.23 s before the GrabStart)
    track = arisen_track(other)
    starts = [(t, a) for k, (t, x, y, z, a) in enumerate(track) if "Jump" in a and (k == 0 or "Jump" not in track[k - 1][4])]
    hops = [(round(t, 1), a) for t, a in starts if any(s <= t <= e for s, e in holding)
            and not any(0 <= t - f <= 3.0 for f in freed)]
    check(not hops, f"{who} holding: the other player did not jump meanwhile ({len(holding)} holds; jumps: {hops[:4]})")
falls = [ln for ln in host + guest if "revive: down (a fall)" in ln]
check(not falls, f"no player went down from a fall after a hold ({len(falls)})")
