// Talks: a hook on the talk start (FUN_00B99290, __fastcall(action)), the player's talk action.
// For now it logs the action's fields that point at game objects, to find which NPC the talk is
// with (the guest must talk to the same one).
#include <windows.h>

#include <cstring>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {

constexpr uintptr_t kTalkStart = 0x00B99290;
using TalkStart_t = void(__fastcall *)(uintptr_t);
TalkStart_t g_talk_orig = nullptr;
CRITICAL_SECTION g_lock;
float g_npc_pos[3] = {};
volatile LONG g_talks = 0; // talk starts seen

void __fastcall hk_talk_start(uintptr_t action)
{
    uintptr_t who = action && mem_readable(reinterpret_cast<void *>(action + 0x74), 4) ? game::at<uintptr_t>(action, 0x74) : 0;
    if (who && mem_readable(reinterpret_cast<void *>(who + 0x40), 12)) {
        EnterCriticalSection(&g_lock);
        memcpy(g_npc_pos, reinterpret_cast<void *>(who + 0x40), sizeof g_npc_pos);
        LeaveCriticalSection(&g_lock);
        InterlockedIncrement(&g_talks);
    }
    if (cfg_debug_log() && action && mem_readable(reinterpret_cast<void *>(action), 0x180)) {
        uintptr_t npc = game::at<uintptr_t>(action, 0x74);
        bool pos = npc && mem_readable(reinterpret_cast<void *>(npc + 0x40), 12);
        logf("talk: start, action %p (vt %08X), actor %p at (%.0f, %.0f, %.0f)", (void *)action,
             unsigned(game::at<uintptr_t>(action, 0)), (void *)npc, pos ? game::at<float>(npc, 0x40) : 0.f,
             pos ? game::at<float>(npc, 0x44) : 0.f, pos ? game::at<float>(npc, 0x48) : 0.f);
        for (uintptr_t f = 4; f < 0x180; f += 4) {
            uintptr_t v = game::at<uintptr_t>(action, f);
            if (v < 0x10000 || (v & 3) || !mem_readable(reinterpret_cast<void *>(v), 8))
                continue;
            uintptr_t vt = game::at<uintptr_t>(v, 0);
            if (vt >= 0x00401000 && vt < 0x01700000)
                logf("talk:   +0x%X -> %p %s", unsigned(f), (void *)v, enemies_class_of(v));
        }
    }
    g_talk_orig(action);
}

} // namespace

// The NPC of the latest talk start (its position) and how many talks started so far.
uint32_t talk_last(float pos[3])
{
    EnterCriticalSection(&g_lock);
    memcpy(pos, g_npc_pos, sizeof g_npc_pos);
    LeaveCriticalSection(&g_lock);
    return uint32_t(g_talks);
}

bool talk_install()
{
    InitializeCriticalSection(&g_lock);
    void *target = reinterpret_cast<void *>(kTalkStart);
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&hk_talk_start), reinterpret_cast<void **>(&g_talk_orig));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    logf("talk: hook TalkStart %08X: %s", unsigned(kTalkStart), MH_StatusToString(st));
    return st == MH_OK;
}
