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
// Moving an item between bags and the storage ("Retirer" to oneself or to a pawn, giving to a pawn) adds it
// through 0x0045FB56; a shop purchase through 0x006E676E (shop test 2026-11-06 23:09-23:11: item 727 bought,
// to 0; item 22 withdrawn, to 0; item 50 given to the pawn showing the host, to 2).
constexpr uintptr_t kMoveCall = 0x0045FB56, kShopCall = 0x006E676E;
// A gift rides the pickup stream: flags bit 31 set, bits 24-25 = the player it is for (item flags use the
// low bits: quality in the low 4).
constexpr unsigned kGift = 0x80000000u, kItemFlags = 0x00FFFFFFu;
struct Queued { LootEv e; int from; };
AddItem_t g_orig = nullptr;
thread_local bool g_applying = false;
CRITICAL_SECTION g_lock;
std::vector<LootEv> g_mine;
std::deque<Queued> g_queue;

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
    // a gift: an item given to a pawn that shows another player (shared world, issue on Nexus 2026-11-06:
    // "we are not able to give each other items") goes to that player's own Arisen
    if (net_shared() && from == kMoveCall && r && who >= 1 && who <= 3 && net_puppet(who) && item <= 0xFFFF && count) {
        int to = net_party_slot(who);
        if (to >= 0 && to <= 3 && to != net_my_slot()) {
            LootEv g = {uint16_t(item), uint16_t(count > 999 ? 999 : count), (flags & kItemFlags) | kGift | (unsigned(to) << 24)};
            EnterCriticalSection(&g_lock);
            g_mine.push_back(g);
            LeaveCriticalSection(&g_lock);
            logf("loot: item %u x%u given to the pawn showing player %d: sent to that player", item, count, to);
            // a guest that gives away something it gained this session (bought, picked up) does not keep it
            // for its own save too
            if (net_role() == NET_GUEST)
                progress_shared_gain(0, 0, int(item), -int(g.count), flags & kItemFlags);
        }
        return r;
    }
    // a shop purchase on a guest: its own world is never saved, so the item is kept for its own save
    if (net_shared() && from == kShopCall && r && who == 0 && net_role() == NET_GUEST && item <= 0xFFFF && count) {
        logf("loot: bought item %u x%u: kept for this player's own save", item, count);
        progress_shared_gain(0, 0, int(item), int(count > 999 ? 999 : count), flags);
        return r;
    }
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

// The Arisen's gold: 0x006E08D0 adds (capped at 99,999,999), 0x006E0950 takes (not below 0), at sGameSys +
// 0xA76D0 + 0x348; both virtual (vtables at 0x0158651C/20...), __thiscall, one stack argument, `ret 4`.
// The shop's deal (code around 0x006E3340) calls the add for a sale and the take for a purchase: write
// watch on the gold, 2026-11-07 00:51 (sale written before 0x006E0914, return 0x006E3369 on the stack;
// purchase before 0x006E098B, 0x006E3356). A guest's bag in its world is the host's (the save's item lists
// are not its character's), so a sale there turned the host's items into the guest's gold: the gold of a
// sale is not kept for its own save (progress_guest_gold_tick takes it off).
using AddGold_t = void(__attribute__((thiscall)) *)(void *self, int amount);
constexpr uintptr_t kAddGold = 0x006E08D0;
AddGold_t g_orig_gold = nullptr;
volatile LONG g_sale_gold = 0;

void __attribute__((thiscall)) hk_add_gold(void *self, int amount)
{
    uintptr_t from = uintptr_t(__builtin_return_address(0));
    g_orig_gold(self, amount);
    bool sale = from >= 0x006E3300 && from < 0x006E3400;
    if (cfg_debug_log())
        logf("loot: gold +%d from %08X%s", amount, unsigned(from), sale ? " (a sale)" : "");
    if (sale && amount > 0 && net_role() == NET_GUEST)
        InterlockedExchangeAdd(&g_sale_gold, amount);
}

} // namespace

// Gold from the guest's sales since the last call (taken off its kept gold changes).
long loot_take_sale_gold() { return InterlockedExchange(&g_sale_gold, 0); }

bool loot_install()
{
    InitializeCriticalSection(&g_lock);
    {
        MH_STATUS gs = MH_CreateHook(reinterpret_cast<void *>(kAddGold), reinterpret_cast<void *>(&hk_add_gold),
                                     reinterpret_cast<void **>(&g_orig_gold));
        if (gs == MH_OK)
            gs = MH_EnableHook(reinterpret_cast<void *>(kAddGold));
        logf("loot: hook AddGold %08X: %s", unsigned(kAddGold), MH_StatusToString(gs));
    }
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

void loot_net_apply(const LootEv &e, int from_slot)
{
    EnterCriticalSection(&g_lock);
    g_queue.push_back(Queued{e, from_slot});
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
        Queued q = g_queue.front();
        g_queue.pop_front();
        LeaveCriticalSection(&g_lock);
        LootEv e = q.e;
        void *mgr = game::at<void *>(kItemManager, 0);
        int r = 0;
        if (e.flags & kGift) {
            int to = int((e.flags >> 24) & 3);
            if (to != net_my_slot())
                continue; // for another player
            if (mgr) {
                g_applying = true;
                r = g_orig(mgr, e.item, e.count, 0, 0, e.flags & kItemFlags, 1);
                g_applying = false;
            }
            logf("loot: player %d gave this player item %u x%u: put in its bag -> %d", q.from, e.item, e.count, r);
            // a guest keeps it for its own save, unless the host gave it: the host's world has it in the bag
            // of the pawn showing this guest, which the host's progress records already count
            if (net_role() == NET_GUEST && q.from != 0 && r)
                progress_shared_gain(0, 0, e.item, e.count, e.flags & kItemFlags);
            continue;
        }
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
