// Character actions (study 2026-10-28). Every human (uPlayerBase: the Arisen, pawns) runs one action
// object at a time, [ch+0x2DC8] (also +0x2DE8), rebuilt in place when the action changes (seen going
// cPlActWait -> cPlActRun at the same address while walking). The actions are numbered by the table
// at 0x01866C48 (0x44-byte entries, the action's MtDTI first: 0 cPlActWait, 1 cPlActWalk, 2 cPlActRun,
// 0x80 cPlActCmcNeardeath, 0xA4 cPlActLifted, 0xA9 cPlActHagaijime). vtable slot 0x210
// (FUN_00B540B0, __thiscall(ch, number), `ret 4`) changes the action; FUN_00B60870 does it when a
// character is restored, and for 0x80 also sets ch+0x273C |= 0x20 and ch+0x31EA = 1.
#include <windows.h>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {
volatile LONG g_req = 0; // tests: (who + 1) << 16 | number, who 0 = the Arisen, 1..3 = pawn 0..2
}

// The class name of a character's current action ("?" when unknown).
const char *action_name(uintptr_t ch)
{
    uintptr_t act = ch ? game::at<uintptr_t>(ch, 0x2DC8) : 0;
    if (!act || !mem_readable(reinterpret_cast<void *>(act), 4))
        return "?";
    uintptr_t vt = game::at<uintptr_t>(act, 0);
    if (!vt || !mem_readable(reinterpret_cast<void *>(vt), 0x14))
        return "?";
    using GetDti_t = uintptr_t(__fastcall *)(uintptr_t);
    uintptr_t dti = reinterpret_cast<GetDti_t>(game::at<uintptr_t>(vt, 0x10))(act);
    const char *name = dti ? game::at<const char *>(dti, 4) : nullptr;
    return name && mem_readable(name, 1) ? name : "?";
}

// Game thread: the action change the way the game makes it.
void action_set(uintptr_t ch, int number)
{
    using SetAction_t = void(__thiscall *)(uintptr_t, int);
    auto set = reinterpret_cast<SetAction_t>(game::at<uintptr_t>(game::at<uintptr_t>(ch, 0), 0x210));
    const char *before = action_name(ch);
    set(ch, number);
    logf("actions: %p action %s -> %s (number 0x%X)", (void *)ch, before, action_name(ch), unsigned(number));
}

// Up from the near-death action (0x80): the flags the game sets with it (FUN_00B60870: +0x273C |= 0x20,
// +0x31EA = 1) go too, else the skull stayed over a pawn standing again (revive suite shot, 230 s).
namespace {
uintptr_t hp_entry(uintptr_t ch)
{
    uintptr_t h = game::at<uintptr_t>(ch, 0x1D18);
    return h ? game::at<uintptr_t>(h, 0) : 0;
}
struct Before {
    uintptr_t ch;
    uint32_t flags; // HP entry +0x110 before the near-death action
};
Before g_before[8] = {};
} // namespace

// Into the near-death action (0x80), remembering the HP entry's flags first.
void action_lie_down(uintptr_t ch)
{
    uintptr_t e = hp_entry(ch);
    Before *slot = nullptr;
    for (Before &b : g_before)
        if (b.ch == ch || (!slot && !b.ch))
            slot = &b;
    if (slot && slot->ch != ch && e) {
        *slot = Before{ch, game::at<uint32_t>(e, 0x110)};
        logf("actions: %p lies down, HP entry flags %08X", (void *)ch, slot->flags);
    }
    action_set(ch, 0x80);
}

void action_stand_up(uintptr_t ch)
{
    uintptr_t e = hp_entry(ch);
    for (Before &b : g_before)
        if (b.ch == ch) {
            if (e) {
                logf("actions: %p stands up, HP entry flags %08X (before: %08X), +0x273C %08X, +0x31EA %u", (void *)ch,
                     game::at<uint32_t>(e, 0x110), b.flags, game::at<uint32_t>(ch, 0x273C), game::at<uint8_t>(ch, 0x31EA));
                game::at<uint32_t>(e, 0x110) = b.flags;
            }
            b = Before{};
        }
    game::at<uint32_t>(ch, 0x273C) &= ~0x20u;
    game::at<uint8_t>(ch, 0x31EA) = 0;
    // Known: on a pawn the skull next to its name stays after this (the game's own "pawn down" state).
    // Not cleared by: these two flags, the HP entry's flags (+0x110, 0 before and after), bit 0x00100000
    // of +0x2364/+0x236C/+0x2374 (set by the near-death action, cleared: no change), action 0x1D
    // (cPlActDmgStandUp); action 0xB5 (cPlActStatusRecover) crashed the game at 0x00B0F9E9.
    action_set(ch, 0);
}

void action_queue(int who, int number) { InterlockedExchange(&g_req, LONG(((who + 1) << 16) | (number & 0xFFFF))); }

namespace {
volatile LONG g_grab_req = 0; // tests: 1 + pawn the Arisen holds
}
void action_queue_grab(int pawn) { InterlockedExchange(&g_grab_req, 1 + pawn); }

// Game thread, from the Arisen's update.
void action_tick(uintptr_t arisen)
{
    if (LONG g = InterlockedExchange(&g_grab_req, 0)) { // tests: the Arisen holds pawn g - 1
        uintptr_t held = possess_pawn_ptr(int(g) - 1);
        if (held) {
            game::at<uintptr_t>(arisen, 0x38F0) = held;
            action_set(arisen, 0xA9);
            action_set(held, 0xAA);
        }
    }
    LONG r = InterlockedExchange(&g_req, 0);
    if (!r)
        return;
    int who = (r >> 16) - 1, number = r & 0xFFFF;
    uintptr_t ch = who == 0 ? arisen : possess_pawn_ptr(who - 1);
    if (ch && number == 0x80)
        action_lie_down(ch);
    else if (ch && number == 0xFFFF) // tests: act <who> FFFF = stand up the mod's way
        action_stand_up(ch);
    else if (ch)
        action_set(ch, number);
}

// The fall check FUN_00B78BF0 (__stdcall(ch, flag), `ret 8`, from the characters' update) hurts a
// character by its height through FUN_00888DA0 (another HP change than the hit one enemies.cpp
// hooks). A pawn showing a player in another area waits out of sight off the floor and it was killed
// there (write watch on its dead flag, 2026-10-28 11:32): such pawns skip the check, their place is
// the network's.
namespace {
using FallCheck_t = void(__stdcall *)(uintptr_t, char);
FallCheck_t g_fall_orig = nullptr;
void __stdcall hk_fall_check(uintptr_t ch, char flag)
{
    if (net_puppet_ptr(ch))
        return;
    g_fall_orig(ch, flag);
}
} // namespace

bool actions_install()
{
    void *target = reinterpret_cast<void *>(0x00B78BF0);
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&hk_fall_check), reinterpret_cast<void **>(&g_fall_orig));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    logf("actions: hook FallCheck 00B78BF0: %s", MH_StatusToString(st));
    return st == MH_OK;
}
