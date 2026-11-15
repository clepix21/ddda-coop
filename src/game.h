#pragma once
// Addresses and layouts for DDDA.exe. They are written as in the Steam build 2364871 (TimeDateStamp
// 0x5A314C31); va() turns one into the running build's: the same for Steam, through addr_gog.h for
// GOG's build (TimeDateStamp 0x5936ABAB, 2017-06-06), made by pairing both exes' functions in Ghidra.
// A field that moved in GOG's build is picked with by_build(steam, gog).
// Source: games/dragons-dogma-dark-arisen/NOTES.md in the coop-anything hub.
#include <cstdint>

#include "addr_gog.h"

namespace game {

enum Build { BUILD_UNKNOWN = 0, BUILD_STEAM = 1, BUILD_GOG = 2 };
constexpr uint32_t kStampSteam = 0x5A314C31, kStampGog = 0x5936ABAB;
Build build();                 // from DDDA.exe's TimeDateStamp, read once
const char *build_name();      // "Steam", "GOG" or "unknown"
uintptr_t va(uintptr_t steam); // a Steam address in this build (Steam's own when not in the GOG table: logged once)
inline uintptr_t by_build(uintptr_t steam, uintptr_t gog) { return build() == BUILD_GOG ? gog : steam; }

// Functions
inline const uintptr_t kUpdateCommands = game::va(0x00B6AEA0); // void __fastcall(uPlayerBase *), vtable slot 200

// Globals (pointers to singletons)
inline const uintptr_t kInput = game::va(0x018D8BB0);    // sQlocInput *
inline const uintptr_t kKeyboard = game::va(0x018D0FAC); // sKeyboard *
inline const uintptr_t kMouse = game::va(0x018D103C);    // mouse-like input *

// Class vtables (identify a character's concrete class)
inline const uintptr_t kVtPlayer = game::va(0x015E90D0); // uPlayer (Arisen)
inline const uintptr_t kVtCmc = game::va(0x015E8468);    // uCmc (pawn)
inline const uintptr_t kVtPlayerBase = game::va(0x015EA430);
inline const uintptr_t kVtNpc = game::va(0x015F1AE0);
inline const uintptr_t kEnemyDeath = game::va(0x00AAB3B0); // uEnemy death handler (vtable slot 0x120), thiscall(ch, a, hit)
// Stages (areas): sArea singleton (sAreaExt), its current area object at +0x3834 is an aStage<NNN>.
// aStage +0x724 stage number; +0x808 1 = playing, 2 = jump requested; +0x80C / +0x810 target stage
// and entry point; +4 state (0x13 = jump). FUN_00401160 (the request the gates use) writes those;
// the stage's own state machine (FUN_0050EF80) then fades and calls FUN_004FBD50, which creates
// "aStage%03d" and stores the stage / entry point in [0x018FA4BC]+0x34 / +0x44.
inline const uintptr_t kArea = game::va(0x018D099C);
constexpr uintptr_t kAreaCurrent = 0x3834;
inline const uintptr_t kGameMain = game::va(0x018FA4BC);
// sGameSys ([0x018FA4BC], size 0xBE470) fields found by dumps around a talk and an inn rest
// (2026-10-26): +0x9E3A8 event state (256 normal, 7 during a talk), +0xB8760 day, +0xB876C hour,
// +0xB8770 minute, +0xB8774 second, +0xB877C time speed (500, 0 during a talk). The clock itself is
// +0xB8768, milliseconds into the day (39105015 = 10:51:45.015, 21600000 = 6:00:00 after an inn rest);
// hour / minute / second are derived from it every frame (writing them alone does not stick).
constexpr uintptr_t kGsEvent = 0x9E3A8, kGsDay = 0xB8760, kGsDayMs = 0xB8768, kGsHour = 0xB876C,
                    kGsMinute = 0xB8770, kGsSecond = 0xB8774, kGsTimeSpeed = 0xB877C;
inline const uintptr_t kStageLoad = game::va(0x004FBD50); // EAX = {stage, point, flag}, ESI = area stack object
inline const uintptr_t kApplyDamage = game::va(0x00776F50);     // HP entry in EAX, (float damage, char flag), ret 8
inline const uintptr_t kEnemyFinalizeRet = game::va(0x00AA9A08); // return address of the FUN_0084BF60 call in uEnemy slot 91

// uPlayerBase layout
constexpr uintptr_t kCmd = 0x32B4;    // embedded command object
constexpr uintptr_t kCmdMode = 0x60;  // byte, 1 = pad-driven
constexpr uintptr_t kCmdHeld = 0x30;  // uint32 held bits
constexpr uintptr_t kCmdPress = 0x2C; // uint32 pressed-this-frame bits

// uCmc layout
constexpr uintptr_t kCmcId = 0x5854;   // int
constexpr uintptr_t kCmcPlayer = 0x585C; // uPlayer *

// sQlocInput layout
inline const uintptr_t kPadInfo = by_build(0x1774, 0x1764); // cPadInfo[4] (GOG: the fields after +0x1080 are 0x10 lower)
constexpr uintptr_t kPadInfoSize = 0x2E8;

template <class T> inline T &at(uintptr_t base, uintptr_t off) { return *reinterpret_cast<T *>(base + off); }

} // namespace game
