# Porting the mod's addresses to another DDDA.exe build

How `src/addr_gog.h` was made (GOG's build, TimeDateStamp 0x5936ABAB), and how to redo it for another build.

1. Analyse both exes in Ghidra (headless import, default analysis).
2. Export every function of each: `analyzeHeadless <project> <name> -process DDDA.exe -noanalysis -readOnly
   -scriptPath tools/gog -postScript ExportFuncs.java` with `GHIDRA_OUT=funcs_<build>.txt`.
3. `python -I match_funcs.py funcs_steam.txt funcs_gog.txt .` pairs the functions (same instructions, then
   same shape, then address order between pairs) and writes `insn_map.tsv`, `ref_map.tsv` (globals, vtables,
   import slots), `scalar_diff.tsv` and `funcs_map.tsv`.
4. `python -I match.py <steam exe> <gog exe> src map.tsv`: unique byte patterns (absolute addresses and
   relative branches masked), a second source for code addresses.
5. `python -I scalar_stats.py funcs_steam.txt funcs_gog.txt funcs_map.tsv src`: the scalars the mod uses that
   changed in paired code (moved fields). Each one is checked by hand and picked with `game::by_build()`.
6. `python -I list_addrs.py src used.txt`, then `python -I make_table.py used.txt src/addr_gog.h`. Addresses it
   cannot place are listed; those were placed by hand (see `MANUAL` in the script).
7. `wrap_addrs.py` wrapped every address of the code in `game::va()` once; new code writes `game::va(0x...)`
   with the Steam address.

What differed in GOG's build (2026-11-15): sQlocInput's fields from +0x1084 are 0x10 lower, the system
object's +0x2566 is +0x2565, the save owner check (Galaxy id), the saves (GOG Galaxy storage through the
game's `GA::RemoteStorage` wrappers, hooked in `save_guard.cpp`), no Steam (LAN / direct IP only).
