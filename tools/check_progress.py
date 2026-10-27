r"""Checks a guest progress run (suite_progress.ps1): (1) two copies, the guest's character (a level-44
Arisen) plays hired pawn 1, the host's party kills the small creatures of the underground and the
host saves (mode 7): the guest must get the party's exp and discipline in coop_progress\pending.txt;
(2) the guest's copy alone with keep_progress=1: its save, read with the gains added (backup first),
then written by the game, must hold them. Prints PASS/FAIL lines. Usage: check_progress.py <dir>"""
import hashlib
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import save_merge  # noqa: E402

out = sys.argv[1]
guest_dir = sys.argv[2]   # the guest copy's folder (DDDA2)
own_save = sys.argv[3]    # the guest's own save (test_save)


def lines(name):
    with open(os.path.join(out, name), encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


host, guest, solo = lines("host.log"), lines("guest.log"), lines("solo.log")
rec = [re.search(r"progress: pawn (\d) \((\w+)\): \+(\d+) exp, \+(\d+) discipline", ln) for ln in host]
rec = [m for m in rec if m]
last = rec[-1] if rec else None
check(bool(last) and int(last[3]) > 0, "host: the guest's pawn got the party's exp" + (f": pawn {last[1]} ({last[2]}) +{last[3]} exp, +{last[4]} discipline" if last else ""))
got = [re.search(r"pending in total \+(\d+) exp, \+(\d+) discipline", ln) for ln in guest if "progress: from the host" in ln]
got = [m for m in got if m]
check(bool(got) and last and got[-1][1] == last[3] and got[-1][2] == last[4],
      "guest: kept the same gains" + (f" (+{got[-1][1]} exp, +{got[-1][2]} discipline)" if got else ""))
added = [re.search(r"progress: added to (\w+)'s save: \+(\d+) exp, \+(\d+) discipline.*the save before: ([^)]+)\)", ln) for ln in solo]
added = [m for m in added if m]
check(bool(added) and got and added[0][2] == got[-1][1], "guest alone: the gains were added when its save was read" + (f" ({added[0][0][19:]})" if added else ""))
if added:
    backup = os.path.join(guest_dir, added[0][4])
    same = os.path.exists(backup) and hashlib.sha256(open(backup, 'rb').read()).digest() == hashlib.sha256(open(own_save, 'rb').read()).digest()
    check(same, f"backup {added[0][4]} is the save as it was")
else:
    check(False, "backup is the save as it was")
applied = [ln for ln in solo if "progress: the game saved with the gains" in ln]
check(bool(applied) and not os.path.exists(os.path.join(guest_dir, "coop_progress", "pending.txt")),
      "after the game's write, pending.txt became applied_<time>.txt")
written = os.path.join(guest_dir, "write_1.sav")
if added and os.path.exists(written):
    def exp(path):
        _, x = save_merge.unpack(path)
        top = x.find('<class name="mPlayerDataManual"')
        return int(re.search(r'<u32 name="mExp" value="(\d+)"', x[top:]).group(1))
    before, after = exp(own_save), exp(written)
    check(after - before == int(added[0][2]), f"the game's next save holds the exp: {before} -> {after}")
else:
    check(False, "the game's next save holds the exp")
