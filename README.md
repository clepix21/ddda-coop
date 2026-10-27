# ddda-pawn-coop

Co-op for **Dragon's Dogma: Dark Arisen** (PC, Steam), 2 to 4 players. The host plays the Arisen
as usual; up to three friends, each on their own PC with their own copy of the game, join over
Steam and each replaces one of the host's pawns (the hired pawns first, the main pawn last), with
their OWN character from their own save: look, name, vocation, level, equipment. Host-authoritative:
the host's world is the real one, the guests' games show it. A second controller on the same PC can
also drive the main pawn (local mode).

Status: **alpha, bot tests on one PC only** (up to three copies of the game). The network mode has
never run between two real PCs or two real Steam accounts. See "What works" and "Known issues". Not released.

## Supported game version

Only this build is supported; the mod disables itself on any other `DDDA.exe`:

| | |
|---|---|
| Store | Steam (appid 367500), build 2364871 |
| `DDDA.exe` SHA-256 | `19FACC2642F79A09D14CE55EACE5F228161F2F8BAB60EC67EB3C6196CC6277B8` |
| PE TimeDateStamp | `0x5A314C31` (checked at startup) |

## Install

1. Back up your saves (`%ProgramFiles(x86)%\Steam\userdata\<id>\367500\remote`).
2. Copy `dinput8.dll` and `coop.ini` next to `DDDA.exe`. Another mod that also uses
   `dinput8.dll` cannot be installed at the same time.
3. Start the game normally.

Uninstall: delete `dinput8.dll` and `coop.ini`.

## Controls

- Player 1: any controller in slot 1, or keyboard and mouse. Unchanged.
- Player 2: controller in XInput slot `p2_pad` (default 1 = second controller).
  - **Back + Start** on player 2's controller: take over the main pawn / give it back to its AI.
  - While possessed, the main pawn uses player 2's sticks and buttons with the normal
    gameplay layout (X light attack, Y heavy attack, A jump, ...), relative to the shared camera.
  - Player 2's Back and Start never reach the game (no pause menu from player 2), and player 2
    never moves the Arisen.

## coop.ini

```
p2_pad=1          # XInput slot of player 2 (0-3)
idle_pad=3        # a slot with no controller, used internally as "nothing pressed"
auto_possess=0    # ms after start to take over the pawn automatically (tests); 0 = off
block_saves=0     # 1 = block every Steam Cloud save write (tests only; you lose progress)
debug_log=0       # 1 = detailed coop.log
```

## Over Steam, 2 to 4 players (alpha)

Every player owns the game on Steam, is a Steam friend of the host, and installs the mod. In
`coop.ini`, the host sets `role=host` and each friend `role=guest` (keep `transport=steam`). Start
the game on every PC; a friend presses Start at the title once connected (a few seconds) and loads
("Charger"). The host may already be playing. No address or port is needed:

- each side sets a Steam rich presence key `ddda_coop` (host or guest), visible to friends;
- the guest finds a friend in Dragon's Dogma whose key says host, and connects through Steam P2P
  (Valve's relay, no port forwarding);
- the host accepts friends that are in the game, up to three; guest 1 takes hired pawn 1, guest 2
  hired pawn 2, guest 3 the main pawn (`pawn_order=` changes that). If a guest goes silent for
  10 s its pawn returns to its AI, and it rejoins on its own.
- each guest's own character (from its own save) replaces that pawn in the host's save, which the
  host and every guest load; the host's own saves keep its real pawns (they are put back before
  every write). A friend who connects while the host plays: the host's game saves, loads the
  merged save and rebuilds its world (about 4 s, like a reload), and friends already in game reload
  with it. `send_character=0` on a friend's PC: that friend plays the host's pawn as it is.

What is synchronised: the Arisen and the three pawns (position, facing, actions and their
animations through the command object, HP). Each guest's camera follows its own pawn
(`guest_cam_pawn=1`), and its stick is relative to its own camera. Enemies
(alpha), decided by the host: each guest enemy is paired with the host's enemy of the same class
that spawned at the same point, follows its position, facing and HP, and dies when it dies (by the
guest's replayed killing blow, or the game's own death handler if none lands within 1.5 s). The
guest's own hits on enemies the host says are alive do nothing, nor do hits on the guest's Arisen
and pawn, whose HP is the host's. An enemy only the guest's game spawned is removed after 10 s.
**Not synchronised yet:** enemies the host has and the guest's game did not load (the host cannot
spawn them there), enemy actions and animations, other pawns, NPCs, items, quests, time of day.

