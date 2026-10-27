// Talks: a hook on the talk start (FUN_00B99290, __fastcall(action)), the player's talk action.
// For now it logs the action's fields that point at game objects, to find which NPC the talk is
// with (the guest must talk to the same one).
#include <windows.h>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {

constexpr uintptr_t kTalkStart = 0x00B99290;
using TalkStart_t = void(__fastcall *)(uintptr_t);
TalkStart_t g_talk_orig = nullptr;

void __fastcall hk_talk_start(uintptr_t action)
{
    if (cfg_debug_log() && action && mem_readable(reinterpret_cast<void *>(action), 0x180)) {
        logf("talk: start, action %p (vt %08X), actor %p", (void *)action, unsigned(game::at<uintptr_t>(action, 0)),
             (void *)game::at<uintptr_t>(action, 0x74));
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

bool talk_install()
{
    void *target = reinterpret_cast<void *>(kTalkStart);
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&hk_talk_start), reinterpret_cast<void **>(&g_talk_orig));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    logf("talk: hook TalkStart %08X: %s", unsigned(kTalkStart), MH_StatusToString(st));
    return st == MH_OK;
}
