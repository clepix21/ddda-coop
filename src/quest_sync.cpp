// Shared world: the quest list follows everywhere (owner's choice: "Les deux", a short reload now,
// a live way later). Quest flags already go to every world as they change (flags.cpp), but the game
// keeps the quest list (the journal), NPC states, talk history and quest scripts' state elsewhere:
// accepting a quest in one world added it to that world's list only (2026-10-27 21:22, quest.sav).
//
// When a quest flag changes in this world (flags.cpp calls quest_sync_local_change), and the world
// is quiet for 5 s: a save request (mode 7, a fresh snapshot), its quest state (save_quest_state:
// mPlGameData's quest keys in both player data copies) is deflated and sent to every other world
// (net.cpp, through the host). A world that gets another world's quest state: a fresh snapshot of
// itself, the quest state put in, that save loaded (B, 0) and the world reloaded (bit 26), about
// 4 s; it stays where it was. Nothing happens when the state is the one this world already has.
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "game.h"
#include "mod.h"

bool save_machine_request(uint8_t mode, uint8_t slot); // rejoin.cpp
bool save_machine_idle();
bool rejoin_world_quiet();
void rejoin_reload_world();

namespace {

enum Step { IDLE, SNAP_SEND, SNAP_APPLY, LOADING, RELOADING };
const char *kName[] = {"idle", "snapshot to send", "snapshot to update", "loading", "reloading"};
Step g_step = IDLE;
DWORD g_step_at = 0;
volatile LONG g_dirty_at = 0;   // a local quest change waiting (tick), 0 = none
uint32_t g_own_before = 0;
uint32_t g_last_crc = 0;        // the quest state this world has (sent or applied)
uint32_t g_my_list = 0;         // the quest list this world has (save_quest_list_crc), 0 = not known
CRITICAL_SECTION g_lock;
volatile LONG g_ready = 0;
std::string g_pending;          // another world's quest state to take
uint32_t g_pending_crc = 0;
constexpr uintptr_t kQuestManager = 0x018FA4F0, kGetOrCreateQuest = 0x00484D90, kStartQuest = 0x0078D6D0;
constexpr int kLiveMax = 16;
uint32_t g_live_ids[kLiveMax] = {}; // quests to add on the game thread (g_lock)
int g_live_n = 0;
std::vector<uint32_t> g_journal;   // this world's journal, copied on the game thread (g_lock)
bool g_journal_known = false;
constexpr DWORD kQuietMs = 5000, kStepMs = 20000;

std::vector<uint32_t> state_quest_ids(const std::string &state)
{
    std::vector<uint32_t> ids;
    size_t a = state.find("name=\"mQuestData\""), b = state.find("name=\"mQuestSubData\"", a);
    if (a == std::string::npos)
        return ids;
    const std::string key = "name=\"mQuestID\" value=\"";
    for (size_t p = state.find(key, a); p != std::string::npos && (b == std::string::npos || p < b);
         p = state.find(key, p + 1)) {
        long v = strtol(state.c_str() + p + key.size(), nullptr, 10);
        if (v != -1)
            ids.push_back(uint32_t(v));
    }
    return ids;
}

// Another world's quest state that only adds quests to this world's journal: those quests are added
// live (no reload). Returns false when anything else differs (a quest finished or gone): reload.
bool try_live(const std::string &theirs)
{
    std::vector<uint32_t> want = state_quest_ids(theirs), have;
    EnterCriticalSection(&g_lock);
    bool known = g_journal_known;
    have = g_journal;
    LeaveCriticalSection(&g_lock);
    if (!known || want.empty())
        return false;
    for (uint32_t h : have)
        if (std::find(want.begin(), want.end(), h) == want.end())
            return false; // a quest this world has and the other has not
    int added = 0;
    for (uint32_t w : want)
        if (std::find(have.begin(), have.end(), w) == have.end()) {
            quest_live_queue(w);
            ++added;
        }
    logf("quests: the other player's quest list adds %d quest(s) to this world's %u: added live, no reload", added,
         unsigned(have.size()));
    return true;
}

void go(Step s, DWORD now)
{
    logf("quests: %s -> %s", kName[g_step], kName[s]);
    g_step = s;
    g_step_at = now | 1;
}

} // namespace

void quest_sync_init()
{
    InitializeCriticalSection(&g_lock);
    InterlockedExchange(&g_ready, 1);
}

void quest_sync_local_change() { InterlockedExchange(&g_dirty_at, LONG(GetTickCount() | 1)); }

