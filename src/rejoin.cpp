// Join during play. A friend's character reaches the host while the host already plays: the host's
// world still shows the pawn it had. The game rebuilds the world from a save in two places only (a
// game loaded from the title, a retry after death), so the host goes through the same steps:
//   1. a save request to the save/load machine [0x018FA518] (mode 7, slot 0: it takes a fresh
//      snapshot of the world; mode 2 slot 3, the autosave's request, writes the last checkpoint's
//      snapshot, which put the host back where it had loaded, 2026-10-27 13:15), so the host keeps
//      its place and progress; the write goes through save_guard (un-merged);
//   2. once the merged save is rebuilt from that write, a load request (mode B, slot 0: what "load
//      game" at the title sends). The game reads DDDA.sav (served merged by save_guard) into its
//      in-memory copy [0x018FA4F8]+0x50 and restores the party from it;
//   3. sGameSys +0xB8840 bit 26: the stage update reloads the world from that copy (FUN_004FB7B0 ->
//      FUN_00498D30 -> stage load from 0x004FB924), as after "Ferrystone"-like items.
// A guest in game whose host save holds other characters than the one its game read (the host's
// world changed) does steps 2 and 3 with the host's save. Requests are written the way the game's
// menu writes them (FUN_00532110), only when the machine is idle (+0x648 = 0). Found 2026-10-27.
#include <windows.h>

#include "game.h"
#include "mod.h"

namespace {

constexpr uintptr_t kSaveMachine = 0x018FA518;
constexpr uint32_t kReloadBit = 1u << 26, kGameOverBit = 1u << 25;
constexpr DWORD kQuietMs = 3000;   // the world must be quiet that long before a reload starts
constexpr DWORD kStepMs = 15000;   // a step that takes longer is given up (tried again later)

enum Step { IDLE, SAVING, MERGING, LOADING, RELOADING };
const char *kStepName[] = {"idle", "saving", "merging", "loading", "reloading"};
Step g_step = IDLE;
DWORD g_step_at = 0, g_quiet_since = 0, g_retry_at = 0;
uint32_t g_target = 0, g_own_before = 0;
volatile LONG g_loaded_sig = 0;

uintptr_t machine() { return game::at<uintptr_t>(kSaveMachine, 0); }
bool machine_idle()
{
    uintptr_t sm = machine();
    return sm && game::at<uint8_t>(sm, 0x648) == 0;
}

bool machine_request(uint8_t mode, uint8_t slot)
{
    uintptr_t sm = machine();
    if (!sm || game::at<uint8_t>(sm, 0x648) != 0)
        return false;
    game::at<int32_t>(sm, 0xA44) = -1;
    game::at<int32_t>(sm, 0xA48) = -1;
    game::at<uint8_t>(sm, 0x638) = slot;
    game::at<uint8_t>(sm, 0x63B) = 1;
    game::at<int32_t>(sm, 0xA4C) = 0;
    game::at<int32_t>(sm, 0xA50) = 0;
    game::at<uint8_t>(sm, 0x829) = 1;
    game::at<uint8_t>(sm, 0x64D) = 0;
    game::at<uint8_t>(sm, 0x82A) = mode == 0x12;
    game::at<uint8_t>(sm, 0x649) = 0;
    MemoryBarrier();
    game::at<uint8_t>(sm, 0x648) = mode; // last: the machine starts on it
    return true;
}

// In a stage, not jumping, no talk or cutscene, no game over or reload pending, the Arisen there.
bool world_quiet()
{
    uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0), st = current_stage_area();
    if (!gs || !st || !possess_arisen())
        return false;
    return game::at<int32_t>(st, 0x808) == 1 && game::at<int32_t>(gs, game::kGsEvent) == 256 &&
           (game::at<uint32_t>(gs, 0xB8840) & (kReloadBit | kGameOverBit)) == 0 && machine_idle();
}

void go(Step s, DWORD now)
{
    logf("rejoin: %s -> %s", kStepName[g_step], kStepName[s]);
    g_step = s;
    g_step_at = now | 1;
}

