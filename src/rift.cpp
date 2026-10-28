// The Rift stays shut while co-op is on (owner's request, 2026-10-28: the Rift and its online pawns
// broke co-op tests twice). The Rift stone is uOmObj8505 (vtable 0x016093D0; found 97 u in front of
// the Arisen at the Pawn Guild start of guild.sav, the only one in the area). Its vtable slot 8,
// FUN_00C495A0 (only referenced there), runs the stone's state machine; the state is the byte at
// +0x28. In state 1 the stone waits for "Examiner": +0x29A6 bit 0x10. The game's own "Rift not
// available" path (when [[0x018D099C]+0x3834]+0x84 == 1) clears that bit, sets +0x291C = 0 and
// +0x29F0 = 7 and stays in state 1. The hook does the same before the original runs, so the press
// does nothing. allow_rift=1 in coop.ini turns the block off.
// Tests: test_rift_kill=1 ends the game at once if the stone still leaves state 1 (a press that got
// through), before the Rift loads.
#include <windows.h>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {

constexpr uintptr_t kRiftStoneUpdate = 0x00C495A0;
constexpr uintptr_t kState = 0x28, kFlags = 0x29A6, kA47 = 0x291C, kPrompt = 0x29F0;
using Update_t = void(__fastcall *)(uintptr_t);
Update_t g_orig = nullptr;
bool g_block = false, g_kill = false;
volatile LONG g_blocked = 0;

void __fastcall hk_update(uintptr_t stone)
{
    uint8_t before = game::at<uint8_t>(stone, kState);
    if (g_block && before == 1 && (game::at<uint8_t>(stone, kFlags) & 0x10)) {
        game::at<uint8_t>(stone, kFlags) &= uint8_t(~0x10);
        game::at<uint32_t>(stone, kA47) = 0;
        game::at<uint32_t>(stone, kPrompt) = 7;
        LONG n = InterlockedIncrement(&g_blocked);
        logf("rift: Rift stone %p examined: the Rift stays shut while co-op is on (%ld so far)", (void *)stone, n);
    }
    g_orig(stone);
    uint8_t after = game::at<uint8_t>(stone, kState);
    if (after != before && after != 0 && after != 1 && after != 6 && after != 7)
        logf("rift: Rift stone %p state %u -> %u", (void *)stone, unsigned(before), unsigned(after));
    if (g_kill && before == 1 && after != 1 && after != 0) {
        logf("rift: test_rift_kill: the stone left state 1 (%u): ending the game before the Rift", unsigned(after));
        TerminateProcess(GetCurrentProcess(), 0x52494654);
    }
}

} // namespace

bool rift_install()
{
    g_block = cfg_role() != NET_OFF && !early_flag("allow_rift");
    g_kill = early_flag("test_rift_kill");
    void *target = reinterpret_cast<void *>(kRiftStoneUpdate);
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&hk_update), reinterpret_cast<void **>(&g_orig));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    logf("rift: hook RiftStoneUpdate %08X: %s; the Rift is %s", unsigned(kRiftStoneUpdate), MH_StatusToString(st),
         g_block ? "shut (co-op on)" : "open");
    return st == MH_OK;
}

uint32_t rift_blocked() { return uint32_t(g_blocked); }
