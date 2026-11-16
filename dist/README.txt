DDDA Coop 0.10.0 (alpha) - online co-op for Dragon's Dogma: Dark Arisen (Steam or GOG, 2 to 4 players)
=======================================================================================================

Each player needs the game (Steam or GOG, current version) on their own PC, and this same mod
version (0.10.0).
- Steam: guests must be Steam friends with the host. No IP address or port to set.
- GOG: players join by IP address (direct IP, UDP port 27101). See "GOG and direct IP" below.

Files: DDDA-Coop.exe (the launcher), dinput8.dll (the mod), coop.ini (settings). Keep them together,
anywhere. The launcher is in English, French, Italian, German and Spanish (globe button, bottom left).

Play
----
1. Open DDDA-Coop.exe. It finds the game (Steam or GOG). Both installed? "Settings" > "Game version":
   Steam or GOG. Not found: "Settings" > "Change…", pick the folder with DDDA.exe.
2. In "Play", pick a role:
   - "Host": loads their game; friends join them.
   - "Guest": joins the host with their own character, which takes a pawn's place.
   - "Solo": your normal game; what you earned in co-op is added to it.
3. Click "Launch the game". Your saves are first copied to Documents\DDDA Coop\Sauvegardes.
4. Guest: stay on the title screen until the launcher says "Connected!".
   Then press Start and load a game: you arrive in the host's world.
Several hosts at once on Steam? The host copies the code ("Lobby" > "Copy the code"); the guest pastes
it in their "Lobby" before launching. Without a code, a guest joins the first friend who hosts.

GOG and direct IP
-----------------
The GOG version has no Steam, so players join by address. "Play without Steam" (Settings) is always
on for GOG. Steam players can turn it on too, to play with a GOG friend or on a local network.
1. Host: launch the game. "Lobby" shows your addresses: "Copy the address" and send it to your friends.
2. Guest: "Guest", paste that address in "Lobby" (for example 192.168.1.20), then launch the game.
3. Same network (same router): nothing else to do.
   Over the internet: the host forwards UDP port 27101 on their router to their PC, or you all use a
   VPN that makes a local network, and the address it gives.
GOG only:
- Keep GOG Galaxy open and signed in: without it the game may stay on "Accessing server...".
- When you load a game, the GOG game may show "Connection failed" (GOG's pawn server): press A or
  Enter, the game loads.
- Your GOG Galaxy saves are copied at each launch to Documents\DDDA Coop\Sauvegardes\<date>\GOG.

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

What's new in 0.10.0
--------------------
- The GOG version of the game is supported (the mod, the launcher and the backup of GOG Galaxy saves).
- "Play without Steam": local network or direct IP (UDP port 27101), for GOG, and for Steam players
  who want it. The host's addresses are in "Lobby", the guest pastes one there.
- "Settings" > "Game version": Steam or GOG, and "How the GOG version works".
- The launcher tells each step over direct IP: host ready, guest looking for the host, host found,
  no answer (with what to check), port already in use.
- An unsupported DDDA.exe: the launcher says which versions the mod needs, with the exe's fingerprint.
- The previous game's log is kept (coop.prev.log) and exported with the log: a crash is no longer
  lost when the game is started again.
- The Lobby no longer lists virtual machine network cards (VirtualBox, VMware, Hyper-V).

Licence: MIT for the mod's own code; MinHook and Roboto: see LICENCES.txt.
Unofficial mod, not affiliated with Capcom. Dragon's Dogma: Dark Arisen (c) Capcom.
