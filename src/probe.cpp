// Hook on the per-character command update (FUN_00B6AEA0). Runs possession around the original
// call; with debug_log=1 also logs every character seen (class, pawn id, command mode), pressed
// command bits of the Arisen and main pawn, and once per second the state of up to 8 of them.
#include <windows.h>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {

using UpdateCommands_t = void(__fastcall *)(uintptr_t);
UpdateCommands_t g_orig = nullptr;

// FUN_0084BF60: uCharacterBase transform finalize (angle from the world matrix, quaternion from
// +0x257C, position). Found with a hardware write watchpoint on the Arisen's quaternion and angle.
// 0 = the Arisen, 1 + mCmcId for a party pawn, -1 for anyone else
int party_index(uintptr_t ch)
{
    if (ch == possess_arisen())
        return 0;
    for (int p = 0; p < kPawns; ++p)
        if (ch == possess_pawn_ptr(p))
            return 1 + p;
    return -1;
}

using FinalizeTransform_t = void(__fastcall *)(uintptr_t);
FinalizeTransform_t g_orig_finalize = nullptr;
void __fastcall hk_finalize(uintptr_t ch)
{
    g_orig_finalize(ch);
    if (__builtin_return_address(0) == reinterpret_cast<void *>(game::kEnemyFinalizeRet)) {
        enemies_on_finalize(ch);
        return;
    }
    if (net_role() != NET_GUEST)
        return;
    int idx = party_index(ch);
    if (idx >= 0) {
        net_apply_transform(ch, idx);
        net_guest_camera();
    }
}

struct Seen {
    uintptr_t ch;
    uintptr_t vt;
    DWORD last_log;
};
Seen g_seen[32];

// NPCs (uNpc) updated recently, for tests (bridge "nlist" / "ngoto"): pointer and last update.
struct NpcSeen {
    uintptr_t ch;
    DWORD last;
};
NpcSeen g_npcs[64] = {};

void note_npc(uintptr_t ch)
{
    DWORD now = GetTickCount();
    int free_i = -1;
    for (int i = 0; i < 64; ++i) {
        if (g_npcs[i].ch == ch) {
            g_npcs[i].last = now;
            return;
        }
        if (free_i < 0 && (!g_npcs[i].ch || now - g_npcs[i].last > 2000))
            free_i = i;
    }
    if (free_i >= 0)
        g_npcs[free_i] = {ch, now};
}
int g_nseen = 0;

const char *class_name(uintptr_t vt)
{
    switch (vt) {
    case game::kVtPlayer: return "uPlayer";
    case game::kVtCmc: return "uCmc";
    case game::kVtNpc: return "uNpc";
    case game::kVtPlayerBase: return "uPlayerBase";
    default: return "other";
    }
}

void __fastcall hk_update(uintptr_t ch)
{
    uint8_t *copy = nullptr;
    int who = possess_pre(ch, &copy);
    g_orig(ch);
    if (who)
        possess_post(who, copy);
    if (who && net_role() == NET_GUEST)
        net_apply_snapshot(ch, possess_party_index(who));
    if (net_role() == NET_HOST) {
        int idx = party_index(ch);
        if (idx >= 0)
            net_host_record(ch, idx);
    }
    if (game::at<uintptr_t>(ch, 0) == game::kVtNpc)
        note_npc(ch); // several game threads: a stale or torn entry only affects test commands
    if (!cfg_debug_log())
        return;
    uintptr_t vt = game::at<uintptr_t>(ch, 0);
    int i = 0;
    while (i < g_nseen && g_seen[i].ch != ch)
        ++i;
    if (i == g_nseen) {
        if (g_nseen == 32)
            return;
        g_seen[g_nseen++] = {ch, vt, 0};
        uintptr_t cmd = ch + game::kCmd;
        if (vt == game::kVtCmc)
            logf("probe: new %s %p cmcId %d mpPlayer %p cmdMode %u cmdOwner %p", class_name(vt),
                 (void *)ch, game::at<int>(ch, game::kCmcId), (void *)game::at<uintptr_t>(ch, game::kCmcPlayer),
                 game::at<uint8_t>(cmd, game::kCmdMode), (void *)game::at<uintptr_t>(cmd, 8));
        else
            logf("probe: new %s %p (vt %08X) cmdMode %u cmdOwner %p", class_name(vt), (void *)ch, (unsigned)vt,
                 game::at<uint8_t>(cmd, game::kCmdMode), (void *)game::at<uintptr_t>(cmd, 8));
    }
    // party: the Arisen ("arisen"), the main pawn ("pawn", as before) and the hired pawns ("pawn1", "pawn2")
    int idx = party_index(ch);
    const char *party_names[kParty] = {"arisen", "pawn", "pawn1", "pawn2"};
    if (idx >= 0) {
        uint32_t press = game::at<uint32_t>(ch + game::kCmd, game::kCmdPress);
        if (press)
            logf("probe: PRESS %s %08X", party_names[idx], press);
    }
    DWORD now = GetTickCount();
    if (now - g_seen[i].last_log >= 1000 && (i < 8 || idx >= 0)) {
        g_seen[i].last_log = now;
        uintptr_t cmd = ch + game::kCmd;
        logf("probe: %s %p mode %u held %08X press %08X", class_name(vt), (void *)ch,
             game::at<uint8_t>(cmd, game::kCmdMode), game::at<uint32_t>(cmd, game::kCmdHeld),
             game::at<uint32_t>(cmd, game::kCmdPress));
        if (idx >= 0) {
            const float *f = reinterpret_cast<const float *>(ch + 0x30);
            logf("probe:   +30 %.2f %.2f %.2f %.2f | +40 %.2f %.2f %.2f %.2f | +50 %.2f %.2f %.2f %.2f", f[0], f[1],
                 f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
        }
    }
}

} // namespace

bool probe_install()
{
    void *target = reinterpret_cast<void *>(game::kUpdateCommands);
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&hk_update), reinterpret_cast<void **>(&g_orig));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    logf("probe: hook UpdateCommands %p: %s", target, MH_StatusToString(st));
    void *fin = reinterpret_cast<void *>(0x0084BF60);
    MH_STATUS st2 = MH_CreateHook(fin, reinterpret_cast<void *>(&hk_finalize), reinterpret_cast<void **>(&g_orig_finalize));
    if (st2 == MH_OK)
        st2 = MH_EnableHook(fin);
    logf("probe: hook FinalizeTransform %p: %s", fin, MH_StatusToString(st2));
    return st == MH_OK && st2 == MH_OK;
}

// NPCs updated in the last second (tests).
int probe_npcs(uintptr_t *out, int cap)
{
    DWORD now = GetTickCount();
    int n = 0;
    for (int i = 0; i < 64 && n < cap; ++i)
        if (g_npcs[i].ch && now - g_npcs[i].last < 1000)
            out[n++] = g_npcs[i].ch;
    return n;
}