Time: the guest's clock follows the host's (an inn rest jumps both to 6:00; talks stop both).

Talks: when the host talks to someone, the same talk starts on each guest's game (the host's B
reaches the guest's copy of the host's Arisen), and the host's controller drives the lines and menus
there too, so guests read along. A guest that lags a step is brought out after the host's talk ends.
A guest's choices only change its own copy of the world (it never saves). Each guest's camera frames
the Arisen during talks and events.

World: the guest's game loads the host's save, sent over the network when the guest connects (and
again after each host save, autosaves included). On the guest, "Charger" at the title starts the
host's world: the host's Arisen and pawns, quests, items, time and place. The guest must be connected
before pressing Start at the title (the game reads the save list then). The guest's own save is
never read for loading nor written while `role=guest`.

Areas: when the host goes to another area (a gate, a door), the guest's game goes to the same area
and entry point. When the host reloads (retry after death, last save, a game loaded from the title)
the guest's game reloads the same area and entry point. A guest that joins late, or ends up
elsewhere, is sent to the host's area after 10 s; while the host is in the title menus, the guest
waits. Saves: only the host's game saves. A guest's game never saves while `role=guest` (its
autosaves would write the host's world into the guest's own save); to play their own save
normally, the guest removes `role=guest` first.

Options: `peer_steamid=<SteamID64>` to pick the other player instead of searching the friends
list; `transport=udp` with `peer=<address>` and `port=` (default 27101) for a LAN.

## How it works

Every human in the game (`uPlayerBase`: the Arisen `uPlayer`, pawns `uCmc`, NPCs) has a command
object filled each frame by one function (`0x00B6AEA0`). A mode byte says whether it reads the pad
or takes commands from the AI, and the game merges all controllers and the keyboard into the one
pad-driven character. The mod hooks that function and, for the main pawn (`mCmcId == 0`) while
possessed, switches it to pad mode and lets it see only player 2's controller; the Arisen never
sees player 2's controller. Characters update on several threads at once, so the shared input
objects are never edited: the 50 instructions that load them in the command builder are patched
to read a per-thread pointer (TLS) that points at a private copy, falling back to the real object
on every other thread. Details and evidence: the write-up in the coop-anything hub,
`games/dragons-dogma-dark-arisen/NOTES.md`.

## What works (bot tests on the supported build)

- Player 2's left stick moves the main pawn, camera-relative; the Arisen does not move.
- Player 2's X / Y / A reach the pawn as light attack / heavy attack / jump (seen in screenshots).
- Keyboard input reaches the Arisen and not the possessed pawn.
- Back + Start toggles possession; with it off, the pawn returns to its AI after a few seconds.
- 4 minutes of random input without a crash.

Two-copy tests on one PC (`tools\suite_pair.ps1`, `tools\suite_reconnect.ps1`), over UDP and
over the Steam code path with a test stand-in for Steam (`steam_mock.cpp`):

- the guest connects, takes over the pawn, and the Arisen and pawn follow the host on the guest
  (median error 0.0 to 0.1 u), also with 10 % packet loss and 80 ms latency;
- the pawn moves along the guest's camera, not the host's;
- the guest's game killed and restarted: the host drops it, gives the pawn back to its AI, and
  accepts it again; the pawn is placed correctly after the rejoin;
- three players (`tools\suite_three.ps1`, three copies): guest 1 gets hired pawn 1 and guest 2 hired
  pawn 2; each pawn moves along its own guest's camera, and each guest sees the other's pawn where
  the host has it (median error 0.00 to 0.04 u) (12/12 over UDP and the Steam stand-in);
- each player's own character (`tools\suite_party.ps1`, three copies): both guests' characters
  reach the host and replace hired pawns 1 and 2 in the save the host and both guests load (on all
  three, those pawns have the guests' characters' max HP); the host's autosave puts its own pawns
  back (14/14 over UDP);
- areas (`tools\suite_stage.ps1`): the host goes through the iron gate from Gran Soren (stage 220)
  to the underground (600); the guest's game follows 2.4 s later, its Arisen then follows the
  host's (median error 0.0 u), and its area-change autosave is blocked by its role alone (it runs
  with `block_saves=0`). Late join: a guest connecting after the host's jump is sent to the host's
  area 13.5 s after connecting. The host dies and picks "Réessayer", "Dernier point de sauvegarde",
  or returns to the title and loads the game: each time the guest reloads with the host (0.0 to
  1.0 s later) and its Arisen follows again (14/14 for each, over UDP and the Steam stand-in for
  retry and title);
