"""Writes the English docs of a release zip: README.txt and HOW-TO-TEST.txt.
Usage: python -I tools/release_docs.py <version> <out dir>
Versions before 0.9.2 have a French-only launcher: their labels are given in French with the English meaning."""
import os
import sys

NEWS = {
    '0.9.0': ['New launcher window: Play, Lobby, Saves, Settings and Log screens.',
              'Lobby: every player with their Steam name, character, area and ping; a connection code to join one host.'],
    '0.9.1': ['Fixed a crash when a player grabs another player who is far away.',
              'No more false "older version" warning when the host says hello.'],
    '0.9.2': ['Other players move smoothly on an uneven connection.',
              'Guests see their hits on monsters at once (the host still decides HP and deaths).',
              'Launcher in English, French, Italian, German and Spanish; update check; new font and icon.',
              'Co-op starts only after the tutorial, so a new game cannot break a session.',
              'Bows: a player\'s pawn aims and shoots like its player.',
              'Deep water: back on the shore at once with a little less HP, instead of dying.'],
    '0.9.3': ['A player\'s lit lantern is seen by the others.',
              'Gear changes are seen by the others; the game loads the piece first.',
              'Fixed: a lantern already lit when a friend joins is now shown.'],
    '0.9.4': ['Automatic updates: at startup the launcher downloads a newer release, checks it (SHA-256),\n'
              '  installs it and restarts. Your coop.ini is kept. Not while the game runs.',
              'Setting: Settings > "Update automatically at startup".',
              'Release files in English: README.txt and HOW-TO-TEST.txt.'],
    '0.9.5': ['Fixed: new players got the old classic mode (a guest driving one of the host\'s pawns) instead of\n'
              '  the shared world: no quest or gear sync, menus driven by the guest\'s pad (issue #1). The shared\n'
              '  world is now the default; shared_world=0 in coop.ini gives the classic mode.',
              'Fixed: a grabbed player (RT from behind) was thrown to the far edge of the map when let go, or\n'
              '  fell through the floor, and the next grab missed (issue #1). Grabs work both ways.',
              'Checked: gear bought from a merchant and worn is seen by the others.',
              'The mod writes its version at the top of its log.'],
}

EN = {'play': '"Play"', 'lobby': '"Lobby"', 'settings': '"Settings"', 'log': '"Log"', 'host': '"Host"', 'guest': '"Guest"',
      'solo': '"Solo"', 'launch': '"Launch the game"', 'change': '"Change…"', 'copy': '"Copy the code"',
      'export': '"Export the log"', 'remove': '"Remove the mod"', 'detailed': '"Detailed log"',
      'connected': '"Connected!"', 'supported': '"Game version supported"'}
FR = {'play': '"Jouer" (Play)', 'lobby': '"Salon" (Lobby)', 'settings': '"Paramètres" (Settings)', 'log': '"Journal" (Log)',
      'host': '"Hôte" (Host)', 'guest': '"Invité" (Guest)', 'solo': '"Seul" (Solo)', 'launch': '"Lancer le jeu" (Launch)',
      'change': '"Changer…" (Change)', 'copy': '"Copier le code" (Copy the code)',
      'export': '"Exporter le journal" (Export the log)', 'remove': '"Retirer le mod" (Remove the mod)',
      'detailed': '"Journal détaillé" (Detailed log)', 'connected': '"Connecté !" (Connected!)',
      'supported': '"Version du jeu prise en charge" (Game version supported)'}


