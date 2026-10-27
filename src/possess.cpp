// Pawn possession: other players' pads drive pawns through the game's own command builder.
//
// The game merges all four pad slots (sQlocInput::mPad[4] raw, mPadInfo[4] mapped) into the one
// pad-driven character. Around each character's UpdateCommands call we give the builder a
// private, per-thread copy of the input object (see input_redirect.cpp):
//   a possessed pawn (uCmc, mCmcId 0 = main pawn, 1 and 2 = hired pawns): command mode forced to 1
//     (pad), every slot except its controller's replaced by "nothing pressed" bytes in the copy;
//   Arisen (uPlayer), always: every slot that drives a pawn replaced the same way.
// The shared object is never written, except the Arisen's toggle bytes copied back.
// See NOTES.md, "Input path", "Phase 5" and "2 to 4 players".
// Keyboard and mouse read as idle for possessed pawns (player 1 may play on keyboard).
//
// Controllers: local split screen (Back + Start, auto_possess) drives the main pawn from pad slot
// p2_pad; over the network (net.cpp) guest k drives the pawn the host gave it from pad slot k + 1.
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "game.h"
#include "mod.h"

namespace {

constexpr uintptr_t kInputSize = 0x2B38; // sQlocInput DTI size
constexpr uintptr_t kPadBase = 0x3C;     // sPad::mPad[4]
constexpr uintptr_t kPadSize = 0x400;
constexpr uintptr_t kToggles = 0x1085; // 3 bytes the builder writes back into the input object
constexpr uintptr_t kTogglesLen = 3;

int g_p2 = 1;   // pad slot of player 2 (local split screen)
int g_idle = 3; // a slot with no player, read once as "nothing pressed"
DWORD g_auto_ms = 0; // possess automatically this long after load (tests); 0 = off
DWORD g_buf_tls = TLS_OUT_OF_INDEXES;
bool g_block_saves = false; // tests only: block every Steam Cloud write
bool g_debug_log = false;   // per-character probe lines in coop.log
bool g_bridge = false;      // tests only: bridge_in.txt / bridge_out.txt
int g_bridge_sign = 1;      // sign of the camera "right" vector used by the bridge's goto
volatile uintptr_t g_arisen = 0;
volatile uintptr_t g_pawn[kPawns] = {}; // by mCmcId: last seen, for the bridge and the net
// when each was last updated: after a reload the old objects are freed, and a pointer kept from
// before crashed the guest's camera (2026-10-27, DDDA.exe+0x3C8C08 reading the followed pawn)
volatile DWORD g_arisen_tick = 0, g_pawn_tick[kPawns] = {};
constexpr DWORD kFreshMs = 500;
uintptr_t fresh(uintptr_t p, DWORD tick) { return p && GetTickCount() - tick < kFreshMs ? p : 0; }
NetRole g_cfg_role = NET_OFF;
char g_cfg_peer[64] = "127.0.0.1";
int g_cfg_port = 27101;
int g_win[4] = {0, 0, 0, 0}; // window=x,y,w,h (tests); w = 0 means unset

// One controller per pawn (index = mCmcId): which pad slot drives it, and its camera.
struct Ctl {
    volatile LONG on;
    int slot;
    int camera_peer; // net.cpp peer whose camera turns the stick (-1 = the real camera)
    uintptr_t forced; // the pawn put in pad mode, to hand back to its AI
    uint8_t toggles[kTogglesLen];
};
Ctl g_ctl[kPawns] = {};

// "Nothing pressed" bytes: slot g_idle copied while no player uses it (a connected, neutral pad
// in tests, or no pad at all). With four players there is no free slot left, so it is kept.
uint8_t g_idle_pad[kPadSize], g_idle_info[game::kPadInfoSize];
volatile LONG g_idle_valid = 0;

uint8_t *thread_buffer()
{
    auto b = static_cast<uint8_t *>(TlsGetValue(g_buf_tls));
    if (!b) {
        b = static_cast<uint8_t *>(VirtualAlloc(nullptr, kInputSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        TlsSetValue(g_buf_tls, b);
    }
    return b;
}

bool slot_in_use(int slot)
{
    for (const Ctl &c : g_ctl)
        if (c.on && c.slot == slot)
            return true;
    return false;
}

void keep_idle(const uint8_t *input)
{
    if (g_idle_valid || slot_in_use(g_idle))
        return;
    memcpy(g_idle_pad, input + kPadBase + g_idle * kPadSize, kPadSize);
    memcpy(g_idle_info, input + game::kPadInfo + g_idle * game::kPadInfoSize, game::kPadInfoSize);
    InterlockedExchange(&g_idle_valid, 1);
}

void hide_slot(uint8_t *copy, int i)
{
    // the idle slot read live while it is free (as before 2026-10-26); the copy kept from it only
    // once a third guest uses that slot (a kept copy may carry edge bits: an Arisen "press" was
    // seen once in a single-copy run with the kept copy always used)
    if (g_idle_valid && slot_in_use(g_idle)) {
        memcpy(copy + kPadBase + i * kPadSize, g_idle_pad, kPadSize);
        memcpy(copy + game::kPadInfo + i * game::kPadInfoSize, g_idle_info, game::kPadInfoSize);
    } else {
        memcpy(copy + kPadBase + i * kPadSize, copy + kPadBase + g_idle * kPadSize, kPadSize);
        memcpy(copy + game::kPadInfo + i * game::kPadInfoSize, copy + game::kPadInfo + g_idle * game::kPadInfoSize,
               game::kPadInfoSize);
    }
}

// mCmcId of a party pawn (0..2), or -1
int pawn_index(uintptr_t ch, uintptr_t vt)
{
    if (vt != game::kVtCmc)
        return -1;
    int id = game::at<int>(ch, game::kCmcId);
    return id >= 0 && id < kPawns ? id : -1;
}

constexpr int ARISEN = 1;            // possess_pre tokens: ARISEN, or PAWN_BASE + mCmcId
constexpr int PAWN_BASE = 2;

} // namespace

void possess_load_config()
{
    g_buf_tls = TlsAlloc();
    FILE *f = fopen("coop.ini", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            int v;
            if (sscanf(line, "p2_pad=%d", &v) == 1 && v >= 0 && v < 4)
                g_p2 = v;
            else if (sscanf(line, "idle_pad=%d", &v) == 1 && v >= 0 && v < 4)
                g_idle = v;
            else if (sscanf(line, "auto_possess=%d", &v) == 1 && v >= 0)
                g_auto_ms = DWORD(v);
            else if (sscanf(line, "block_saves=%d", &v) == 1)
                g_block_saves = v != 0;
            else if (sscanf(line, "debug_log=%d", &v) == 1)
                g_debug_log = v != 0;
            else if (strncmp(line, "role=host", 9) == 0)
                g_cfg_role = NET_HOST;
            else if (strncmp(line, "role=guest", 10) == 0)
                g_cfg_role = NET_GUEST;
            else if (sscanf(line, "peer=%63s", g_cfg_peer) == 1)
                ;
            else if (sscanf(line, "window=%d,%d,%d,%d", &g_win[0], &g_win[1], &g_win[2], &g_win[3]) == 4)
                ;
            else if (sscanf(line, "port=%d", &v) == 1 && v > 0 && v < 65536)
                g_cfg_port = v;
            else if (sscanf(line, "bridge=%d", &v) == 1)
                g_bridge = v != 0;
            else if (sscanf(line, "bridge_sign=%d", &v) == 1)
                g_bridge_sign = v < 0 ? -1 : 1;
        }
        fclose(f);
    }
    if (g_idle == g_p2)
        g_idle = (g_p2 + 3) % 4;
    logf("config: p2 pad slot %d, idle slot %d, auto_possess %lu ms, block_saves %d%s, debug_log %d", g_p2, g_idle,
         g_auto_ms, g_block_saves, g_cfg_role == NET_GUEST ? " (guest: saves always blocked)" : "", g_debug_log);
}

// Pawn `pawn` (mCmcId) driven from pad `slot` (on), or given back to its AI (off).
void possess_pawn(int pawn, bool on, int slot, int camera_peer)
{
    if (pawn < 0 || pawn >= kPawns)
        return;
    if (on && !input_redirect_active()) {
        logf("possess: refused, input redirection is not installed");
        return;
    }
    Ctl &c = g_ctl[pawn];
    if (on) {
        c.slot = slot;
        c.camera_peer = camera_peer;
    }
    if (InterlockedExchange(&c.on, on ? 1 : 0) != (on ? 1 : 0))
        logf("possess: %s (pawn %d%s, pad slot %d)", on ? "ON" : "OFF (back to its AI)", pawn,
             pawn == 0 ? ", the main pawn" : "", c.slot);
}

// Local split screen: player 2 (pad p2_pad) and the main pawn.
void possess_set(bool on) { possess_pawn(0, on, g_p2, -1); }
bool possess_on() { return g_ctl[0].on != 0; }
bool possess_pawn_on(int pawn) { return pawn >= 0 && pawn < kPawns && g_ctl[pawn].on != 0; }
// The guest's game shows the host's world: its autosaves (every area change) would write the host's
// stage and positions into the guest's own save, so a guest never saves.
bool cfg_block_saves() { return g_block_saves || g_cfg_role == NET_GUEST; }
bool cfg_debug_log() { return g_debug_log; }
bool cfg_bridge() { return g_bridge; }
NetRole cfg_role() { return g_cfg_role; }
bool cfg_window(int &x, int &y, int &w, int &h)
{
    x = g_win[0], y = g_win[1], w = g_win[2], h = g_win[3];
    return w > 0 && h > 0;
}
const char *cfg_peer() { return g_cfg_peer; }
int cfg_port() { return g_cfg_port; }
int possess_bridge_sign() { return g_bridge_sign; }
// The party, as long as the game keeps updating them (0 while a stage loads, or after a reload
// until the new objects run).
uintptr_t possess_arisen() { return fresh(g_arisen, g_arisen_tick); }
DWORD possess_last_update() { return g_arisen_tick; }
uintptr_t possess_main_pawn() { return fresh(g_pawn[0], g_pawn_tick[0]); }
uintptr_t possess_pawn_ptr(int pawn) { return pawn >= 0 && pawn < kPawns ? fresh(g_pawn[pawn], g_pawn_tick[pawn]) : 0; }
DWORD possess_auto_ms() { return g_auto_ms; }
int possess_p2_pad() { return g_p2; }

// Called before UpdateCommands(ch). Returns 0, or a token to pass to possess_post() with *copy.
int possess_pre(uintptr_t ch, uint8_t **out_copy)
{
    uintptr_t vt = game::at<uintptr_t>(ch, 0);
    int pawn = pawn_index(ch, vt);
    if (vt == game::kVtPlayer) {
        g_arisen = ch;
        g_arisen_tick = GetTickCount();
        net_capture(ch, 0);
        enemies_run_queued(ch);
        exp_sync_apply(); // shared world: the exp other players earned
    } else if (pawn >= 0) {
        g_pawn[pawn] = ch;
        g_pawn_tick[pawn] = GetTickCount();
        net_capture(ch, 1 + pawn);
    }
    if (!input_redirect_active())
        return 0;
    for (Ctl &c : g_ctl) // hand pawns back to their AI
        if (!c.on && c.forced == ch) {
            game::at<uint8_t>(ch + game::kCmd, game::kCmdMode) = 0;
            c.forced = 0;
        }
    // set by net_start (late with net_late=1: walks first); a guest exploring on its own (roam.cpp)
    // plays its own Arisen with its own controller
    bool guest = net_role() == NET_GUEST && !net_guest_roaming() && !net_shared();
    // shared world: a pawn that shows another player is that player's puppet; back to its AI after
    bool puppet = pawn >= 0 && net_puppet(1 + pawn);
    static uintptr_t puppet_forced[kPawns] = {};
    if (pawn >= 0 && !puppet && puppet_forced[pawn] == ch) {
        game::at<uint8_t>(ch + game::kCmd, game::kCmdMode) = 0;
        puppet_forced[pawn] = 0;
    }
    int who = 0;
    if (vt == game::kVtPlayer)
        who = ARISEN; // other players never drive the Arisen, possessed or not
    else if (pawn >= 0 && (guest || puppet || g_ctl[pawn].on))
        who = PAWN_BASE + pawn; // on the guest, the whole party are puppets of the host
    if (!who)
        return 0;
    uintptr_t in = game::at<uintptr_t>(0, game::kInput);
    if (!in)
        return 0;
    uint8_t *copy = thread_buffer();
    if (!copy)
        return 0;
    memcpy(copy, reinterpret_cast<const void *>(in), kInputSize);
    keep_idle(copy);
    if (guest || (puppet && who != ARISEN)) { // puppets: no local input at all, no AI
        for (int i = 0; i < 4; ++i)
            if (i != g_idle)
                hide_slot(copy, i);
        if (who != ARISEN)
            game::at<uint8_t>(ch + game::kCmd, game::kCmdMode) = 1;
        if (puppet)
            puppet_forced[pawn] = ch;
        *out_copy = copy;
        input_redirect_set(reinterpret_cast<uintptr_t>(copy), true);
        return who;
    }
    if (who == ARISEN) {
        hide_slot(copy, g_p2); // player 2 never drives the Arisen, possessed or not
        for (const Ctl &c : g_ctl)
            if (c.on)
                hide_slot(copy, c.slot);
        if (net_role() == NET_HOST) // the guests' pad slots, before and after they take a pawn
            for (int i = 1; i < 4; ++i)
                hide_slot(copy, i);
        *out_copy = copy;
        input_redirect_set(reinterpret_cast<uintptr_t>(copy), false);
        return who;
    }
    Ctl &c = g_ctl[pawn];
    for (int i = 0; i < 4; ++i)
        if (i != c.slot)
            hide_slot(copy, i);
    memcpy(copy + kToggles, c.toggles, kTogglesLen);
    game::at<uint8_t>(ch + game::kCmd, game::kCmdMode) = 1;
    c.forced = ch;
    *out_copy = copy;
    // a guest's stick is relative to that guest's own camera (net.cpp), not the host's
    input_redirect_set(reinterpret_cast<uintptr_t>(copy), true,
                       c.camera_peer >= 0 ? net_host_guest_camera_holder(c.camera_peer) : 0);
    return who;
}

void possess_post(int who, uint8_t *copy)
{
    input_redirect_set(0, false);
    if (who == ARISEN) {
        uintptr_t in = game::at<uintptr_t>(0, game::kInput);
        if (in)
            memcpy(reinterpret_cast<void *>(in + kToggles), copy + kToggles, kTogglesLen);
    } else if (who >= PAWN_BASE && who < PAWN_BASE + kPawns) {
        memcpy(g_ctl[who - PAWN_BASE].toggles, copy + kToggles, kTogglesLen);
    }
}

// The token's party index for the net code: 0 = Arisen, 1 + mCmcId for a pawn, -1 otherwise.
int possess_party_index(int who) { return who == ARISEN ? 0 : who >= PAWN_BASE ? 1 + (who - PAWN_BASE) : -1; }
