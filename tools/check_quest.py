r"""Checks the shared world quest runs (suite_quest.ps1, owner's quest.sav: a quest giver in front):
run "host": the host takes the quest, the guest's world must get it (quest list), staying in place;
run "guest": the other way round; the host's next save holds it. Prints PASS/FAIL lines.
Usage: check_quest.py <dir with host/ and guest/>"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import save_merge  # noqa: E402

out = sys.argv[1]


def lines(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def check(ok, text):
    print(("PASS  " if ok else "FAIL  ") + text)


def quests(path):
    _, x = save_merge.unpack(path)
    i = x.find('<array name="mQuestData"')
    seg = x[i:i + 400000]
    ids = re.findall(r'name="mQuestID" value="(-?\d+)"', seg)
    return {int(a) for a in ids if a != '-1'}


NEW = 0x350000
for run, taker, other in (("host", "host", "guest"), ("guest", "guest", "host")):
    t, o = lines(f"{out}/{run}/{taker}.log"), lines(f"{out}/{run}/{other}.log")
    sent = [ln for ln in t if "quest state" in ln and "sent to the other players" in ln]
    check(bool(sent), f"{run} takes the quest: its quest state was sent")
    got = [ln for ln in o if "reloaded with the other player's quest state" in ln or "added live, no reload" in ln]
    live = [ln for ln in o if "quests: live add 00350000" in ln]
    check(bool(got), f"{run} takes the quest: the {other}'s world got it ({'live' if live else 'reload'})")
    snap = os.path.join(out, run, f"{other}_last.sav")
    check(os.path.exists(snap) and NEW in quests(snap), f"{run} takes the quest: the {other}'s next save holds quest {NEW:#x}")
    pos = [(m[1], m[2]) for ln in o for m in [re.search(r"shared: me at \((-?\d+), (-?\d+)\)", ln)] if m]
    check(len(set(pos[-6:])) == 1 and bool(pos), f"{run} takes the quest: the {other} stayed where it was ({pos[-1] if pos else None})")