def readme(v, L, licences):
    news = '\n'.join('- ' + n for n in NEWS[v])
    lang = ('The launcher is in English, French, Italian, German and Spanish (globe button, bottom left).'
            if L is EN else 'This version\'s launcher is in French only; its labels are given below with their meaning.')
    lic = ('Licence: MIT for the mod\'s own code; MinHook and Roboto: see LICENCES.txt.' if licences
           else 'MinHook (BSD 2-Clause licence) is built into dinput8.dll.')
    title = f"DDDA Coop {v} (alpha) - online co-op for Dragon's Dogma: Dark Arisen (Steam, 2 to 4 players)"
    return f'''{title}
{'=' * len(title)}

Each player needs the game on Steam (current version), on their own PC, and this same mod version
({v}). Guests must be Steam friends with the host. No IP address or port to set.

Files: DDDA-Coop.exe (the launcher), dinput8.dll (the mod), coop.ini (settings). Keep them together,
anywhere. {lang}

Play
----
1. Open DDDA-Coop.exe. It finds the game; if not: {L['settings']} > {L['change']}, pick the folder with DDDA.exe.
2. In {L['play']}, pick a role:
   - {L['host']}: loads their game; friends join them.
   - {L['guest']}: joins the host with their own character, which takes a pawn's place.
   - {L['solo']}: your normal game; what you earned in co-op is added to it.
3. Click {L['launch']}. Your saves are first copied to Documents\\DDDA Coop\\Sauvegardes.
4. Guest: stay on the title screen until the launcher says {L['connected']}.
   Then press Start and load a game: you arrive in the host's world.
Several hosts at once? The host copies the code ({L['lobby']} > {L['copy']}); the guest pastes it in
their {L['lobby']} before launching. Without a code, a guest joins the first friend who hosts.

In game
-------
- Help a downed player up: hold B next to them.
- Explore alone, or come back to the host: hold LB + RB + Y for 1.5 s (or F7).
- Grab a player or a small monster: RT.
- The Rift is closed during co-op. Avoid the game's online features.

After playing
-------------
- Guest: your own save is not loaded or saved. Experience, discipline and items your character
  earned wait in the game folder (coop_progress). To add them: {L['solo']}, launch, load your game, save.
- {L['remove']} restores the original game.
- A problem? {L['log']} > {L['export']}, then send the file (Documents\\DDDA Coop\\Journaux).

Good to know
------------
- Alpha: keep copies of your saves.
- If the game folder already has another dinput8.dll (another mod), it is not replaced: remove it first.

What's new in {v}
{'-' * (14 + len(v))}
{news}

{lic}
Unofficial mod, not affiliated with Capcom. Dragon's Dogma: Dark Arisen (c) Capcom.
'''


def how_to_test(v, L):
    gear = '\n- Light the lantern; change a piece of gear in the menu.' if v >= '0.9.3' else ''
    title = f'How to test DDDA Coop {v} with friends'
    return f'''{title}
{'=' * len(title)}

Before
------
- Everyone: the game up to date on Steam, and Steam friends with the host.
- Everyone: unzip, open DDDA-Coop.exe. It must say {L['supported']}.
  Windows may warn that the program is not signed: "More info", then "Run anyway".
- Keep {L['detailed']} on ({L['settings']}).

Start, in this order
--------------------
1. Host: {L['host']}, {L['launch']}, load their game.
2. Each friend: {L['guest']}, {L['launch']}, and STAY on the title screen until {L['connected']}.
3. Only then: Start, load a game. The friend arrives with their own character; the host's game
   saves and reloads for a few seconds.
One friend at a time is easier to follow if something goes wrong.

Try
---
- Walk and fight together; go through a door or a gate.
- Host talks to an NPC, sleeps at the inn.
- A friend quits and comes back.
- A friend explores alone (LB + RB + Y for 1.5 s, or F7) and comes back the same way.{gear}
- Avoid the Rift and the game's online features.

If something goes wrong
-----------------------
- No Steam friend found in the game: be Steam friends, and in Dragon's Dogma.
- "Host not found" after a minute: restart the friend's game.
- A crash or anything odd: note the time and what you were doing.

After
-----
- Everyone: {L['log']} > {L['export']} BEFORE restarting the game (it clears the log), and send
  the file from Documents\\DDDA Coop\\Journaux, saying who you are.
- Friends, back to your own game: {L['solo']} (your co-op gains are added) or {L['remove']}.
'''


def main():
    v, out = sys.argv[1], sys.argv[2]
    labels = EN if v >= '0.9.2' else FR
    os.makedirs(out, exist_ok=True)
    licences = v >= '0.9.2'
    for name, text in (('README.txt', readme(v, labels, licences)), ('HOW-TO-TEST.txt', how_to_test(v, labels))):
        with open(os.path.join(out, name), 'w', encoding='utf-8-sig', newline='\r\n') as f:
            f.write(text)


if __name__ == '__main__':
    main()
