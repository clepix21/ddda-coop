"""Checks a pair enemy run (suite_enemies.ps1): the guest pairs its enemies with the host's, follows
their positions, ends with the same HP and death state (death handler hook), and drops its own
hits on living enemies. Prints PASS/FAIL lines.
Usage: check_enemies.py <run dir>  (host.log, guest.log, host_enemies.txt, guest_enemies.txt)"""
import math
import re
import sys

out = sys.argv[1]


def lines(name):
    with open(f"{out}/{name}", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def dump(name):
    """slot -> (class, position, hp, hp max, host slot followed or -1, died)"""
    rows = {}
    for ln in lines(name):
        m = re.match(r"(\d+) (\S+) \S+ (\S+) (\S+) (\S+) size \S+ hp@\S+ (\S+) (\S+).* follows (-?\d+) dead (-?\d+)$", ln)
        if m:
            rows[int(m[1])] = (m[2], tuple(float(m[i]) for i in (3, 4, 5)), float(m[6]), float(m[7]), int(m[8]), int(m[9]))
    return rows


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


guest = lines("guest.log")
pairs = [ln for ln in guest if "paired with host" in ln]
check(len(pairs) >= 3, f"guest paired its enemies with the host's ({len(pairs)} pairs)")
spread = [float(m[1]) for ln in pairs if (m := re.search(r"spawns ([\d.]+) u apart", ln))]
by_pos = sum("positions" in ln for ln in pairs)
check(bool(spread) and max(spread) < 5, f"paired spawn points agree (worst {max(spread or [0]):.1f} u; {by_pos} more paired on current position)")
stats = [re.search(r"enemies: (\d+) from the host, (\d+) local updates applied, \d+ new pairs, \d+ unpaired updates, worst error ([\d.]+)", ln) for ln in guest]
stats = [(int(m[1]), int(m[2]), float(m[3])) for m in stats if m]
active = [s for s in stats if s[1] > 0]
check(len(active) >= 3, f"guest applied host enemy states in {len(active)} of {len(stats)} 5 s windows")
# after the first window (initial snaps): fast creatures move ~60 u between host pages, so the
# per-window worst error is judged at 100 u, with one spike allowed (a pair made mid-window)
later = [s[2] for s in active[1:]]
over = [x for x in later if x >= 100]
check(bool(later) and len(over) <= 1, f"guest enemies follow the host (worst error per window after the first: {', '.join(f'{x:.0f}' for x in later)} u)")
h, g = dump("host_enemies.txt"), dump("guest_enemies.txt")
check(bool(h) and bool(g), f"both copies dumped enemies at the end (host {len(h)}, guest {len(g)})")
# each guest enemy that follows a host slot must stand where that host enemy stands, with its HP and death
bad, followed = [], set()
for gi, (gc, gp, ghp, _, k, gdead) in sorted(g.items()):
    if k < 0 or k not in h:
        continue
    followed.add(k)
    hc, hpos, hhp, _, _, hdead = h[k]
    d = math.dist(gp, hpos)
    if hc != gc or d > 60 or ghp != hhp or gdead != hdead:
        bad.append(f"guest #{gi} {gc} follows host #{k} {hc}: {d:.0f} u apart, hp {ghp:g}/{hhp:g}, dead {gdead}/{hdead}")
check(not bad and bool(followed), f"guest enemies stand where the host enemy they follow stands, same HP and death ({len(followed)} pairs at the end)" + ("" if not bad else ": " + "; ".join(bad[:4])))
# Known gap (milestone 4): the host cannot make the guest's game spawn an enemy yet, so a host enemy
# the guest's game did not load stays missing there. Reported, not counted.
missing = [f"#{k} {c} at {p[0]:.0f},{p[2]:.0f}" for k, (c, p, *_) in sorted(h.items()) if k not in followed]
unfollowed = [f"#{k} {c} at {p[0]:.0f},{p[2]:.0f}" for k, (c, p, _, _, f, d) in sorted(g.items()) if f < 0 and not d]
print(f"KNOWN host enemies with no guest twin: {len(missing)}" + (f" ({'; '.join(missing)})" if missing else "") + f"; live guest enemies following nobody: {len(unfollowed)}" + (f" ({'; '.join(unfollowed)})" if unfollowed else ""))
dead = sum(1 for *_, hdead in h.values() if hdead == 1)
print(f"info  host enemies dead at the end: {dead} of {len(h)}")
hits = [re.search(r"local hits (\d+) blocked, (\d+) let through \(host: dead\), (\d+) deaths forced", ln) for ln in guest]
hits = [tuple(int(x) for x in m.groups()) for m in hits if m]
blocked, allowed, forced = (sum(x[i] for x in hits) for i in range(3))
# not every run has the guest's own party swinging at a living enemy (23:00 had none): reported only;
# the death match check above is what fails if a local hit killed one
print(f"info  guest's own hits on enemies alive on the host dropped: {blocked}")
print(f"info  guest hits let through on host-dead enemies: {allowed}; deaths forced: {forced}")
host_deaths = sum("enemy: died" in ln for ln in lines("host.log"))
guest_deaths = [ln for ln in guest if "enemy: died" in ln]
removals = sum(", removed" in ln for ln in guest)  # guest-only enemies, each followed by its own death line
forced_deaths = sum("forced" in ln for ln in guest_deaths) - removals
synced = len(guest_deaths) - removals
check(host_deaths > 0 and synced == host_deaths, f"each enemy death on the host also happened on the guest (host {host_deaths}, guest {synced}, of which {forced_deaths} forced; plus {removals} guest-only removed)")
hp_lines = [re.search(r"net: guest (arisen|pawn) error [\d.]+ u before correction, HP (-?[\d.]+) \(host (-?[\d.]+)\)", ln) for ln in guest]
hp_lines = [(m[1], float(m[2]), float(m[3])) for m in hp_lines if m]
off = [x for x in hp_lines[20:] if abs(x[1] - x[2]) > 1]  # after the first seconds of the connection
check(len(hp_lines) > 20 and not off, f"guest's Arisen and pawn keep the host's HP ({len(hp_lines)} samples, {len(off)} off" + (f", e.g. {off[0]}" if off else "") + ")")
pup = [re.search(r"(\d+) hits on the puppets blocked", ln) for ln in guest]
removed = [re.search(r"(\d+) guest-only removed", ln) for ln in guest]
print(f"info  guest-side hits on the puppets dropped: {sum(int(m[1]) for m in pup if m)}; guest-only enemies removed: {sum(int(m[1]) for m in removed if m)}")
