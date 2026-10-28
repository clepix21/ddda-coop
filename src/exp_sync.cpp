// Shared world: every enemy killed by any player gives the same exp to all players, wherever they are.
//
// The game gives a kill's exp to the party with FUN_00443470(sGameSys, exp, factor, fixed) (stdcall;
// the enemy death code FUN_008573e0 calls it with fixed = 0; with fixed != 0 the exp is used as
// given). It adds it to the Arisen ([sGameSys+0xA76D0], exp at +0x2C4, FUN_00782D10 also adds
// vocation exp and discipline), to the main pawn (with a catch-up) and gives the hired pawns
// something else (+0x34C). Found 2026-10-27 17:49: a write watchpoint on the Arisen's exp
// ([Arisen+0x3DEC]+0x2C4, the same object) hit in FUN_00782D10 under FUN_00443470 at each kill.
//
// Each game counts the exp its own world gave its Arisen (measured around the call) as a running
// total and sends it (guests in PSTATE, the host in SNAP with every player's total). Each game gives
// its own party, through the same function with fixed = 1, what another player's total grew by:
// totals survive lost or repeated packets. A guest in the host's area does not count its own kills:
// its enemies there are the host's (their deaths are copies), and the host's total brings that exp.
#include <windows.h>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {

using Grant_t = void(__stdcall *)(uintptr_t gs, int exp, float factor, char fixed); // RET 0x10 (0x004437BD)
constexpr uintptr_t kGrant = 0x00443470;
constexpr uintptr_t kArisenParam = 0xA76D0, kExp = 0x2C4, kDiscipline = 0x344; // FUN_00782D10: [0xB1], [0xD1]
Grant_t g_orig = nullptr;
thread_local bool g_applying = false;
volatile LONG64 g_total = 0;   // exp this world gave its Arisen (shared world)
volatile LONG64 g_pending = 0; // other players' exp not given yet

int32_t arisen_exp(uintptr_t gs) { return gs ? game::at<int32_t>(gs, kArisenParam + kExp) : 0; }
int32_t arisen_jp(uintptr_t gs) { return gs ? game::at<int32_t>(gs, kArisenParam + kDiscipline) : 0; }

// A guest's own world is never saved: what its Arisen gains is kept for its own save.
void keep_for_guest(int32_t exp, int32_t jp)
{
    if (net_role() == NET_GUEST && (exp > 0 || jp > 0))
        progress_shared_gain(exp, jp, -1, 0, 0);
}

void __stdcall hk_grant(uintptr_t gs, int exp, float factor, char fixed)
{
    if (g_applying || !net_shared()) {
        g_orig(gs, exp, factor, fixed);
        return;
    }
    if (net_role() == NET_GUEST && !fixed && net_enemy_follower()) {
        static DWORD next_log = 0;
        if (!next_log || int(GetTickCount() - next_log) >= 0) {
            next_log = GetTickCount() + 2000;
            logf("exp: a kill where another player's game runs the enemies (the host's area or a guest's group): its exp comes from there (%d here dropped)", exp);
        }
        return;
    }
    int32_t before = arisen_exp(gs), jp_before = arisen_jp(gs);
    g_orig(gs, exp, factor, fixed);
    int32_t gained = arisen_exp(gs) - before;
    keep_for_guest(gained, arisen_jp(gs) - jp_before);
    if (gained > 0) {
        LONG64 total = InterlockedAdd64(&g_total, gained);
        logf("exp: +%d in this world (total %lld), shared with the other players", gained, total);
    }
}

} // namespace

bool exp_sync_install()
{
    MH_STATUS st = MH_CreateHook(reinterpret_cast<void *>(kGrant), reinterpret_cast<void *>(&hk_grant),
                                 reinterpret_cast<void **>(&g_orig));
    if (st == MH_OK)
        st = MH_EnableHook(reinterpret_cast<void *>(kGrant));
    logf("exp: hook PartyExp %08X: %s", unsigned(kGrant), MH_StatusToString(st));
    return st == MH_OK;
}

long long exp_sync_total() { return g_total; }

// Net thread: another player's total, seen at `slot` (its place in the host's list). The first value
// seen, or a smaller one (that player restarted its game), is only a starting point.
void exp_sync_seen(int slot, long long total)
{
    static long long base[8] = {};
    static bool known[8] = {};
    if (slot < 0 || slot >= 8)
        return;
    if (!known[slot] || total < base[slot]) {
        known[slot] = true;
        base[slot] = total;
        return;
    }
    if (total > base[slot]) {
        InterlockedAdd64(&g_pending, total - base[slot]);
        logf("exp: player %d's world gave +%lld exp: given here too", slot, total - base[slot]);
        base[slot] = total;
    }
}

// Game thread, the Arisen's update: give the party what the other players earned.
void exp_sync_apply()
{
    if (!g_orig || !g_pending)
        return;
    LONG64 n = InterlockedExchange64(&g_pending, 0);
    uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0);
    if (n <= 0 || !gs || !current_stage_area()) {
        InterlockedAdd64(&g_pending, n);
        return;
    }
    g_applying = true;
    int32_t before = arisen_exp(gs), jp_before = arisen_jp(gs);
    g_orig(gs, int(n > 0x7FFFFFFF ? 0x7FFFFFFF : n), 1.0f, 1);
    g_applying = false;
    keep_for_guest(arisen_exp(gs) - before, arisen_jp(gs) - jp_before);
    logf("exp: +%lld from the other players given to this party (Arisen exp %d -> %d)", n, before, arisen_exp(gs));
}
