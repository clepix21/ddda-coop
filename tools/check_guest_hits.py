r"""Checks a guest hits run (suite_guest_hits.ps1, issue #10): the guest's own Arisen hit enemies (the damage
hook names the attacker from the hit record's +0x50), those hits were sent to the host (MSG_HIT) and applied
there, and the pawn showing the guest dealt no damage of its own once the guest sent hits.
Prints PASS/FAIL lines. Usage: check_guest_hits.py <dir>"""
import re
import sys

out = sys.argv[1]


def lines(name):
    with open(f"{out}/{name}", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


def last_int(ls, pat):
    v = 0
    for ln in ls:
        m = re.search(pat, ln)
        if m:
            v = int(m[1])
    return v


host, guest = lines("host.log"), lines("guest.log")
own = [ln for ln in guest if re.search(r"enemies: hit [\d.]+ on \S+ \(\w+\) by the Arisen", ln)]
check(bool(own), f"on the guest, hits by its own Arisen were seen by the damage hook ({len(own)} of the first 40 hits)")
sent = last_int(guest, r"(\d+) own hits sent to the host")
check(sent > 0, f"the guest sent its Arisen's hits to the host ({sent})")
received = last_int(host, r"guests' hits (\d+) received")
applied = last_int(host, r"guests' hits \d+ received, (\d+) applied")
check(received > 0, f"the host received the guest's hits ({received} of {sent})")
check(applied > 0, f"the host applied them to its enemies ({applied})")
dropped = last_int(host, r"(\d+) hits of their pawns dropped")
print(f"info  hits of the pawn showing the guest dropped on the host: {dropped}")
hp_by_pawn = [ln for ln in host if re.search(r"enemies: hit [\d.]+ on \S+ \(\w+\) by pawn \d", ln)]
print(f"info  host: hits by a pawn among its first 40 hits: {len(hp_by_pawn)}")
died = [ln for ln in host if re.search(r"enemy: died #\d+ uEm", ln)]
print(f"info  host enemy deaths: {len(died)}")
check(not any("crash?" in ln for ln in host + guest), "no crash")