// Net thread: another world's quest state (inflated).
void quest_sync_received(const std::string &state, uint32_t crc)
{
    if (!g_ready || crc == g_last_crc)
        return;
    EnterCriticalSection(&g_lock);
    g_pending = state;
    g_pending_crc = crc;
    LeaveCriticalSection(&g_lock);
    logf("quests: another player's quest state (%08X, %u bytes): this world takes it at the next quiet moment", crc,
         unsigned(state.size()));
}

// Net thread, ~every 33 ms, shared world.
void quest_sync_tick()
{
    if (!g_ready || !net_shared())
        return;
    DWORD now = GetTickCount();
    if (g_step != IDLE && int(now - g_step_at) > int(kStepMs)) {
        logf("quests: %s took too long, given up", kName[g_step]);
        storage_set_override_save({});
        g_step = IDLE;
        return;
    }
    std::vector<uint8_t> file;
    uint32_t version = 0;
    switch (g_step) {
    case IDLE: {
        bool quiet = rejoin_world_quiet() && !rejoin_busy() && !revive_down();
        EnterCriticalSection(&g_lock);
        bool pending = !g_pending.empty();
        LeaveCriticalSection(&g_lock);
        LONG dirty = g_dirty_at;
        if (!quiet)
            return;
        if (pending) { // only new quests: added live
            EnterCriticalSection(&g_lock);
            std::string theirs = g_pending;
            uint32_t crc = g_pending_crc;
            LeaveCriticalSection(&g_lock);
            if (try_live(theirs)) {
                g_last_crc = crc;
                g_my_list = save_quest_list_crc(theirs);
                EnterCriticalSection(&g_lock);
                g_pending.clear();
                LeaveCriticalSection(&g_lock);
                return;
            }
        }
        if (pending || (dirty && int(now - DWORD(dirty)) > int(kQuietMs))) {
            storage_own_raw(file, g_own_before);
            if (save_machine_request(7, 0)) {
                if (!pending)
                    InterlockedExchange(&g_dirty_at, 0);
                go(pending ? SNAP_APPLY : SNAP_SEND, now);
            }
        }
        return;
    }
    case SNAP_SEND:
    case SNAP_APPLY: {
        if (!save_machine_idle() || int(now - g_step_at) < 500)
            return;
        if (!storage_own_raw(file, version) || version == g_own_before)
            return; // the write comes back through save_guard
        std::string xml, state;
        uint32_t head[8];
        if (!save_unpack(file, xml, head) || !save_quest_state(xml, state)) {
            logf("quests: the snapshot could not be read");
            g_step = IDLE;
            return;
        }
        if (g_step == SNAP_SEND) {
            uint32_t crc = save_crc(state.data(), state.size()) | 1, list = save_quest_list_crc(state);
            if (list == g_my_list) { // talks, NPCs: nothing worth a reload elsewhere
                g_last_crc = crc;
                g_step = IDLE;
                return;
            }
            g_my_list = list;
            if (crc != g_last_crc) {
                std::vector<uint8_t> blob;
                if (save_deflate(state, blob)) {
                    g_last_crc = crc;
                    net_quest_publish(blob, crc);
                    logf("quests: this world's quest state %08X (%u bytes, %u deflated) sent to the other players", crc,
                         unsigned(state.size()), unsigned(blob.size()));
                }
            }
            g_step = IDLE;
            return;
        }
        // SNAP_APPLY: this world's fresh snapshot with the other world's quest state, then a reload
        if (net_role() == NET_HOST) { // the host's world shows the guests' characters: the merged save
            uint32_t loaded = 0, own = 0;
            bool stable = false, arrivals = false;
            storage_host_sigs(&loaded, &stable, &own, &arrivals);
            std::vector<uint8_t> merged;
            uint32_t v = 0;
            if (!stable)
                return; // the merge of the new snapshot is being built
            if (storage_own_save(merged, v) && save_unpack(merged, xml, head)) {
            } else {
                return;
            }
        }
        EnterCriticalSection(&g_lock);
        std::string theirs = g_pending;
        uint32_t crc = g_pending_crc;
        LeaveCriticalSection(&g_lock);
        uint32_t mine_list = save_quest_list_crc(state), their_list = save_quest_list_crc(theirs);
        if (mine_list == their_list) { // the same quest list here already: no reload
            logf("quests: the other player's quest list is this world's already: no reload");
            g_my_list = mine_list;
            g_last_crc = crc;
            EnterCriticalSection(&g_lock);
            g_pending.clear();
            LeaveCriticalSection(&g_lock);
            g_step = IDLE;
            return;
        }
        std::vector<uint8_t> out;
        if (!save_put_quest_state(xml, theirs) || !save_pack(xml, head, out)) {
            logf("quests: the other player's quest state could not be put in this world's save");
            EnterCriticalSection(&g_lock);
            g_pending.clear();
            LeaveCriticalSection(&g_lock);
            g_step = IDLE;
            return;
        }
        storage_set_override_save(out);
        g_last_crc = crc;
        g_my_list = their_list;
        if (save_machine_request(0xB, 0))
            go(LOADING, now);
        return;
    }
    case LOADING:
        if (!save_machine_idle() || int(now - g_step_at) < 500)
            return;
        rejoin_reload_world();
        go(RELOADING, now);
        return;
    case RELOADING: {
        int32_t stage = 0;
        DWORD load = stages_last_load(&stage);
        if (!load || int(load - g_step_at) < 0)
            return;
        storage_set_override_save({});
        EnterCriticalSection(&g_lock);
        g_pending.clear();
        LeaveCriticalSection(&g_lock);
        logf("quests: this world reloaded with the other player's quest state %08X", g_last_crc);
        g_step = IDLE;
        return;
    }
    }
}