- the host's save (`tools\suite_save.ps1`): the host goes through the iron gate (its autosave
  there, blocked on disk in tests, is kept in memory and sent, 512 KB in about 1 s); the guest,
  pressing Start only afterwards, loads straight into the host's place (stage 600; the guest
  copy's own save is in Gran Soren) (7/7 over UDP and over the Steam stand-in);
- inn (`tools\suite_inn.ps1`, a save in front of the innkeeper): the host talks and rests; the guest's
  clock jumps to the host's 6:00 and stays within 1 game second, its Arisen follows into the room
  with the host's HP; the guest's game goes through the same talk (started 0.03 s after the host's,
  ended with it or after a 1 s catch-up) (11/11, three runs, over UDP and the Steam stand-in);
- join during play (`tools\suite_join.ps1`): a friend's game starts 140 s after the host's; its
  character reaches the host while the host plays; the host saves, loads the merged save and
  rebuilds its world in about 4 s, after which pawn 1 has the friend's character on both PCs
  (11/11 over UDP and the Steam stand-in). With three copies (`-Three`), the friend already in game
  reloads too and all three show both friends' characters (14/14);
- enemies (`tools\suite_enemies.ps1`): both copies walk to the same underground area, the host's
  party kills 6 small creatures; on the guest all 8 enemies are paired (spawn points 0.0 u apart),
  follow the host, end where the host's lie with the same HP and death, the 6 deaths happen on the
  guest too (1 to 5 of them forced), the guest's own hits on living enemies are dropped, and the
  guest's Arisen and pawn keep the host's HP (11/11 over UDP and over the Steam stand-in).

## Known issues / not tested

- Host saves with `block_saves=0` (normal play) go through the mod's storage hook to Steam Cloud as
  before; that path has not been tested (tests never let a save reach the owner's Steam Cloud).
- Each player's character: its progress as a pawn (exp, items) is not saved anywhere; vocations
  only the Arisen can have (e.g. Mystic Knight) are not checked on a pawn; the save owner check is
  patched on guests (the host's save belongs to another account), untested with two accounts; the
  tests used the same character for both guests.
- The real Steam path has only been checked to start (Steam interfaces found, logged on); no
  packet has gone between two real Steam accounts.

- Enemies: each game still loads and unloads its own. Seen once: the guest's game unloaded 2 far
  zombies the host kept (they stay missing on the guest) and loaded 3 creatures the host did not
  have (removed after 10 s). Enemy attacks and animations are the guest game's own; position,
  facing, HP and death come from the host. A forced death has no hit behind it, so no knock-back.
  The removal of guest-only enemies and the pairing on current position were only exercised
  before the last fixes (those runs had no such enemies). Bosses and large monsters are not tested.
- Not tested: skills (RT/LT combos), grab/climb, items, whether the
  pawn AI interferes during fights, fast travel, "mode facile" retries, cutscenes on the guest, the Everfall, long sessions, the leash that may
  pull a far-away pawn back.
- Talks: a guest may talk to another NPC than the host's when the NPCs do not stand at the same
  place on both PCs (seen once at the inn); cutscenes are not tested.
- Join during play forces a save on the host (the same save the game makes at a gate), then a
  reload: the host loses nothing, but the world restarts around the party (enemies, NPC places).
- Player 2's face buttons can still act in menus while player 1 is in a menu.
- One shared camera that follows the Arisen.
- Only the main pawn can be possessed.

## Build

Windows, [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20260922 and
[MinHook](https://github.com/TsudaKageyu/minhook) v1.3.4 source:

```
.\build.ps1 -Toolchain <llvm-mingw dir> -MinHook <minhook dir>
```

Output: `build\dinput8.dll` (32-bit).

## Tests

`tools\run_test.ps1` runs a copy of the game with a scripted bot (`tests\*.txt`), screenshots,
and a check that the save folder is byte-identical afterwards (tests force `block_saves=1`).
`tools\suite.ps1` runs the scenarios and asserts on the probe log with `tools\analyze_log.py`.
Never point the tests at your real install.

## License

Not chosen yet. MinHook is BSD-2-Clause (its license applies to the bundled code).
