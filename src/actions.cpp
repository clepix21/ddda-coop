// Character actions (study 2026-10-28). Every human (uPlayerBase: the Arisen, pawns) runs one action
// object at a time, [ch+0x2DC8] (also +0x2DE8), rebuilt in place when the action changes (seen going
// cPlActWait -> cPlActRun at the same address while walking). The actions are numbered by the table
// at 0x01866C48 (0x44-byte entries, the action's MtDTI first: 0 cPlActWait, 1 cPlActWalk, 2 cPlActRun,
// 0x80 cPlActCmcNeardeath, 0xA4 cPlActLifted, 0xA9 cPlActHagaijime). vtable slot 0x210
// (FUN_00B540B0, __thiscall(ch, number), `ret 4`) changes the action; FUN_00B60870 does it when a
// character is restored, and for 0x80 also sets ch+0x273C |= 0x20 and ch+0x31EA = 1.
#include <windows.h>

#include <cstring>
#include <tuple>

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
// pawn fields the near-death action changes and leaves changed (object dumps before / after, 2026-10-28)
constexpr uintptr_t kKept[] = {0x3C0, 0x1BAC, 0x251C, 0x2A20, 0x4C20};
struct Before {
    uintptr_t ch;
    uint32_t flags; // HP entry +0x110 before the near-death action
    uint32_t kept[sizeof kKept / sizeof kKept[0]];
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
        *slot = Before{ch, game::at<uint32_t>(e, 0x110), {}};
        for (size_t k = 0; k < sizeof kKept / sizeof kKept[0]; ++k)
            slot->kept[k] = game::at<uint32_t>(ch, kKept[k]);
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
            if (early_flag("test_skull_restore"))
                for (size_t k = 0; k < sizeof kKept / sizeof kKept[0]; ++k) {
                    logf("actions:   +0x%X %08X -> %08X (as before lying down)", unsigned(kKept[k]), game::at<uint32_t>(ch, kKept[k]),
                         b.kept[k]);
                    game::at<uint32_t>(ch, kKept[k]) = b.kept[k];
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
} // namespace
int g_in_fall = 0; // game thread: inside the Arisen's fall check (its HP change is the fall's)
namespace {
void __stdcall hk_fall_check(uintptr_t ch, char flag)
{
    if (net_puppet_ptr(ch))
        return;
    bool arisen = ch == possess_arisen();
    g_in_fall += arisen;
    g_fall_orig(ch, flag);
    g_in_fall -= arisen;
}
} // namespace

// Hidden pawns sleep: a hired pawn no player uses (shared world, kept out of sight under the Arisen)
// still had its AI, which starts actions without the command object (it cast cPlActWpnWandShield
// twice while parked, test 2026-10-28). The two action changers, vtable slots 0x210 (FUN_00B540B0,
// thiscall(ch, number), `ret 4`) and 0x214 (FUN_00B6B270, thiscall(ch, number, 16 more), `ret 0x44`),
// refuse such a pawn anything but standing, moving, falling and being hit.
namespace {
using Set210_t = void(__fastcall *)(uintptr_t, void *, int);
using Set214_t = void(__fastcall *)(uintptr_t, void *, int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
Set210_t g_set210_orig = nullptr;
Set214_t g_set214_orig = nullptr;
volatile LONG g_refused = 0;

} // namespace

// The class name of action number n (the table at 0x01866C48, 0x44-byte entries, MtDTI first).
const char *action_class(int n)
{
    if (n < 0 || n > 0x201)
        return "";
    uintptr_t dti = game::at<uintptr_t>(0x01866C48 + uintptr_t(n) * 0x44, 0);
    const char *name = dti ? game::at<const char *>(dti, 4) : nullptr;
    return name && mem_readable(name, 1) ? name : "";
}

namespace {
bool sleeping_may(int n)
{
    if (n == 0 || n == 1 || n == 2) // wait, walk, run
        return true;
    const char *c = action_class(n);
    return strstr(c, "Fall") || strstr(c, "Land") || strstr(c, "Wait") || strstr(c, "Walk") || strstr(c, "Run") ||
           strncmp(c, "cPlActDmg", 9) == 0 || strstr(c, "Dead") || strstr(c, "Neardeath");
}

// The game's death actions: the Arisen's ends the game, a pawn's makes it vanish (deep water).
bool deadly(int n)
{
    const char *c = action_class(n);
    return strstr(c, "Dead") || strstr(c, "DipWater");
}

bool refuse(uintptr_t ch, int n)
{
    static int log_all = -1; // tests (test_log_actions=1): every action the Arisen and the player pawns start
    if (log_all < 0)
        log_all = early_flag("test_log_actions") ? 1 : 0;
    if (log_all && (ch == possess_arisen() || net_puppet_ptr(ch)))
        logf("actions: %s %p starts %s (0x%X)", ch == possess_arisen() ? "arisen" : "puppet", (void *)ch, action_class(n),
             unsigned(n));
    if (ch == possess_arisen() && deadly(n)) // shared world: down instead while another player is up
        return revive_on_fatal(ch, action_class(n), true);
    if (net_puppet_ptr(ch) && deadly(n)) { // a pawn showing a player dies with that player only
        static LONG k = 0;
        if (++k <= 20 || k % 100 == 0)
            logf("actions: a pawn showing a player asked for %s (0x%X): refused (%ld so far)", action_class(n), unsigned(n), k);
        return true;
    }
    if (n == 0x39 && net_puppet_in_weapon_action(ch)) { // cPlActGrabStart while its player aims or swings
        static LONG k = 0;
        if (++k <= 20 || k % 100 == 0)
            logf("actions: a pawn showing a player asked for a grab while its player is in a weapon action: refused (%ld so far)", k);
        return true;
    }
    if (!net_hidden_pawn_ptr(ch) || sleeping_may(n))
        return false;
    LONG k = InterlockedIncrement(&g_refused);
    if (k <= 20 || k % 100 == 0)
        logf("actions: a hidden pawn %p asked for %s (0x%X): refused (%ld so far)", (void *)ch, action_class(n), unsigned(n), k);
    return true;
}

void __fastcall hk_set210(uintptr_t ch, void *edx, int n)
{
    int64_t t0 = perf_now();
    bool no = refuse(ch, n);
    perf_add(PERF_ACTIONS, t0);
    if (!no)
        g_set210_orig(ch, edx, n);
}

void __fastcall hk_set214(uintptr_t ch, void *edx, int n, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f,
                          uint32_t g, uint32_t h, uint32_t i, uint32_t j, uint32_t k, uint32_t l, uint32_t m, uint32_t o, uint32_t p)
{
    if (!refuse(ch, n))
        g_set214_orig(ch, edx, n, a, b, c, d, e, f, g, h, i, j, k, l, m, o, p);
}
} // namespace

// A pawn showing a player says no pawn lines ("Ah, il en est donc ainsi..." from the guest's pawn on the
// host, screenshot review 2026-10-28). FUN_005AADC0 (__stdcall(ctrl), `ret 4`) is the per-pawn message
// controller's update: ctrl+4 -> +4 is its character, and the lines it hands to sAIMessageCtrl
// ([0x018D9270], by mCmcId) wait at ctrl+0x8C, count at ctrl+0x10C. That count is emptied first for
// such pawns (and for the hidden ones).
namespace {
using MsgUpdate_t = void(__stdcall *)(uintptr_t);
MsgUpdate_t g_msg_orig = nullptr;
volatile LONG g_muted = 0;
void __stdcall hk_msg_update(uintptr_t ctrl)
{
    uintptr_t owner = ctrl ? game::at<uintptr_t>(ctrl, 4) : 0;
    uintptr_t ch = owner ? game::at<uintptr_t>(owner, 4) : 0;
    if (ch && (net_puppet_ptr(ch) || net_hidden_pawn_ptr(ch)) && game::at<uint32_t>(ctrl, 0x10C)) {
        game::at<uint32_t>(ctrl, 0x10C) = 0;
        LONG k = InterlockedIncrement(&g_muted);
        if (k <= 5 || k % 50 == 0)
            logf("actions: pawn %p shows a player: its pawn line dropped (%ld so far)", (void *)ch, k);
    }
    g_msg_orig(ctrl);
}
} // namespace

// The save icon ("Enregistrement en cours. N'éteignez pas votre ordinateur.") during the mod's own
// snapshots: uGUISaveIcon's update (vtable slot 9, FUN_006DEC80, __fastcall(icon)) scales its +0x14
// by +0x24C each frame; 0 there for 8 s after a mod snapshot request. The save itself is untouched.
namespace {
using IconUpdate_t = void(__fastcall *)(uintptr_t);
IconUpdate_t g_icon_orig = nullptr;
volatile LONG g_quiet_until = 0;
void __fastcall hk_icon_update(uintptr_t icon)
{
    LONG until = g_quiet_until;
    if (until && int(GetTickCount() - DWORD(until)) < 0)
        game::at<float>(icon, 0x24C) = 0.f;
    g_icon_orig(icon);
}
} // namespace

void actions_quiet_save() { InterlockedExchange(&g_quiet_until, LONG((GetTickCount() + 8000) | 1)); }

// The other HP change, FUN_00888DA0 (EAX = character, stack: float change, int, char; `ret 12`; 49
// callers: falls, landings, ...): a pawn showing a player, or a hidden one, loses no HP through it.
// After the fall check was skipped, a pawn parked under the ground for minutes still died when its
// player came back (cPlActDmgDownDead, monster grab test 2026-10-28 19:28).
// The Arisen's fall takes its HP through here too: a fatal one (shared world, another player up)
// leaves 1 HP and puts it down where it last stood (revive.cpp); any other fatal change too, in place.
extern "C" bool __cdecl actions_hp_filter(uintptr_t ch, float *change) asm("actions_hp_filter");
extern "C" void *g_hp_orig asm("g_hp_orig");
void *g_hp_orig = nullptr;
extern "C" bool __cdecl actions_hp_filter(uintptr_t ch, float *change)
{
    if (*change >= 0.f)
        return false;
    if (net_puppet_ptr(ch) || net_hidden_pawn_ptr(ch))
        return true;
    if (ch != possess_arisen() || !net_shared())
        return false;
    if (revive_down())
        return true; // down: no more HP lost
    const float *hp = enemies_hp(ch);
    if (!hp || hp[0] + *change > 0.f)
        return false;
    if (!revive_on_fatal(ch, g_in_fall ? "a fall" : "a fatal HP change", g_in_fall != 0))
        return false;
    *change = hp[0] > 1.f ? 1.f - hp[0] : 0.f;
    return false;
}
extern "C" __attribute__((naked)) void actions_hp_thunk() asm("actions_hp_thunk");
extern "C" __attribute__((naked)) void actions_hp_thunk()
{
    asm volatile("pushl %eax\n\t"
                 "leal 8(%esp), %ecx\n\t" // the change (first stack argument, past the pushed EAX)
                 "pushl %ecx\n\t"
                 "pushl %eax\n\t"
                 "call actions_hp_filter\n\t"
                 "addl $8, %esp\n\t"
                 "testb %al, %al\n\t"
                 "popl %eax\n\t"
                 "jnz 1f\n\t"
                 "jmp *g_hp_orig\n"
                 "1:\n\t"
                 "ret $12\n\t");;
}

bool actions_install()
{
    {
        void *t = reinterpret_cast<void *>(0x00888DA0);
        MH_STATUS st = MH_CreateHook(t, reinterpret_cast<void *>(&actions_hp_thunk), &g_hp_orig);
        if (st == MH_OK)
            st = MH_EnableHook(t);
        logf("actions: hook HpChange 00888DA0: %s", MH_StatusToString(st));
    }
    {
        void *t = reinterpret_cast<void *>(0x006DEC80);
        MH_STATUS st = MH_CreateHook(t, reinterpret_cast<void *>(&hk_icon_update), reinterpret_cast<void **>(&g_icon_orig));
        if (st == MH_OK)
            st = MH_EnableHook(t);
        logf("actions: hook SaveIconUpdate 006DEC80: %s", MH_StatusToString(st));
    }
    {
        void *t = reinterpret_cast<void *>(0x005AADC0);
        MH_STATUS st = MH_CreateHook(t, reinterpret_cast<void *>(&hk_msg_update), reinterpret_cast<void **>(&g_msg_orig));
        if (st == MH_OK)
            st = MH_EnableHook(t);
        logf("actions: hook PawnMessageUpdate 005AADC0: %s", MH_StatusToString(st));
    }
    for (auto [addr, hook, orig] : {std::tuple<uintptr_t, void *, void **>{0x00B540B0, reinterpret_cast<void *>(&hk_set210),
                                                                            reinterpret_cast<void **>(&g_set210_orig)},
                                    std::tuple<uintptr_t, void *, void **>{0x00B6B270, reinterpret_cast<void *>(&hk_set214),
                                                                            reinterpret_cast<void **>(&g_set214_orig)}}) {
        MH_STATUS st = MH_CreateHook(reinterpret_cast<void *>(addr), hook, orig);
        if (st == MH_OK)
            st = MH_EnableHook(reinterpret_cast<void *>(addr));
        logf("actions: hook SetAction %08X: %s", unsigned(addr), MH_StatusToString(st));
    }
    void *target = reinterpret_cast<void *>(0x00B78BF0);
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&hk_fall_check), reinterpret_cast<void **>(&g_fall_orig));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    logf("actions: hook FallCheck 00B78BF0: %s", MH_StatusToString(st));
    return st == MH_OK;
}
