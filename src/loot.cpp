// Shared world loot: the player who picks an item up keeps it; every other player gets a copy in
// its storage (owner's choice, 2026-10-27).
//
// The game adds items with FUN_0045E680, __thiscall on the item manager ([0x018FA4D4], checked:
// it reads [this+0x110]+0x60 like the code that reads [0x018FA4D4]+0x110), 6 stack arguments (RET
// 0x18): (item number, count (999 at most), who (0 = Arisen, 1..3 = pawns by mCmcId, 4 = storage,
// full at 7000 = [this+0x4920]), ?, flags (low 4 bits: quality), ?). Found 2026-10-27 19:20 from
// the instructions comparing [reg+0x4920] with 7000 (ScalarRefs 0x1B58). A pickup in the world
// comes from 0x00C29471 with (0, 0, 1) (a pawn picked up item 1337 twice in a fight, 19:21).
//
// Each player keeps the list of its own party's pickups (its Arisen's and its AI pawns', from the
// pickup code; who 1..3 = 1 + mCmcId, the party index net_puppet takes); net.cpp sends
// it to the host and the host to the other guests, numbered, each side telling what it applied.
// A copy goes in the storage through the same function (who = 4). A guest's own world is never
// saved, so on a guest its pickups and the copies it gets are also kept for its own save
// (progress_shared_gain, pending.txt).
#include <windows.h>

#include <deque>
#include <vector>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {

using AddItem_t = int(__attribute__((thiscall)) *)(void *mgr, unsigned item, unsigned count, int who, int a5,
                                                    unsigned flags, unsigned a7);
constexpr uintptr_t kAddItem = 0x0045E680, kItemManager = 0x018FA4D4, kPickupCall = 0x00C29471;
AddItem_t g_orig = nullptr;
thread_local bool g_applying = false;
CRITICAL_SECTION g_lock;
std::vector<LootEv> g_mine;
std::deque<LootEv> g_queue;

int __attribute__((thiscall)) hk_add_item(void *mgr, unsigned item, unsigned count, int who, int a5, unsigned flags, unsigned a7)
{
    int r = g_orig(mgr, item, count, who, a5, flags, a7);
    uintptr_t from = uintptr_t(__builtin_return_address(0));
    if (g_applying)
        return r;
    if (cfg_debug_log())
        logf("loot: add item %u x%u to %d (a5 %d, flags %X, a7 %X) from %08X -> %d", item, count, who, a5, flags, a7,
             unsigned(from), r);
    // this world's party: its Arisen (this player) or its own AI pawns; not a pawn showing another
    // player (that player's own world counts its pickups)
    bool ours = who == 0 || (who >= 1 && who <= 3 && !net_puppet(who));
    if (!net_shared() || !ours || from != kPickupCall || !r || item > 0xFFFF || !count)
        return r;
    LootEv e = {uint16_t(item), uint16_t(count > 999 ? 999 : count), flags};
    EnterCriticalSection(&g_lock);
    g_mine.push_back(e);
    size_t n = g_mine.size();
    LeaveCriticalSection(&g_lock);
    logf("loot: picked up item %u x%u (pickup %u), a copy goes to the other players", item, count, unsigned(n));
    if (net_role() == NET_GUEST)
        progress_shared_gain(0, 0, int(item), int(e.count), flags); // its own world is never saved
    return r;
}

} // namespace

bool loot_install()
{
    InitializeCriticalSection(&g_lock);
    MH_STATUS st = MH_CreateHook(reinterpret_cast<void *>(kAddItem), reinterpret_cast<void *>(&hk_add_item),
                                 reinterpret_cast<void **>(&g_orig));
    if (st == MH_OK)
        st = MH_EnableHook(reinterpret_cast<void *>(kAddItem));
    logf("loot: hook AddItem %08X: %s", unsigned(kAddItem), MH_StatusToString(st));
    return st == MH_OK;
}

uint32_t loot_mine_count()
{
    EnterCriticalSection(&g_lock);
    uint32_t n = uint32_t(g_mine.size());
    LeaveCriticalSection(&g_lock);
    return n;
}

// This player's pickups number from_seq.. (1-based), up to max.
void loot_net_mine(std::vector<LootEv> &out, uint32_t from_seq, int max)
{
    out.clear();
    EnterCriticalSection(&g_lock);
    for (uint32_t s = from_seq; s >= 1 && s <= g_mine.size() && int(out.size()) < max; ++s)
        out.push_back(g_mine[s - 1]);
    LeaveCriticalSection(&g_lock);
}

void loot_net_apply(const LootEv &e)
{
    EnterCriticalSection(&g_lock);
    g_queue.push_back(e);
    LeaveCriticalSection(&g_lock);
}

// Game thread (the Arisen's update): copies of the other players' pickups, into the storage.
void loot_apply_queued()
{
    if (!g_orig)
        return;
    for (;;) {
        EnterCriticalSection(&g_lock);
        if (g_queue.empty()) {
            LeaveCriticalSection(&g_lock);
            return;
        }
        LootEv e = g_queue.front();
        g_queue.pop_front();
        LeaveCriticalSection(&g_lock);
        void *mgr = game::at<void *>(kItemManager, 0);
        int r = 0;
        if (mgr) {
            g_applying = true;
            r = g_orig(mgr, e.item, e.count, 4, 0, e.flags, 1);
            g_applying = false;
        }
        logf("loot: a copy of another player's item %u x%u put in the storage -> %d", e.item, e.count, r);
        if (net_role() == NET_GUEST)
            progress_shared_gain(0, 0, e.item, e.count, e.flags);
    }
}