void give_up(DWORD now, const char *why)
{
    logf("rejoin: %s gave up (%s), trying again in 20 s", kStepName[g_step], why);
    g_step = IDLE;
    g_retry_at = (now + 20000) | 1;
}

} // namespace

bool rejoin_busy() { return g_step != IDLE; }
uint32_t rejoin_loaded_sig() { return uint32_t(g_loaded_sig); }

void rejoin_tick(int role, uint32_t host_loaded_sig)
{
    DWORD now = GetTickCount();
    uint32_t want = 0, loaded = 0, own = 0;
    bool stable = false, read_any = false, arrivals = true;
    if (role == NET_HOST) {
        want = storage_host_sigs(&loaded, &stable, &own, &arrivals);
        InterlockedExchange(&g_loaded_sig, LONG(loaded));
    } else if (role == NET_GUEST) {
        want = storage_guest_sigs(&loaded, &read_any);
        stable = read_any && want == host_loaded_sig; // the host shows those characters already
    } else {
        return;
    }
    // signed: g_step_at is `now | 1`, one ms ahead of a later `now` in the same tick (an unsigned
    // difference gave up a step 8 ms after it began, 2026-10-27 13:32)
    if (g_step != IDLE && int(now - g_step_at) > int(kStepMs)) {
        give_up(now, "too long");
        return;
    }
    switch (g_step) {
    case IDLE: {
        bool quiet = world_quiet();
        // only a character the world does not show yet is worth a reload: a guest that leaves keeps
        // its look on its pawn until the next load (save_guard keeps un-merging the host's writes),
        // and coming back with the same character needs nothing (seen in the soak test: a drop made
        // the host reload, 2026-10-27 06:52)
        if (!quiet || want == loaded || want == 0 || !arrivals || !stable) {
            g_quiet_since = 0;
            return;
        }
        if (!g_quiet_since)
            g_quiet_since = now | 1;
        if (int(now - g_quiet_since) < int(kQuietMs) || (g_retry_at && int(now - g_retry_at) < 0))
            return;
        g_target = want;
        g_own_before = own;
        logf("rejoin: the %s's world shows characters %08X, the save holds %08X: %s", role == NET_HOST ? "host" : "guest",
             loaded, want, role == NET_HOST ? "save, then load the merged save" : "load the host's save");
        if (role == NET_HOST ? machine_request(7, 0) : machine_request(0xB, 0))
            go(role == NET_HOST ? SAVING : LOADING, now);
        return;
    }
    case SAVING: // the save runs (write, then a read back); then the merge of the new own save
        if (machine_idle() && int(now - g_step_at) > 500)
            go(MERGING, now);
        return;
    case MERGING:
        if (!stable)
            return; // storage_merge_tick (net thread) rebuilds it
        if (own == g_own_before)
            logf("rejoin: the save request wrote nothing new (own save version %u)", own);
        if (machine_request(0xB, 0))
            go(LOADING, now);
        return;
    case LOADING:
        if (!machine_idle() || int(now - g_step_at) < 500)
            return;
        if (loaded != g_target) {
            give_up(now, "the game did not read the expected save");
            return;
        }
        if (uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0))
            InterlockedOr(reinterpret_cast<volatile LONG *>(gs + 0xB8840), LONG(kReloadBit));
        go(RELOADING, now);
        return;
    case RELOADING: {
        int32_t stage = 0;
        DWORD load = stages_last_load(&stage);
        if (load && int(load - g_step_at) >= 0) {
            logf("rejoin: world reloaded with characters %08X", loaded);
            g_step = IDLE;
        }
        return;
    }
    }
}

// Shared with roam.cpp (free roam): the save/load machine and the quiet-world test.
bool save_machine_request(uint8_t mode, uint8_t slot) { return machine_request(mode, slot); }
bool save_machine_idle() { return machine_idle(); }
bool rejoin_world_quiet() { return world_quiet(); }
void rejoin_reload_world()
{
    if (uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0))
        InterlockedOr(reinterpret_cast<volatile LONG *>(gs + 0xB8840), LONG(kReloadBit));
}
