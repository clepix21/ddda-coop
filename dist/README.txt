DDDA Coop 0.9.4 (alpha) - online co-op for Dragon's Dogma: Dark Arisen (Steam, 2 to 4 players)
==============================================================================================

Each player needs the game on Steam (current version), on their own PC, and this same mod version
(0.9.4). Guests must be Steam friends with the host. No IP address or port to set.

Files: DDDA-Coop.exe (the launcher), dinput8.dll (the mod), coop.ini (settings). Keep them together,
anywhere. The launcher is in English, French, Italian, German and Spanish (globe button, bottom left).

Play
----
1. Open DDDA-Coop.exe. It finds the game; if not: "Settings" > "Change…", pick the folder with DDDA.exe.
2. In "Play", pick a role:
   - "Host": loads their game; friends join them.
   - "Guest": joins the host with their own character, which takes a pawn's place.
   - "Solo": your normal game; what you earned in co-op is added to it.
3. Click "Launch the game". Your saves are first copied to Documents\DDDA Coop\Sauvegardes.
4. Guest: stay on the title screen until the launcher says "Connected!".
   Then press Start and load a game: you arrive in the host's world.
Several hosts at once? The host copies the code ("Lobby" > "Copy the code"); the guest pastes it in
their "Lobby" before launching. Without a code, a guest joins the first friend who hosts.

In game
-------
- Help a downed player up: hold B next to them.
- Explore alone, or come back to the host: hold LB + RB + Y for 1.5 s (or F7).
- Grab a player or a small monster: RT.
- The Rift is closed during co-op. Avoid the game's online features.

After playing
-------------
- Guest: your own save is not loaded or saved. Experience, discipline and items your character
  earned wait in the game folder (coop_progress). To add them: "Solo", launch, load your game, save.
- "Remove the mod" restores the original game.
- A problem? "Log" > "Export the log", then send the file (Documents\DDDA Coop\Journaux).

Good to know
------------
- Alpha: keep copies of your saves.
- If the game folder already has another dinput8.dll (another mod), it is not replaced: remove it first.

What's new in 0.9.4
-------------------
- Automatic updates: at startup the launcher downloads a newer release, checks it (SHA-256),
  installs it and restarts. Your coop.ini is kept. Not while the game runs.
- Setting: Settings > "Update automatically at startup".
- Release files in English: README.txt and HOW-TO-TEST.txt.

Licence: MIT for the mod's own code; MinHook and Roboto: see LICENCES.txt.
Unofficial mod, not affiliated with Capcom. Dragon's Dogma: Dark Arisen (c) Capcom.
