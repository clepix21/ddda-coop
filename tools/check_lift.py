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
# a lifted or thrown monster's copy never sent far away on the guest (issue #10: "far from host #8: 30874 u",
# the host sending a lifted monster's place as about (0, 0, 0)); held-back jumps are counted, not failed
# Only a bogus host place fails (near the origin, as in the issue): the guest's own copy found near the
# origin at the release, with a sane host place, is the copy being brought back (lift run 2026-11-22 04:36:03,
# "here (-7, 2, 2), host (2706, -539, 5300)").
far, back = [], []
for ln in guest:
    m = re.search(r"far from host #\d+: ([\d.]+|inf) u \(here \(([-\d.e+]+), ([-\d.e+]+), ([-\d.e+]+)\), host \(([-\d.e+]+), ([-\d.e+]+), ([-\d.e+]+)\)", ln)
    if not m or not (m[1] == "inf" or float(m[1]) > 2000):
        continue
    host_near_origin = sum(abs(float(v)) for v in m.group(5, 6, 7)) < 600
    (far if host_near_origin else back).append(m[1])
check(not far, f"on the guest no enemy copy was sent to a bogus host place near the origin ({len(far)}: {far[:5]})")
print(f"info  guest copies far from a sane host place, brought back: {len(back)} {back[:5]}")
held = [ln for ln in guest if "place jumped" in ln and "not taken yet" in ln]
print(f"info  host places held back on the guest as jumps: {len(held)}")
throws = [ln for ln in host if re.search(r"bridge: actname arisen -1 cPlAct\w*(Throw|LiftEnd)", ln)]
print(f"info  host throw or lift-end samples: {len(throws)}")
