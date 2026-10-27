"""Write a reproducible random-input bot script for soak tests.
Usage: python -I make_soak.py <load script> <out> <start_s> <duration_s> <seed>
Pads 1 and 2 get random sticks and face buttons every 0.4-1.5 s; one 12 s straight run by
pad 2 in the middle checks whether the game pulls a far-away pawn back.
"""
import random
import sys

load, out, start, dur, seed = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), int(sys.argv[5])
rng = random.Random(seed)
buttons = [0, 0, 0, 0x4000, 0x8000, 0x1000, 0x2000, 0x0100, 0x0200]  # none, X, Y, A, B, LB, RB
lines = open(load).read().splitlines() + ['0 1 0']
mid = start + dur / 2
for pad in (0, 1):
    t = start
    while t < start + dur:
        if pad == 1 and mid <= t < mid + 12:
            lines.append(f'{int(t * 1000)} 1 0 30000 0  # long straight run')
            t = mid + 12
            continue
        b = rng.choice(buttons)
        lx, ly = rng.randint(-32000, 32000), rng.randint(-32000, 32000)
        if rng.random() < 0.3:
            lx = ly = 0
        lines.append(f'{int(t * 1000)} {pad} {b:x} {lx} {ly}')
        t += rng.uniform(0.4, 1.5)
        lines.append(f'{int(t * 1000)} {pad} 0 {lx} {ly}')
        t += 0.1
for pad in (0, 1):
    lines.append(f'{int((start + dur) * 1000)} {pad} 0')
open(out, 'w').write('\n'.join(lines) + '\n')
print(f'{len(lines)} lines')
