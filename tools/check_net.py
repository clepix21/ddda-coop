"""Assertions for a two-copy network run (run_pair.ps1 output folder).

Usage: python -I check_net.py <pair dir> --pawn-window START END
  START/END: seconds after the host's DLL load during which the guest pushes its stick FORWARD.
Checks:
  - the guest connected to the host and possession turned on;
  - the Arisen and the main pawn on the guest follow the host (compare_pair.py);
  - while the guest pushes forward, the host's main pawn moves along the guest camera's forward
    vector (dot product > 0.95), which is only true if the host used the guest's camera.
Exit 1 on any failure.
"""
import math
import re
import subprocess
import sys
from pathlib import Path

d = Path(sys.argv[1])
args = sys.argv[2:]
ws, we = map(float, args[args.index('--pawn-window') + 1:args.index('--pawn-window') + 3])
# the pawn the (first) guest drives: hired pawn 1 by default (pawn_order 1,2,0)
cmc = args[args.index('--cmc') + 1] if '--cmc' in args else '1'
# --guest-log / --guest-num: which guest (several guests: guest.log / guest2.log, guest 1 / 2)
guest_log = args[args.index('--guest-log') + 1] if '--guest-log' in args else 'guest.log'
guest_num = args[args.index('--guest-num') + 1] if '--guest-num' in args else None
tools = Path(__file__).parent
host = (d / 'host.log').read_text(encoding='utf-8', errors='replace').splitlines()
failed = 0


def check(ok, what):
    global failed
    failed += not ok
    print(f'{"PASS" if ok else "FAIL"}  {what}')


check(any('net: guest connected' in l for l in host), 'guest connected to the host')
check(any('possess: ON' in l for l in host), 'possession turned on by the guest')

r = subprocess.run([sys.executable, '-I', str(tools / 'compare_pair.py'), str(d / 'host.log'), str(d / guest_log), '--cmc', cmc],
                   capture_output=True, text=True)
for line in r.stdout.splitlines():
    print(line)
failed += r.returncode != 0

cam_re = rf'guest {guest_num} camera forward' if guest_num else r'guest (?:\d )?camera forward'
cams = [tuple(map(float, m.groups())) for l in host if (m := re.search(cam_re + r' \((\S+), (\S+)\)', l))]
r = subprocess.run([sys.executable, '-I', str(tools / 'analyze_log.py'), str(d / 'host.log'), '--window', 'fwd', str(ws), str(we), '--cmc', cmc],
                   capture_output=True, text=True)
m = re.search(r'pawn: moved\s+(\S+) heading \((\S+), (\S+)\)', r.stdout)
if not cams or not m:
    check(False, 'pawn heading follows the guest camera (no data)')
else:
    cx, cz = cams[-1]
    hx, hz = float(m.group(2)), float(m.group(3))
    dot = cx * hx + cz * hz
    check(float(m.group(1)) > 100 and dot > 0.95,
          f'pawn heading ({hx:+.2f}, {hz:+.2f}) follows the guest camera ({cx:+.3f}, {cz:+.3f}), dot {dot:.3f}, moved {m.group(1)} u')
sys.exit(1 if failed else 0)
