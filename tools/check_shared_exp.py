r"""Checks the shared world exp runs (suite_shared_exp.ps1): in run "guest" the guest fights alone in the
underground while the host stays in Gran Soren; in run "host" the other way round. Each kill's exp
in one world must be given to the other player's party. Prints PASS/FAIL lines.
Usage: check_shared_exp.py <dir with guest/ and host/>"""
import re
import sys

out = sys.argv[1]


def lines(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


for run, fighter, other, slot in (("guest", "guest", "host", 1), ("host", "host", "guest", 0)):
    f = lines(f"{out}/{run}/{fighter}.log")
    o = lines(f"{out}/{run}/{other}.log")
    made = [int(m[1]) for ln in f for m in [re.search(r"exp: \+(\d+) in this world", ln)] if m]
    got = [int(m[1]) for ln in o for m in [re.search(r"exp: \+(\d+) from the other players given", ln)] if m]
    check(sum(made) > 0, f"{run} fights: its world gave {sum(made)} exp ({len(made)} kill(s))")
    check(sum(got) == sum(made) and sum(made) > 0, f"{run} fights: the {other} got the same {sum(got)} exp")
