r"""Checks a lift run (suite_lift.ps1): the host lifted a monster, and on the guest the pawn showing the
host held it (caught by the game, or carried where the host has it) close to its hands.
Prints PASS/FAIL lines. Usage: check_lift.py <dir>"""
import re
import sys

out = sys.argv[1]


def lines(name):
    with open(f"{out}/{name}", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


host, guest = lines("host.log"), lines("guest.log")
lifts = [ln for ln in host if re.search(r"bridge: actname arisen -1 cPlAct\w*Lift", ln)]
check(bool(lifts), f"the host lifted a monster ({len(lifts)} of its actname samples in a Lift action)")
samples = [(m[1], float(m[2])) for ln in guest for m in [re.search(r"holding enemy \d+: (held|lost), (\d+) u from the pawn", ln)] if m]
carried = [ln for ln in guest if "carried where its player holds it" in ln]
held_close = [d for how, d in samples if d < 150]
check(bool(samples), f"the guest followed the host's hold ({len(samples)} samples, {len(carried)} carried)")
check(samples and len(held_close) >= 0.8 * len(samples),
      f"on the guest the monster stayed in the pawn's hands: {len(held_close)} of {len(samples)} samples within 150 u"
      + (f" (distances {sorted(d for _, d in samples)[:3]}..{sorted(d for _, d in samples)[-3:]})" if samples else ""))
