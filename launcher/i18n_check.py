"""Checks that every key of launcher/i18n_table.py is still a text of launcher.cpp (a key that drifted from
its source would leave that text in French). Usage: python -I launcher/i18n_check.py"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # run with -I
from i18n_gen import key  # noqa: E402
from i18n_table import T  # noqa: E402

# used through tables made before the language is known (kPageNames, kRoles), or built at run time
INDIRECT = {'Jouer', 'Salon', 'Sauvegardes', 'Paramètres', 'Journal', 'Hôte', 'Invité', 'Seul',
            'Je charge ma partie ; mes amis me rejoignent.', "Je rejoins un ami : ma partie n'est ni chargée ni sauvegardée.",
            'Je joue ma partie ; la progression gagnée en co-op y est ajoutée.'}


def norm(t):
    return re.sub(r'\\u([0-9A-Fa-f]{4})', lambda m: chr(int(m.group(1), 16)), t)


src = open(os.path.join(HERE, 'launcher.cpp'), encoding='utf-8').read()
texts = {norm(m.group(1)) for m in re.finditer(r'L"((?:[^"\\]|\\.)*)"', src)}
missing = [key(row[0]) for row in T if key(row[0]) not in texts and key(row[0]) not in INDIRECT]
sys.stdout.reconfigure(encoding='utf-8')
for k in missing:
    print('not in launcher.cpp:', k[:90])
print(f'{len(T)} keys, {len(missing)} not found')
sys.exit(1 if missing else 0)
