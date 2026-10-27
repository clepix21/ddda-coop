// Shared world, quests and world state: the save's flag tables live in sGameSys ([0x018FA4BC]) in the
// save's own order (found 2026-10-27 19:00 by matching cutscene.sav's arrays in a dump of it):
//   mGsfFlag    48 u32 at +0xAC6E4      mQstFlag 384 u32 at +0xAC7A4 (quests)
//   mNpcDieFlag 29 u32 at +0xACFEC
//   mStgFlag    65 stages at +0xADEE8, 0x14C apart: Flag000..Flag (6 u32), mItemFlag (60 u32: items
//               of the area picked up), mOmFlag (16 u32: objects of the area, chests among them)
// Every word is watched; a change made by one world (bits set and bits cleared) goes to every other
// world (net.cpp, numbered like the pickups), so a quest step, a chest opened or an item picked up by
// any player is the same for all, and the host's saves keep it. A load (a game loaded, a retry, a
// reload) puts the save's flags back: the next look only takes the new values as the starting point.
#include <windows.h>

#include <vector>

#include "game.h"
#include "mod.h"

namespace {

struct Region {
    const char *name;
    uint32_t off, words, stride, count;
};
const Region kRegions[] = {
    {"gsf", 0xAC6E4, 48, 0, 1},
    {"quest", 0xAC7A4, 384, 0, 1},
    {"npcdie", 0xACFEC, 29, 0, 1},
    {"stage", 0xADEE8, 82, 0x14C, 65},
};

// Words that only show a passing state (not world progress): never sent. gsf word 15 (+0xAC720)
// has bit 4 set only while a cutscene plays (the Olra cutscene, 2026-10-27 19:37).
const uint32_t kSkipOffs[] = {0xAC720};
constexpr DWORD kStableMs = 3000; // a change is sent once the word kept its value that long

std::vector<uint32_t> g_offs;  // sGameSys offset of every watched word
std::vector<uint32_t> g_base;  // last value seen or applied
std::vector<uint8_t> g_known;  // g_base valid
std::vector<uint32_t> g_cur;   // value now (a change waits in g_since until stable)
std::vector<DWORD> g_since;    // 0 = no change waiting
DWORD g_seen_load = 0;
CRITICAL_SECTION g_lock;
std::vector<FlagEv> g_mine;     // this world's changes, numbered from 1

void build()
{
    for (const Region &r : kRegions)
        for (uint32_t c = 0; c < r.count; ++c)
            for (uint32_t w = 0; w < r.words; ++w)
                g_offs.push_back(r.off + c * r.stride + w * 4);
    g_base.assign(g_offs.size(), 0);
    g_known.assign(g_offs.size(), 0);
    g_cur.assign(g_offs.size(), 0);
    g_since.assign(g_offs.size(), 0);
}

bool skipped(uint32_t off)
{
    for (uint32_t s : kSkipOffs)
        if (s == off)
            return true;
    return false;
}

// Which table a word is in, for logs.
const char *where(uint32_t i, uint32_t &sub)
{
    uint32_t n = 0;
    for (const Region &r : kRegions) {
        uint32_t size = r.words * r.count;
        if (i < n + size) {
            sub = i - n;
            return r.name;
        }
        n += size;
    }
    sub = i;
    return "?";
}

} // namespace

bool flags_install()
{
    InitializeCriticalSection(&g_lock);
    build();
    logf("flags: watching %u words of sGameSys (quests, game, NPC deaths, %d areas' flags, items and objects)",
         unsigned(g_offs.size()), int(kRegions[3].count));
    return true;
}

// Net thread, every ~250 ms in a stage: this world's changes since the last look.
void flags_scan()
{
    uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0);
    int32_t stage = 0;
    DWORD load = stages_last_load(&stage);
    if (!gs || !current_stage_area() || g_offs.empty())
        return;
    bool rebase = load != g_seen_load; // a load put the save's flags back: a new starting point
    if (rebase) {
        if (GetTickCount() - load < 2000)
            return; // let the load settle
        g_seen_load = load;
    }
    int logged = 0;
    DWORD now = GetTickCount() | 1;
    for (size_t i = 0; i < g_offs.size(); ++i) {
        uint32_t v = game::at<uint32_t>(gs, g_offs[i]);
        EnterCriticalSection(&g_lock);
        uint32_t b = g_base[i];
        if (rebase || !g_known[i] || skipped(g_offs[i])) {
            g_base[i] = g_cur[i] = v;
            g_known[i] = 1;
            g_since[i] = 0;
            LeaveCriticalSection(&g_lock);
            continue;
        }
        if (v != g_cur[i]) { // a new value: wait until it holds
            g_cur[i] = v;
            g_since[i] = v != b ? now : 0;
        }
        if (!g_since[i] || int(now - g_since[i]) < int(kStableMs)) {
            LeaveCriticalSection(&g_lock);
            continue;
        }
        FlagEv e = {uint16_t(i), v & ~b, b & ~v};
        g_base[i] = v;
        g_since[i] = 0;
        g_mine.push_back(e);
        LeaveCriticalSection(&g_lock);
        uint32_t sub = 0;
        const char *t = where(uint32_t(i), sub);
        if (logged++ < 20)
            logf("flags: %s word %u: %08X -> %08X (set %08X, cleared %08X), sent to the other players", t, sub, b, v,
                 e.set, e.clr);
    }
    if (rebase)
        logf("flags: starting point taken after a load (stage %d)", stage);
}

uint32_t flags_mine_count()
{
    EnterCriticalSection(&g_lock);
    uint32_t n = uint32_t(g_mine.size());
    LeaveCriticalSection(&g_lock);
    return n;
}

void flags_net_mine(std::vector<FlagEv> &out, uint32_t from_seq, int max)
{
    out.clear();
    EnterCriticalSection(&g_lock);
    for (uint32_t s = from_seq; s >= 1 && s <= g_mine.size() && int(out.size()) < max; ++s)
        out.push_back(g_mine[s - 1]);
    LeaveCriticalSection(&g_lock);
}

// Net thread: another world's change, made here too (atomic bit operations: the game may write the
// same word meanwhile); the starting point takes it so it is not sent back.
void flags_net_apply(const FlagEv &e)
{
    uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0);
    if (!gs || e.index >= g_offs.size() || !current_stage_area())
        return;
    auto *w = reinterpret_cast<volatile LONG *>(gs + g_offs[e.index]);
    if (e.set)
        InterlockedOr(w, LONG(e.set));
    if (e.clr)
        InterlockedAnd(w, LONG(~e.clr));
    EnterCriticalSection(&g_lock);
    g_base[e.index] = (g_base[e.index] | e.set) & ~e.clr;
    g_cur[e.index] = game::at<uint32_t>(gs, g_offs[e.index]);
    g_since[e.index] = 0;
    LeaveCriticalSection(&g_lock);
    uint32_t sub = 0;
    const char *t = where(e.index, sub);
    logf("flags: from another player: %s word %u set %08X cleared %08X", t, sub, e.set, e.clr);
}
