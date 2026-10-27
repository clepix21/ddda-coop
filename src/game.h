#pragma once
// Addresses and layouts for DDDA.exe Steam build 2364871 (TimeDateStamp 0x5A314C31) only.
// Source: games/dragons-dogma-dark-arisen/NOTES.md in the coop-anything hub.
#include <cstdint>

namespace game {

// Functions
constexpr uintptr_t kUpdateCommands = 0x00B6AEA0; // void __fastcall(uPlayerBase *), vtable slot 200

// Globals (pointers to singletons)
constexpr uintptr_t kInput = 0x018D8BB0;    // sQlocInput *
constexpr uintptr_t kKeyboard = 0x018D0FAC; // sKeyboard *
constexpr uintptr_t kMouse = 0x018D103C;    // mouse-like input *

// Class vtables (identify a character's concrete class)
constexpr uintptr_t kVtPlayer = 0x015E90D0; // uPlayer (Arisen)
constexpr uintptr_t kVtCmc = 0x015E8468;    // uCmc (pawn)
constexpr uintptr_t kVtPlayerBase = 0x015EA430;
constexpr uintptr_t kVtNpc = 0x015F1AE0;
constexpr uintptr_t kEnemyDeath = 0x00AAB3B0; // uEnemy death handler (vtable slot 0x120), thiscall(ch, a, hit)
// Stages (areas): sArea singleton (sAreaExt), its current area object at +0x3834 is an aStage<NNN>.
// aStage +0x724 stage number; +0x808 1 = playing, 2 = jump requested; +0x80C / +0x810 target stage
// and entry point; +4 state (0x13 = jump). FUN_00401160 (the request the gates use) writes those;
// the stage's own state machine (FUN_0050EF80) then fades and calls FUN_004FBD50, which creates
// "aStage%03d" and stores the stage / entry point in [0x018FA4BC]+0x34 / +0x44.
constexpr uintptr_t kArea = 0x018D099C;
constexpr uintptr_t kAreaCurrent = 0x3834;
constexpr uintptr_t kGameMain = 0x018FA4BC;
// sGameSys ([0x018FA4BC], size 0xBE470) fields found by dumps around a talk and an inn rest
// (2026-10-26): +0x9E3A8 event state (256 normal, 7 during a talk), +0xB8760 day, +0xB876C hour,
// +0xB8770 minute, +0xB8774 second, +0xB877C time speed (500, 0 during a talk). The clock itself is
// +0xB8768, milliseconds into the day (39105015 = 10:51:45.015, 21600000 = 6:00:00 after an inn rest);
// hour / minute / second are derived from it every frame (writing them alone does not stick).
constexpr uintptr_t kGsEvent = 0x9E3A8, kGsDay = 0xB8760, kGsDayMs = 0xB8768, kGsHour = 0xB876C,
                    kGsMinute = 0xB8770, kGsSecond = 0xB8774, kGsTimeSpeed = 0xB877C;
constexpr uintptr_t kStageLoad = 0x004FBD50; // EAX = {stage, point, flag}, ESI = area stack object
constexpr uintptr_t kApplyDamage = 0x00776F50;     // HP entry in EAX, (float damage, char flag), ret 8
constexpr uintptr_t kEnemyFinalizeRet = 0x00AA9A08; // return address of the FUN_0084BF60 call in uEnemy slot 91

// uPlayerBase layout
constexpr uintptr_t kCmd = 0x32B4;    // embedded command object
constexpr uintptr_t kCmdMode = 0x60;  // byte, 1 = pad-driven
constexpr uintptr_t kCmdHeld = 0x30;  // uint32 held bits
constexpr uintptr_t kCmdPress = 0x2C; // uint32 pressed-this-frame bits

// uCmc layout
constexpr uintptr_t kCmcId = 0x5854;   // int
constexpr uintptr_t kCmcPlayer = 0x585C; // uPlayer *

// sQlocInput layout
constexpr uintptr_t kPadInfo = 0x1774; // cPadInfo[4]
constexpr uintptr_t kPadInfoSize = 0x2E8;

template <class T> inline T &at(uintptr_t base, uintptr_t off) { return *reinterpret_cast<T *>(base + off); }

} // namespace game