bool quest_sync_busy() { return g_step != IDLE; }

// Live quest list (study, 2026-10-28): sQuestManager [0x018FA4F0] keeps the journal as an MtArray at
// +0x128 (count +0x12C, capacity +0x130, items +0x138: quest objects, id at +0x30). Accepting a quest
// in quest.sav wrote the count from FUN_00484CD0, called by FUN_00484D90(manager, id) (__stdcall,
// `ret 8`; a cdecl call crashed the game): "get or create": returns the quest when listed, else
// creates it (0x138 bytes), loads its resources and appends it. id = (quest number << 16) |
// sub-number (0x350000 there). The quest scripts' "start quest" orders (FUN_0078FBB0 cases 0xE and
// 0x1F) then run FUN_0078D6D0 (EDX = quest): quest+0x2C |= 2 and FUN_00487500 (ECX = manager).

void quest_live_queue(uint32_t id)
{
    EnterCriticalSection(&g_lock);
    if (g_live_n < kLiveMax)
        g_live_ids[g_live_n++] = id;
    LeaveCriticalSection(&g_lock);
}

int quest_live_count()
{
    uintptr_t qm = game::at<uintptr_t>(kQuestManager, 0);
    return qm ? int(game::at<uint32_t>(qm, 0x12C)) : -1;
}

// Game thread, every frame of the Arisen: add the queued quests, then copy the journal's ids.
void quest_live_tick()
{
    if (!g_ready)
        return;
    uintptr_t qm = game::at<uintptr_t>(kQuestManager, 0);
    uint32_t ids[kLiveMax];
    EnterCriticalSection(&g_lock);
    int n = qm ? g_live_n : 0;
    memcpy(ids, g_live_ids, sizeof ids);
    if (qm)
        g_live_n = 0;
    LeaveCriticalSection(&g_lock);
    for (int k = 0; k < n; ++k) {
        int before = quest_live_count();
        using GetOrCreate_t = uintptr_t(__stdcall *)(uintptr_t, int32_t);
        uintptr_t q = reinterpret_cast<GetOrCreate_t>(kGetOrCreateQuest)(qm, int32_t(ids[k]));
        uint32_t flags_before = q ? game::at<uint32_t>(q, 0x2C) : 0;
        if (q) {
            uintptr_t fn = kStartQuest, quest = q;
            asm volatile("call *%%ecx"
                         : "+c"(fn), "+d"(quest)
                         :
                         : "eax", "memory", "cc");
        }
        logf("quests: live add %08X: quest object %p, flags %08X -> %08X, journal %d -> %d", unsigned(ids[k]),
             (void *)q, flags_before, q ? game::at<uint32_t>(q, 0x2C) : 0, before, quest_live_count());
    }
    std::vector<uint32_t> j;
    if (qm) {
        uint32_t count = game::at<uint32_t>(qm, 0x12C);
        uintptr_t items = game::at<uintptr_t>(qm, 0x138);
        for (uint32_t k = 0; items && k < count && k < 256; ++k)
            if (uintptr_t q = game::at<uintptr_t>(items, k * 4))
                j.push_back(game::at<uint32_t>(q, 0x30));
    }
    EnterCriticalSection(&g_lock);
    g_journal.swap(j);
    g_journal_known = qm != 0;
    LeaveCriticalSection(&g_lock);
}
