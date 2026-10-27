// Co-op over the network, 2 to 4 players: host-authoritative sync (milestones 1-6).
//
// coop.ini: role=host|guest, peer=<host address, guest only>, port=<host UDP port, default 27101>,
// pawn_order=<mCmcId list, default 1,2,0> (which pawn the 1st, 2nd and 3rd guest replace: hired
// pawns first, the main pawn last).
//  - guest -> host, ~60 Hz: INPUT = the guest's own controller (slot 0 on the guest PC), only once
//    the guest is in game. Guest k (1..3) gets pad slot k on the host and the next free pawn of
//    pawn_order that exists; its pad there drives that pawn (possess.cpp). Back, Start and the
//    right stick are dropped (no pause menu, no camera turning on the host).
//  - host -> guests, ~30 Hz: SNAP = for the Arisen and the 3 pawns: transform (uCoord +0x40 pos,
//    +0x50 quat), facing angle (+0x2578/+0x257C/+0x2654, found by memory diff), and the command
//    object (+0x32B4: pressed/held/released bits and stick vectors). Pressed and released bits are
//    OR-accumulated between sends so a short press is never lost.
//  - guest: after its own UpdateCommands (all local input hidden), writes the host's command
//    object into its puppets, so their own action engine plays the same moves and animations;
//    then corrects drift: position eased toward the host (snapped if far), facing forced.
// Transport (coop.ini transport=): steam (dist/coop.ini; steam_link.cpp, friends find each other
// through Steam, no address or port) or udp (the default; peer=<address>, port=; LAN, one-PC tests).
// A guest silent for 10 s is dropped: its pawn goes back to its AI until it returns. Each guest's
// SNAP says which pawn is its own (its camera follows it).
//  - host -> guest: SAVE_CHUNK = the host's latest DDDA.sav (save_guard.cpp keeps it) in 1 KB
//    pieces with a CRC32 of the whole; guest -> host, 4 a second: SAVE_NEED = the version it has
//    complete and up to 200 missing pieces. A new host save (an autosave at a gate) is sent again.
//    The guest's game then reads the host's save when it loads a game.
//  - guest -> host, every 500 ms until the host has it: CHAR_CHUNK = the guest's own character (its
//    Arisen's mEdit and mParam from its own save, ~17 KB) in 1 KB pieces with a CRC32. The host
//    merges it over the pawn that guest drives (save_guard.cpp); each SNAP tells the guest the CRC
//    of the character the host has for it.
//  - host -> guest, every 500 ms until acked: PROGRESS = what that guest's character gained in the
//    host's world (progress.cpp: exp, discipline, items, per load), in 1 KB pieces with a CRC32;
//    guest -> host: PROGRESS_ACK = the CRC it got (it keeps the gains in coop_progress\pending.txt).
// Shared world (coop.ini shared_world=1): every player plays its own character in its own copy of
// the host's world (a guest loads the host's save with its own character as the Arisen, roam.cpp).
//  - guest -> host, ~60 Hz: PSTATE = its Arisen's state and its area;
//  - the host puts each guest's state on the pawn holding that guest's character, and its SNAP
//    relays every player's state with the area each one is in (stage_of); each game moves the pawn
//    that shows another player like a puppet (commands, position, facing, HP) while that player is
//    in the same area, and runs its own AI pawns. Enemies of the host's area stay the host's.
//  - host -> guest, ~30 pages/s: ENEMIES = every live enemy (enemies.cpp) as class name hash, spawn
//    point (where the host first saw it), position, quaternion, facing and HP ([[em+0x1D18]]+8/+0xC),
//    up to 20 per page. The guest pairs each of its own enemies with the host's one of the same class
//    that spawned within 40 u of it, then writes the host's transform and HP after its finalize, so
//    the host's fight decides where enemies stand and how hurt they are.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "game.h"
#include "mod.h"

void guest_talk_mirror(DWORD now); // below: the guest mirrors the host's pad during talks

namespace {

enum MsgType : uint8_t { MSG_INPUT = 1, MSG_SNAP = 2, MSG_ENEMIES = 3, MSG_SAVE_CHUNK = 4, MSG_SAVE_NEED = 5, MSG_CHAR_CHUNK = 6,
                       MSG_PROGRESS = 7, MSG_PROGRESS_ACK = 8, MSG_PSTATE = 9, MSG_LOOT = 10 };

constexpr uintptr_t kYawA = 0x2578, kYawB = 0x257C, kYawC = 0x2654; // facing angle (radians)
// A big correction must move every copy the movement code keeps, or the next collision pass pulls
// the character back (write watchpoints, 2026-10-26): FUN_0076C540(physics ch+0x10E0, pos ch+0x40,
// destination ch+0xE70, ...) sweeps the collision object (ch+0x19C0, FUN_0079E200) toward the
// destination at +0xE70 (+0xE80 holds the same point) and writes the result back to +0x40; +0x1990
// is the previous position.
constexpr uintptr_t kPosCopies[] = {0xE70, 0xE80, 0x1990};
// Command object fields written by the stick/button builders (FUN_00B51EE0, FUN_00B52880):
// +0x0C..+0x2B stick directions, magnitudes and move state (0-4), +0x2C..+0x3B button bits,
// +0x40..+0x5B stick vectors, +0x88..+0x8F. +0x08 (owner) and +0x60 (mode) are never copied.
constexpr uintptr_t kCmdCopyFrom = 0x0C, kCmdCopyLen = 0x54; // cmd +0x0C .. +0x5F
constexpr uintptr_t kCmdExtraFrom = 0x88, kCmdExtraLen = 0x08;

#pragma pack(push, 1)
struct Header {
    uint32_t magic; // 'DDCO'
    uint8_t type;
    uint32_t seq;
};
struct InputMsg {
    Header h;
    XINPUT_GAMEPAD pad;
    uint8_t has_camera;
    float eye[3], target[3]; // the guest's camera, so the host turns the guest's stick the guest's way
    uint8_t away;            // the guest explores on its own (roam.cpp): its pawn goes back to its AI
};
struct CharState {
    uint8_t valid;
    float pos[4];  // +0x40
    float rot[4];  // +0x50 quaternion
    float yaw;     // +0x2578
    uint8_t cmd[kCmdCopyLen];      // command object +0x0C..+0x5F (sticks, move state, buttons)
    uint8_t cmd_extra[kCmdExtraLen]; // command object +0x88..+0x8F
    float hp, hp_max;               // [[ch+0x1D18]]+8, +0xC (hp_max 0 = unknown)
};
struct StageState {
    int32_t stage, point;           // current stage and entry point ([0x018FA4BC]+0x34 / +0x44)
    int32_t jump_stage, jump_point; // the jump being made (aStage +0x80C / +0x810) while jumping
    uint8_t jumping;                // aStage +0x808 == 2
    uint32_t jump_seq;              // +1 each time the host starts a jump
    uint32_t reload_seq;            // +1 each time the host loads a stage without a jump (retry
                                    // after death, last save, a game loaded from the title)
    int32_t day, day_ms;            // time (sGameSys: day, milliseconds into the day); day -1 = unknown
    int32_t time_speed;             // sGameSys +0xB877C (500 running, 0 stopped: talks, messages)
    int32_t event;                  // sGameSys +0x9E3A8 (256 = none, 7 = a talk)
    uint32_t party_sig;             // the guests' characters the host's world shows (rejoin.cpp)
    // Talks (milestone 6): the host's own pad, which the guest's game mirrors while both are in a
    // talk (the host's B reaches the guest's Arisen puppet, which starts the same talk there)
    uint16_t pad_buttons;
    int16_t pad_lx, pad_ly;
    float talk_npc[3]; // where the NPC of the host's latest talk stands (talk.cpp)
};
struct SnapMsg {
    Header h;
    CharState ch[kParty]; // 0 = Arisen, 1 + mCmcId = pawns (1 = the main pawn)
    StageState stage;
    int8_t my_pawn;       // the pawn (mCmcId) this guest drives, -1 = none yet
    uint32_t char_crc;    // CRC32 of this guest's character the host has complete (0 = none)
    int32_t stage_of[kParty]; // shared world: the area each entry's player is in (the host's layout)
    int64_t exp_total[4];     // shared world: exp each player's world gave (0 = host, 1 + guest place)
    int8_t my_slot;           // this guest's place in exp_total
    uint32_t loot_got;        // shared world: this guest's pickups the host has (loot.cpp)
};
struct PStateMsg { // shared world, guest -> host: its own Arisen
    Header h;
    CharState me;
    int32_t stage;
    int64_t exp_total; // exp_sync.cpp
    uint32_t loot_session;     // this game run (pickups restart at 1 with a new one)
    uint32_t loot_applied[4];  // the other players' pickups this guest applied, by place
};
constexpr int kLootPage = 24;
struct LootMsg { // shared world: pickups of the player at `slot`, numbered from `first`
    Header h;
    uint8_t slot;
    uint32_t session, first;
    uint8_t n;
    LootEv ev[kLootPage];
};
struct EnemyState {
    uint8_t slot;     // the host's enemy table slot (enemies.cpp)
    uint32_t cls;     // FNV-1a of the class name
    float spawn[3];   // where the host first saw it
    float pos[3];     // +0x40
    float rot[4];     // +0x50
    float yaw;        // +0x2578
    float hp, hp_max; // [[em+0x1D18]]+8, +0xC; hp_max 0 = unknown
    uint8_t died;     // its death handler ran on the host
};
constexpr int kEnemiesPerPage = 20;
struct EnemiesMsg {
    Header h;
    uint8_t count;
    EnemyState e[kEnemiesPerPage];
};
constexpr int kSaveChunk = 1024;
struct SaveChunkMsg {
    Header h;
    uint32_t version, total, crc;
    uint32_t sig;         // which guests' characters this save holds (storage_party_sig, 0 = none)
    uint16_t index, len;
    uint8_t data[kSaveChunk];
};
struct CharChunkMsg {
    Header h;
    uint32_t total, crc;
    uint16_t index, len;
    uint8_t data[1024];
};
constexpr int kSaveNeedMax = 200;
struct ProgressAckMsg {
    Header h;
    uint32_t crc;
};
struct SaveNeedMsg {
    Header h;
    uint32_t have;      // version the guest has complete (0 = none)
    uint32_t receiving; // version being received (0 = none)
    uint16_t count;     // missing pieces listed below
    uint16_t index[kSaveNeedMax];
};
#pragma pack(pop)

constexpr uint32_t kMagic = 0x4F434444;
NetRole g_role = NET_OFF;
SOCKET g_sock = INVALID_SOCKET;
bool g_steam = false;
constexpr DWORD kPeerTimeoutMs = 10000;
uint32_t g_seq = 0;
volatile LONG g_enemy_ready = 0; // g_enemy_lock initialised (net_start may come late: net_late=1)
long g_rx = 0, g_tx = 0;

// The other players. Host: up to 3 guests; guest: [0] is its host. Net thread only, except the
// camera fields (read on game threads through net_host_guest_camera_holder).
constexpr int kMaxPeers = 3;
struct Peer {
    bool used;
    sockaddr_in addr; // UDP
    uint64_t steam;   // Steam P2P
    uint32_t last_seq;
    DWORD last_rx;    // 0 = silent
    int pawn;         // host: the pawn (mCmcId) this guest drives, -1 = none
    // host: a stand-in camera holder (+0x34 -> camera, eye +0x40, target +0x60) with the guest's view
    uint8_t holder[0x40], camera[0x80];
    volatile LONG cam_valid;
    DWORD next_cam_log;
    // host: the save this guest has, and the pieces to send it
    uint32_t save_have;
    std::vector<uint8_t> save_queued;
    DWORD save_last_full;
    // host: this guest's character as it arrives (its CRC once complete)
    std::string char_data;
    std::vector<uint8_t> char_got;
    uint32_t char_crc_want, char_crc;
    // host: the progress record this guest acknowledged, and when to send the latest again
    uint32_t prog_acked;
    DWORD prog_next;
    // host, shared world: this guest's latest own state and area, and its exp total
    CharState pstate;
    int32_t pstage;
    DWORD pstate_tick;
    int64_t exp_total;
    // host, shared world loot: what this guest applied of each place's pickups, where it started
    uint32_t loot_applied[4], loot_joined[4];
    DWORD loot_next;
};
Peer g_peers[kMaxPeers] = {};
int g_pawn_order[kPawns] = {1, 2, 0};

// Shared world: the other players' characters, by LOCAL party index (0 = Arisen, 1 + mCmcId), the
// press/release bits not applied yet and when each was set (g_cap_lock; game threads read them).
bool g_shared = false;
CharState g_rp[kParty] = {};
uint32_t g_rp_press[kParty] = {}, g_rp_release[kParty] = {};
DWORD g_rp_tick[kParty] = {};
// a pawn showing a player who is in another area: kept still out of sight, no AI (owner: "quand une
// personne part elle n'est pas remplacée"), until that player comes back
bool g_rp_away[kParty] = {};
// shared world loot (loot.cpp): host, the guests' pickups by place (1 + guest index) and their run;
// guest, what it applied of each place's pickups and from which run
std::vector<LootEv> g_loot_log[4];
uint32_t g_loot_session[4] = {};
uint32_t g_loot_applied[4] = {}, g_loot_known[4] = {};
uint32_t g_loot_my_session = 0;
constexpr float kParkDepth = 3000.f; // that far under the local Arisen

// host: presses/releases seen since the last send, per party member
volatile LONG g_acc_press[kParty] = {}, g_acc_release[kParty] = {};

// guest: latest snapshot + presses/releases not yet applied
CRITICAL_SECTION g_snap_lock;
SnapMsg g_snap = {};
bool g_have_snap = false;
uint32_t g_pending_press[kParty] = {}, g_pending_release[kParty] = {};

const char *peer_name(const Peer &p, char *buf, size_t cap)
{
    if (g_steam) {
        _snprintf(buf, cap, "friend ...%04u", link_steam_short(p.steam));
    } else {
        char ip[64] = "?";
        inet_ntop(AF_INET, const_cast<in_addr *>(&p.addr.sin_addr), ip, sizeof ip);
        _snprintf(buf, cap, "%s:%u", ip, ntohs(p.addr.sin_port));
    }
    return buf;
}

// Host: each party member's state, copied on the game thread (net_capture) right before its own
// update: the net thread never reads a character the game may be freeing (a reload freed one under
// it on 2026-10-27: crash in enemies_hp).
CRITICAL_SECTION g_cap_lock;
CharState g_cap[kParty] = {};
DWORD g_cap_tick[kParty] = {};

// ---- shared world ----

} // namespace
void guest_world(uintptr_t gm, const StageState &h); // below
namespace {

// This game's current area (stage number), -1 outside a stage.
int32_t my_stage()
{
    uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0);
    return gm && current_stage_area() ? game::at<int32_t>(gm, 0x34) : -1;
}

// Another player's state for local party index i (shown when that player is in this area).
void shared_take(int i, const CharState &c, bool here)
{
    if (i < 1 || i >= kParty)
        return;
    uint32_t p, r;
    memcpy(&p, c.cmd + (0x2C - kCmdCopyFrom), 4);
    memcpy(&r, c.cmd + (0x34 - kCmdCopyFrom), 4);
    EnterCriticalSection(&g_cap_lock);
    g_rp[i] = c;
    g_rp[i].valid = c.valid && here;
    g_rp_away[i] = c.valid && !here;
    if (c.valid) {
        if (here) {
            g_rp_press[i] |= p;
            g_rp_release[i] |= r;
        }
        g_rp_tick[i] = GetTickCount() | 1;
    }
    LeaveCriticalSection(&g_cap_lock);
}

// Guest: the host's SNAP in this guest's layout. Its own character is the local Arisen, the host's
// Arisen is on the pawn the host gave this guest, the other guests on their own pawns.
void shared_from_snap(const SnapMsg &m)
{
    int me = m.my_pawn;
    int32_t here = my_stage();
    if (me < 0 || me >= kPawns)
        return;
    for (int i = 0; i < kParty; ++i) {
        if (i == 1 + me)
            continue; // this guest itself
        int local = i == 0 ? 1 + me : i;
        if (m.ch[i].valid)
            shared_take(local, m.ch[i], here >= 0 && m.stage_of[i] == here);
    }
}

} // namespace
bool net_shared_in_host_area();
namespace {
bool shared_in_host_area() { return net_shared_in_host_area(); }
} // namespace
bool net_shared_in_host_area()
{
    if (!g_have_snap)
        return false;
    EnterCriticalSection(&g_snap_lock);
    int32_t hs = g_snap.stage.stage;
    LeaveCriticalSection(&g_snap_lock);
    return hs >= 0 && hs == my_stage();
}
namespace {

// Game thread, after UpdateCommands of a pawn showing another player: that player's commands.
void shared_apply_commands(uintptr_t ch, int i)
{
    if (!net_puppet(i))
        return;
    EnterCriticalSection(&g_cap_lock);
    CharState s = g_rp[i];
    bool away = g_rp_away[i];
    uint32_t press = g_rp_press[i], release = g_rp_release[i];
    g_rp_press[i] = g_rp_release[i] = 0;
    LeaveCriticalSection(&g_cap_lock);
    uintptr_t cmd = ch + game::kCmd;
    if (away) { // still: no sticks, no buttons
        memset(reinterpret_cast<void *>(cmd + kCmdCopyFrom), 0, kCmdCopyLen);
        memset(reinterpret_cast<void *>(cmd + kCmdExtraFrom), 0, kCmdExtraLen);
        return;
    }
    memcpy(reinterpret_cast<void *>(cmd + kCmdCopyFrom), s.cmd, kCmdCopyLen);
    memcpy(reinterpret_cast<void *>(cmd + kCmdExtraFrom), s.cmd_extra, kCmdExtraLen);
    game::at<uint32_t>(cmd, 0x2C) = press;
    game::at<uint32_t>(cmd, 0x34) = release;
}

// Game thread, after the transform finalize of such a pawn: that player's place, facing and HP.
// The guest's own Arisen: the host's time of day (one world clock).
void shared_apply_transform(uintptr_t ch, int i)
{
    if (i == 0) { // its own character: where it is, every 5 s (tests)
        static DWORD next_where = 0;
        if (!next_where || int(GetTickCount() - next_where) >= 0) {
            next_where = (GetTickCount() + 5000) | 1;
            logf("net: shared: me at (%.0f, %.0f), stage %d", game::at<float>(ch, 0x40), game::at<float>(ch, 0x48), my_stage());
        }
    }
    if (i == 0 && g_role == NET_GUEST && g_have_snap) {
        uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0);
        EnterCriticalSection(&g_snap_lock);
        StageState h = g_snap.stage;
        LeaveCriticalSection(&g_snap_lock);
        if (gm && current_stage_area())
            guest_world(gm, h);
        return;
    }
    if (!net_puppet(i))
        return;
    EnterCriticalSection(&g_cap_lock);
    CharState s = g_rp[i];
    bool away = g_rp_away[i];
    LeaveCriticalSection(&g_cap_lock);
    float *pos = reinterpret_cast<float *>(ch + 0x40);
    if (away) { // parked under the local Arisen, out of sight
        uintptr_t me = possess_arisen();
        if (me) {
            float park[3] = {game::at<float>(me, 0x40), game::at<float>(me, 0x44) - kParkDepth, game::at<float>(me, 0x48)};
            memcpy(pos, park, sizeof park);
            for (uintptr_t off : kPosCopies)
                memcpy(reinterpret_cast<void *>(ch + off), park, 12);
        }
        static DWORD next_park_log[kParty] = {};
        if (!next_park_log[i] || int(GetTickCount() - next_park_log[i]) >= 0) {
            next_park_log[i] = GetTickCount() + 5000;
            logf("net: shared: party %d shows a player in another area: kept out of sight at y %.0f", i, pos[1]);
        }
        return;
    }
    float dx = s.pos[0] - pos[0], dy = s.pos[1] - pos[1], dz = s.pos[2] - pos[2];
    float err = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (err > 50.f) {
        memcpy(pos, s.pos, sizeof s.pos);
        for (uintptr_t off : kPosCopies)
            memcpy(reinterpret_cast<void *>(ch + off), s.pos, 12);
    } else {
        pos[0] += dx * 0.2f;
        pos[1] += dy * 0.2f;
        pos[2] += dz * 0.2f;
    }
    game::at<float>(ch, kYawA) = s.yaw;
    game::at<float>(ch, kYawB) = s.yaw;
    game::at<float>(ch, kYawC) = s.yaw;
    memcpy(reinterpret_cast<void *>(ch + 0x50), s.rot, sizeof s.rot);
    if (s.hp_max > 0.f)
        if (float *hp = enemies_hp(ch))
            hp[0] = s.hp;
    static DWORD next_log[kParty] = {};
    if (!next_log[i] || int(GetTickCount() - next_log[i]) >= 0) {
        next_log[i] = GetTickCount() + 1000;
        logf("net: shared: party %d shows another player, error %.1f u before correction, HP %.1f", i, err, s.hp);
    }
}

// The host's save for the guest (milestone 6). Net thread only.
uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
struct HostSave {
    std::vector<uint8_t> data;
    uint32_t version = 0, crc = 0, sig = 0;
} g_hsave; // the pieces each guest still needs are in its Peer
struct GuestSave {
    std::vector<uint8_t> data, got;
    uint32_t version = 0, total = 0, crc = 0, have = 0, sig = 0;
    DWORD next_need = 0;
} g_gsave;

// Enemies (milestone 4). Host: the latest state of each table slot, written on the game thread.
// Guest: the latest state received per host slot, and which host slot each local slot follows.
constexpr int kEnemySlots = 128;
CRITICAL_SECTION g_enemy_lock;
EnemyState g_enemy_state[kEnemySlots] = {};
DWORD g_enemy_tick[kEnemySlots] = {}; // host: last update; guest: last receive (0 = none)
int g_send_cursor = 0;
struct Follow {
    DWORD first_seen; // identifies the local enemy in that slot
    int host_slot;    // -1 = no match yet
    DWORD next_try;
    uintptr_t ptr;      // the local enemy (for the damage hook)
    DWORD dead_since;   // the host's enemy is dead and this one is not yet (0 = no)
    bool forced;        // death forced once
    DWORD unpaired_since; // no host enemy for it while the host sends enemies (0 = no)
    DWORD last;           // last finalize of the local enemy: a gone enemy's entry stays behind
};
Follow g_follow[kEnemySlots] = {};
struct EnemyStats {
    int applied, matched_new, unmatched;
    float worst_err;
    int blocked, allowed, forced, removed, puppet_blocked;
} g_enemy_stats = {};
DWORD g_last_enemies_rx = 0; // guest: last ENEMIES page received (0 = none)

uint32_t class_hash(const char *name)
{
    uint32_t h = 2166136261u;
    for (; *name; ++name)
        h = (h ^ uint8_t(*name)) * 16777619u;
    return h;
}


void capture_state(uintptr_t ch, CharState &s)
{
    s.valid = ch != 0;
    if (!ch)
        return;
    memcpy(s.pos, reinterpret_cast<void *>(ch + 0x40), sizeof s.pos);
    memcpy(s.rot, reinterpret_cast<void *>(ch + 0x50), sizeof s.rot);
    s.yaw = game::at<float>(ch, kYawA);
    memcpy(s.cmd, reinterpret_cast<void *>(ch + game::kCmd + kCmdCopyFrom), kCmdCopyLen);
    memcpy(s.cmd_extra, reinterpret_cast<void *>(ch + game::kCmd + kCmdExtraFrom), kCmdExtraLen);
    if (const float *hp = enemies_hp(ch)) {
        s.hp = hp[0];
        s.hp_max = hp[1];
    }
}

// Net thread: the latest copy (invalid when older than 500 ms: a load, a reload), plus the buttons
// pressed and released since the last send.
void read_state(int i, CharState &s)
{
    EnterCriticalSection(&g_cap_lock);
    s = g_cap[i];
    s.valid = s.valid && g_cap_tick[i] && GetTickCount() - g_cap_tick[i] < 500;
    LeaveCriticalSection(&g_cap_lock);
    if (!s.valid) {
        s = CharState{};
        return;
    }
    uint32_t press = uint32_t(InterlockedExchange(&g_acc_press[i], 0));
    uint32_t release = uint32_t(InterlockedExchange(&g_acc_release[i], 0));
    memcpy(s.cmd + (0x2C - kCmdCopyFrom), &press, 4);   // pressed bits since last send
    memcpy(s.cmd + (0x34 - kCmdCopyFrom), &release, 4); // released bits since last send
}

// sGameSys words that changed only during a cutscene (the Olra one on the Cassardis pier, dumps
// 2026-10-27 11:06): +0xAC720, +0xB883C, +0xB8840 and +0xB8844 (bit 25 set). Logged on change.
void log_scene_flags(const char *who, uintptr_t gm)
{
    static const uint32_t kOffs[] = {0xAC720, 0xB883C, 0xB8840, 0xB8844};
    static uint32_t logged[2][4] = {};
    uint32_t *last = logged[who[0] == 'g'];
    uint32_t v[4];
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
        v[i] = game::at<uint32_t>(gm, kOffs[i]);
        changed = changed || v[i] != last[i];
    }
    if (!changed)
        return;
    logf("world: %s scene flags %08X %08X %08X %08X", who, v[0], v[1], v[2], v[3]);
    memcpy(last, v, sizeof v);
}

// Host, net thread: stage, entry point and jump in progress. A jump lasts the whole fade (seconds),
// so 30 polls a second see every one.
void read_stage(StageState &s)
{
    static bool was_jumping = false;
    static uint32_t seq = 0;
    uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0), st = current_stage_area();
    // -1 = not in a stage (title, menus, loading): the guest waits. [0x018FA4BC]+0x34 already holds
    // the next stage while the title menus are open (seen 24 s before the load), so use the aStage.
    s.stage = st ? game::at<int32_t>(st, 0x724) : -1;
    s.point = gm && st ? game::at<int32_t>(gm, 0x44) : -1;
    s.jumping = st && game::at<int32_t>(st, 0x808) == 2;
    if (s.jumping) {
        s.jump_stage = game::at<int32_t>(st, 0x80C);
        s.jump_point = game::at<int32_t>(st, 0x810);
        if (!was_jumping)
            logf("net: stage: host jumps from %d to %d (point %d), jump %u", s.stage, s.jump_stage, s.jump_point, ++seq);
    }
    was_jumping = s.jumping;
    s.jump_seq = seq;
    s.reload_seq = stages_other_loads();
    s.party_sig = rejoin_loaded_sig();
    talk_last(s.talk_npc);
    XINPUT_STATE pad = {};
    if (pads_read_raw(0, &pad) == ERROR_SUCCESS) { // the host's own pad (the Arisen's, slot 0)
        s.pad_buttons = pad.Gamepad.wButtons;
        s.pad_lx = pad.Gamepad.sThumbLX;
        s.pad_ly = pad.Gamepad.sThumbLY;
    }
    s.day = gm && st ? game::at<int32_t>(gm, game::kGsDay) : -1;
    s.day_ms = gm ? game::at<int32_t>(gm, game::kGsDayMs) : 0;
    s.time_speed = gm ? game::at<int32_t>(gm, game::kGsTimeSpeed) : 500;
    s.event = gm ? game::at<int32_t>(gm, game::kGsEvent) : 0;
    static int32_t ev_logged = -1;
    if (gm && st && s.event != ev_logged) {
        logf("world: host event state %d -> %d (time day %d %02d:%02d)", ev_logged, s.event, s.day, s.day_ms / 3600000,
             s.day_ms / 60000 % 60);
        ev_logged = s.event;
    }
    if (gm && st)
        log_scene_flags("host", gm);
    static int32_t logged = -2;
    if (s.stage != logged) {
        logged = s.stage;
        logf("net: stage: host now in %d (point %d)", s.stage, s.point);
    }
}

void send_to(const Peer &peer, const void *p, int n)
{
    if (!peer.used)
        return;
    if (g_steam) {
        if (g_role == NET_GUEST ? link_steam_send(p, n) : link_steam_send_to(peer.steam, p, n))
            ++g_tx;
        return;
    }
    if (sendto(g_sock, static_cast<const char *>(p), n, 0, reinterpret_cast<const sockaddr *>(&peer.addr),
               sizeof peer.addr) == n) {
        ++g_tx;
    } else {
        static DWORD last_err_log = 0;
        if (GetTickCount() - last_err_log > 5000) {
            last_err_log = GetTickCount();
            logf("net: sendto failed, WSA error %d", WSAGetLastError());
        }
    }
}

// Guest: to its host. Host: to every guest.
void send_raw(const void *p, int n)
{
    for (const Peer &peer : g_peers)
        send_to(peer, p, n);
}

// Host: the pawn for a new guest: the first of pawn_order that exists and no other guest has
// (the hired pawns first, the main pawn last); -1 when none is left.
int free_pawn()
{
    for (int pass = 0; pass < 2; ++pass)
        for (int p : g_pawn_order) {
            bool taken = false;
            for (const Peer &q : g_peers)
                taken |= q.used && q.pawn == p;
            if (!taken && (pass == 1 || possess_pawn_ptr(p))) // pass 1: not seen yet, take it anyway
                return p;
        }
    return -1;
}

void drop_peer(int k, const char *why)
{
    Peer &p = g_peers[k];
    char name[64];
    logf("net: guest %d (%s) %s, dropped%s", k + 1, peer_name(p, name, sizeof name), why,
         p.pawn >= 0 ? "; its pawn goes back to its AI" : "");
    if (p.pawn >= 0) {
        possess_pawn(p.pawn, false, 0, -1);
        storage_set_party_character(p.pawn, std::string()); // its character leaves the merged save
    }
    InterlockedExchange(&p.cam_valid, 0);
    p = Peer{};
    p.pawn = -1;
}

void receive_all()
{
    char buf[1500];
    for (;;) {
        sockaddr_in from = {};
        uint64_t from_steam = 0;
        int n;
        if (g_steam) {
            n = link_steam_recv(buf, sizeof buf, &from_steam);
        } else {
            int fl = sizeof from;
            n = recvfrom(g_sock, buf, sizeof buf, 0, reinterpret_cast<sockaddr *>(&from), &fl);
        }
        if (n < 0)
            return;
        if (n < int(sizeof(Header)))
            continue;
        auto h = reinterpret_cast<const Header *>(buf);
        if (h->magic != kMagic)
            continue;
        // which player sent it: the guest has one peer, its host
        int k = -1;
        for (int i = 0; i < kMaxPeers && k < 0; ++i) {
            const Peer &p = g_peers[i];
            if (!p.used)
                continue;
            if (g_role == NET_GUEST || (g_steam ? p.steam == from_steam
                                                : p.addr.sin_addr.s_addr == from.sin_addr.s_addr &&
                                                      p.addr.sin_port == from.sin_port))
                k = i;
        }
        if (k < 0 && g_role == NET_HOST && h->type == MSG_INPUT && n == sizeof(InputMsg)) {
            // a new guest: a free place, the next pawn, pad slot k + 1
            for (int i = 0; i < kMaxPeers && k < 0; ++i)
                if (!g_peers[i].used)
                    k = i;
            int pawn = k >= 0 ? free_pawn() : -1;
            if (k < 0 || pawn < 0) {
                static DWORD next_full_log = 0;
                if (!next_full_log || int(GetTickCount() - next_full_log) >= 0) {
                    next_full_log = GetTickCount() + 10000;
                    logf("net: a guest knocked, but all %d places are taken", kMaxPeers);
                }
                continue;
            }
            Peer &p = g_peers[k];
            p = Peer{};
            p.used = true;
            p.addr = from;
            p.steam = from_steam;
            p.pawn = pawn;
            for (int s = 0; s < 4; ++s) // pickups made before this guest came are not its
                p.loot_joined[s] = s == 0 ? loot_mine_count() : uint32_t(g_loot_log[s].size());
            char name[64];
            logf("net: guest connected from %s (guest %d, pad slot %d, pawn %d%s)", peer_name(p, name, sizeof name), k + 1,
                 k + 1, pawn, pawn == 0 ? ", the main pawn" : "");
            if (!g_shared)
                possess_pawn(pawn, true, k + 1, k);
        }
        if (k < 0)
            continue;
        Peer &peer = g_peers[k];
        // old or duplicate; a big step back means the other copy restarted
        if (peer.last_seq != 0 && int32_t(h->seq - peer.last_seq) <= 0 && int32_t(peer.last_seq - h->seq) < 1000)
            continue;
        peer.last_seq = h->seq;
        ++g_rx;
        peer.last_rx = GetTickCount() | 1;
        if (g_role == NET_HOST && h->type == MSG_INPUT && n == sizeof(InputMsg)) {
            if (g_shared)
                continue; // each guest moves its own character (PSTATE)
            auto m = reinterpret_cast<const InputMsg *>(buf);
            // The right stick and R3 drive the guest's own camera on the guest PC; on the host they
            // would turn the host's camera (camera code reads every pad), so they are dropped here,
            // and Back / Start too (a guest must not pause the host's game).
            if (m->away != (possess_pawn_on(peer.pawn) ? 0 : 1) && peer.pawn >= 0) {
                possess_pawn(peer.pawn, !m->away, k + 1, k);
                logf("net: guest %d %s", k + 1, m->away ? "explores on its own: its pawn goes back to its AI" : "is back: it drives its pawn again");
            }
            if (m->away)
                continue;
            XINPUT_GAMEPAD pad = m->pad;
            pad.sThumbRX = pad.sThumbRY = 0;
            pad.wButtons &= WORD(~(XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START));
            pads_override(unsigned(k + 1), pad, GetTickCount() + 300);
            if (m->has_camera) {
                memcpy(peer.camera + 0x40, m->eye, sizeof m->eye);
                memcpy(peer.camera + 0x60, m->target, sizeof m->target);
                InterlockedExchange(&peer.cam_valid, 1);
                if (!peer.next_cam_log || int(GetTickCount() - peer.next_cam_log) >= 0) {
                    peer.next_cam_log = (GetTickCount() + 2000) | 1;
                    float fx = m->target[0] - m->eye[0], fz = m->target[2] - m->eye[2], n = std::sqrt(fx * fx + fz * fz);
                    if (n > 1e-3f)
                        logf("net: guest %d camera forward (%.3f, %.3f)", k + 1, fx / n, fz / n);
                }
            }
        } else if (g_role == NET_HOST && h->type == MSG_CHAR_CHUNK && n >= int(sizeof(Header) + 12)) {
            auto m = reinterpret_cast<const CharChunkMsg *>(buf);
            if (m->total == 0 || m->total > 256u * 1024 || m->len > sizeof m->data || peer.char_crc == m->crc)
                continue;
            if (peer.char_crc_want != m->crc || peer.char_data.size() != m->total) { // a new character
                peer.char_crc_want = m->crc;
                peer.char_data.assign(m->total, '\0');
                peer.char_got.assign((m->total + sizeof m->data - 1) / sizeof m->data, 0);
            }
            size_t off = size_t(m->index) * sizeof m->data;
            if (m->index < peer.char_got.size() && off + m->len <= m->total) {
                memcpy(&peer.char_data[off], m->data, m->len);
                peer.char_got[m->index] = 1;
            }
            bool complete = true;
            for (uint8_t b : peer.char_got)
                complete &= b != 0;
            if (complete && crc32(reinterpret_cast<const uint8_t *>(peer.char_data.data()), peer.char_data.size()) == m->crc) {
                peer.char_crc = m->crc;
                logf("net: guest %d's character arrived: %s (%u bytes), it replaces pawn %d", k + 1,
                     save_character_name(peer.char_data).c_str(), unsigned(peer.char_data.size()), peer.pawn);
                if (peer.pawn >= 0)
                    storage_set_party_character(peer.pawn, peer.char_data);
            }
        } else if (g_role == NET_HOST && h->type == MSG_PSTATE && n == int(sizeof(PStateMsg))) {
            auto m = reinterpret_cast<const PStateMsg *>(buf);
            peer.pstate = m->me;
            peer.pstage = m->stage;
            peer.pstate_tick = GetTickCount() | 1;
            peer.exp_total = m->exp_total;
            memcpy(peer.loot_applied, m->loot_applied, sizeof peer.loot_applied);
            if (g_shared)
                exp_sync_seen(1 + k, m->exp_total);
            if (g_shared && peer.pawn >= 0)
                shared_take(1 + peer.pawn, m->me, m->stage == my_stage());
        } else if (h->type == MSG_LOOT && n >= int(sizeof(Header) + 10)) {
            auto m = reinterpret_cast<const LootMsg *>(buf);
            if (m->n > kLootPage || m->slot >= 4 || n < int(sizeof(Header) + 10 + m->n * sizeof(LootEv)))
                continue;
            if (g_role == NET_HOST) { // a guest's own pickups
                int s = 1 + k;
                if (g_loot_session[s] != m->session) {
                    g_loot_session[s] = m->session;
                    g_loot_log[s].clear();
                }
                for (int e = 0; e < m->n; ++e)
                    if (m->first + e == g_loot_log[s].size() + 1) {
                        g_loot_log[s].push_back(m->ev[e]);
                        loot_net_apply(m->ev[e]);
                    }
            } else { // another player's pickups, through the host
                int s = m->slot;
                if (g_loot_known[s] != m->session) {
                    g_loot_known[s] = m->session;
                    g_loot_applied[s] = m->first - 1; // a new run of that player: from what the host sends
                }
                for (int e = 0; e < m->n; ++e)
                    if (m->first + e == g_loot_applied[s] + 1) {
                        loot_net_apply(m->ev[e]);
                        ++g_loot_applied[s];
                    }
            }
        } else if (g_role == NET_HOST && h->type == MSG_PROGRESS_ACK && n == int(sizeof(ProgressAckMsg))) {
            peer.prog_acked = reinterpret_cast<const ProgressAckMsg *>(buf)->crc;
        } else if (g_role == NET_GUEST && h->type == MSG_PROGRESS && n >= int(sizeof(Header) + 12)) {
            auto m = reinterpret_cast<const CharChunkMsg *>(buf);
            static std::string data;
            static std::vector<uint8_t> got;
            static uint32_t want = 0, done = 0;
            if (m->total == 0 || m->total > 256u * 1024 || m->len > sizeof m->data)
                continue;
            if (m->crc != done) {
                if (want != m->crc || data.size() != m->total) {
                    want = m->crc;
                    data.assign(m->total, '\0');
                    got.assign((m->total + sizeof m->data - 1) / sizeof m->data, 0);
                }
                size_t off = size_t(m->index) * sizeof m->data;
                if (m->index < got.size() && off + m->len <= m->total) {
                    memcpy(&data[off], m->data, m->len);
                    got[m->index] = 1;
                }
                bool complete = true;
                for (uint8_t b : got)
                    complete &= b != 0;
                if (!complete || crc32(reinterpret_cast<const uint8_t *>(data.data()), data.size()) != m->crc)
                    continue;
                done = m->crc;
                progress_guest_received(data); // logs; false = not for this character (acked anyway)
            }
            ProgressAckMsg a = {{kMagic, MSG_PROGRESS_ACK, ++g_seq}, done};
            send_raw(&a, sizeof a);
        } else if (g_role == NET_HOST && h->type == MSG_SAVE_NEED && n >= int(sizeof(Header) + 10)) {
            auto m = reinterpret_cast<const SaveNeedMsg *>(buf);
            peer.save_have = m->have;
            if (m->receiving == g_hsave.version && m->have != g_hsave.version)
                for (int i = 0; i < m->count && i < kSaveNeedMax; ++i)
                    if (m->index[i] < peer.save_queued.size())
                        peer.save_queued[m->index[i]] = 1;
        } else if (g_role == NET_GUEST && h->type == MSG_SAVE_CHUNK && n >= int(sizeof(Header) + 20)) {
            auto m = reinterpret_cast<const SaveChunkMsg *>(buf);
            GuestSave &g = g_gsave;
            if (m->version == g.have || m->total == 0 || m->total > 4u << 20 || m->len > kSaveChunk)
                continue;
            if (m->version != g.version) { // a new host save: start over
                g.version = m->version;
                g.total = m->total;
                g.crc = m->crc;
                g.sig = m->sig;
                g.data.assign(m->total, 0);
                g.got.assign((m->total + kSaveChunk - 1) / kSaveChunk, 0);
                logf("net: save: receiving the host's save, version %u, %u bytes", g.version, g.total);
            }
            size_t off = size_t(m->index) * kSaveChunk;
            if (m->index < g.got.size() && off + m->len <= g.total) {
                memcpy(g.data.data() + off, m->data, m->len);
                g.got[m->index] = 1;
            }
            size_t missing = 0;
            for (uint8_t b : g.got)
                missing += !b;
            if (!missing) {
                uint32_t c = crc32(g.data.data(), g.data.size());
                if (c == g.crc) {
                    storage_set_peer_save(g.data.data(), g.data.size(), g.version, g.sig);
                    g.have = g.version;
                    logf("net: save: the host's save version %u arrived (%u bytes, crc %08X)", g.version, g.total, c);
                } else {
                    logf("net: save: version %u crc %08X, expected %08X: asking again", g.version, c, g.crc);
                    g.got.assign(g.got.size(), 0);
                }
            }
        } else if (g_role == NET_GUEST && h->type == MSG_ENEMIES && n >= int(sizeof(Header) + 1)) {
            auto m = reinterpret_cast<const EnemiesMsg *>(buf);
            int count = m->count;
            if (count > kEnemiesPerPage || n != int(sizeof(Header) + 1 + count * sizeof(EnemyState)))
                continue;
            DWORD now = GetTickCount();
            EnterCriticalSection(&g_enemy_lock);
            for (int k = 0; k < count; ++k)
                if (m->e[k].slot < kEnemySlots) {
                    g_enemy_state[m->e[k].slot] = m->e[k];
                    g_enemy_tick[m->e[k].slot] = now;
                }
            g_last_enemies_rx = now;
            LeaveCriticalSection(&g_enemy_lock);
        } else if (g_role == NET_GUEST && h->type == MSG_SNAP && n == sizeof(SnapMsg)) {
            auto m = reinterpret_cast<const SnapMsg *>(buf);
            if (g_shared) {
                shared_from_snap(*m);
                for (int s = 0; s < 4; ++s)
                    if (s != m->my_slot)
                        exp_sync_seen(s, m->exp_total[s]);
            }
            EnterCriticalSection(&g_snap_lock);
            g_snap = *m;
            g_have_snap = true;
            for (int i = 0; i < kParty; ++i) {
                uint32_t p, r;
                memcpy(&p, m->ch[i].cmd + (0x2C - kCmdCopyFrom), 4);
                memcpy(&r, m->ch[i].cmd + (0x34 - kCmdCopyFrom), 4);
                g_pending_press[i] |= p;
                g_pending_release[i] |= r;
            }
            LeaveCriticalSection(&g_snap_lock);
        }
    }
}

void log_enemy_stats(DWORD now)
{
    EnterCriticalSection(&g_enemy_lock);
    int fresh = 0;
    for (int i = 0; i < kEnemySlots; ++i)
        fresh += g_enemy_tick[i] && now - g_enemy_tick[i] < 5000;
    EnemyStats st = g_enemy_stats;
    g_enemy_stats = {};
    LeaveCriticalSection(&g_enemy_lock);
    if (g_role == NET_HOST)
        logf("net: enemies: %d live on the host", fresh);
    else
        logf("net: enemies: %d from the host, %d local updates applied, %d new pairs, %d unpaired updates, "
             "worst error %.1f u; local hits %d blocked, %d let through (host: dead), %d deaths forced, %d "
             "guest-only removed, %d hits on the puppets blocked",
             fresh, st.applied, st.matched_new, st.unmatched, st.worst_err, st.blocked, st.allowed, st.forced, st.removed,
             st.puppet_blocked);
}

// Host: one page of live enemies per call (~30 calls/s), cycling through the table.
void send_enemies(DWORD now)
{
    EnemiesMsg m = {};
    m.h = {kMagic, MSG_ENEMIES, ++g_seq};
    EnterCriticalSection(&g_enemy_lock);
    int next = 0;
    for (int n = 0; n < kEnemySlots && m.count < kEnemiesPerPage; ++n) {
        int i = (g_send_cursor + n) % kEnemySlots;
        // 5 s: far enemies are not updated every frame (a far zombie went unsent with 1 s)
        if (g_enemy_tick[i] && now - g_enemy_tick[i] < 5000)
            m.e[m.count++] = g_enemy_state[i];
        next = (i + 1) % kEnemySlots;
    }
    g_send_cursor = m.count == kEnemiesPerPage ? next : 0;
    LeaveCriticalSection(&g_enemy_lock);
    if (m.count)
        send_raw(&m, int(sizeof(Header) + 1 + m.count * sizeof(EnemyState)));
}

void host_save_pieces(Peer &peer);

// Host, each 33 ms: pick up a new save version, queue it when the guest lacks it, send 24 pieces.
void host_save_send(DWORD now)
{
    HostSave &s = g_hsave;
    std::vector<uint8_t> data;
    uint32_t version = 0, sig = 0;
    if (storage_own_save(data, version, &sig) && version != s.version) {
        s.data.swap(data);
        s.version = version;
        s.sig = sig;
        s.crc = crc32(s.data.data(), s.data.size());
        for (Peer &p : g_peers)
            p.save_queued.clear(); // queued again below for each guest that lacks it
        logf("net: save: sending save version %u (%u bytes, crc %08X)", s.version, unsigned(s.data.size()), s.crc);
    }
    if (s.data.empty())
        return;
    size_t pieces = (s.data.size() + kSaveChunk - 1) / kSaveChunk;
    for (Peer &peer : g_peers) {
        if (!peer.used || peer.save_have == s.version)
            continue;
        if (peer.save_queued.size() != pieces) { // a new version, or a new guest
            peer.save_queued.assign(pieces, 1);
            peer.save_last_full = now | 1;
        }
        // nothing queued but the guest still lacks it (everything lost, or it just reconnected)
        bool any = false;
        for (uint8_t q : peer.save_queued)
            any |= q != 0;
        if (!any && now - peer.save_last_full > 3000) {
            peer.save_queued.assign(pieces, 1);
            peer.save_last_full = now | 1;
        }
        host_save_pieces(peer);
    }
}

void host_save_pieces(Peer &peer)
{
    HostSave &s = g_hsave;
    int sent = 0;
    for (size_t i = 0; i < peer.save_queued.size() && sent < 24; ++i) {
        if (!peer.save_queued[i])
            continue;
        peer.save_queued[i] = 0;
        SaveChunkMsg m = {};
        m.h = {kMagic, MSG_SAVE_CHUNK, ++g_seq};
        m.version = s.version;
        m.total = uint32_t(s.data.size());
        m.crc = s.crc;
        m.sig = s.sig;
        m.index = uint16_t(i);
        size_t off = i * kSaveChunk;
        m.len = uint16_t(std::min<size_t>(kSaveChunk, s.data.size() - off));
        memcpy(m.data, s.data.data() + off, m.len);
        send_to(peer, &m, int(sizeof(Header) + 20 + m.len));
        ++sent;
    }
}

// Host, shared world, every 250 ms per guest: the other players' pickups it has not applied yet.
void host_loot_send(DWORD now)
{
    g_loot_session[0] = g_loot_my_session;
    for (int k = 0; k < kMaxPeers; ++k) {
        Peer &peer = g_peers[k];
        if (!peer.used || (peer.loot_next && int(now - peer.loot_next) < 0))
            continue;
        peer.loot_next = (now + 250) | 1;
        for (int s = 0; s < 4; ++s) {
            if (s == 1 + k)
                continue;
            uint32_t size = s == 0 ? loot_mine_count() : uint32_t(g_loot_log[s].size());
            uint32_t from = std::max(peer.loot_applied[s], peer.loot_joined[s]) + 1;
            if (from > size)
                continue;
            std::vector<LootEv> evs;
            if (s == 0) {
                loot_net_mine(evs, from, kLootPage);
            } else {
                for (uint32_t q = from; q <= size && int(evs.size()) < kLootPage; ++q)
                    evs.push_back(g_loot_log[s][q - 1]);
            }
            LootMsg lm = {};
            lm.h = {kMagic, MSG_LOOT, ++g_seq};
            lm.slot = uint8_t(s);
            lm.session = g_loot_session[s];
            lm.first = from;
            lm.n = uint8_t(evs.size());
            memcpy(lm.ev, evs.data(), evs.size() * sizeof(LootEv));
            send_to(peer, &lm, int(sizeof(Header) + 10 + evs.size() * sizeof(LootEv)));
        }
    }
}

// Host, every 500 ms per guest until acked: what its character gained (progress.cpp), in pieces.
void host_progress_send(DWORD now)
{
    for (Peer &peer : g_peers) {
        if (!peer.used || peer.pawn < 0 || (peer.prog_next && int(now - peer.prog_next) < 0))
            continue;
        peer.prog_next = (now + 500) | 1;
        std::string rec;
        if (!progress_host_record(peer.pawn, rec))
            continue;
        uint32_t crc = crc32(reinterpret_cast<const uint8_t *>(rec.data()), rec.size());
        if (crc == peer.prog_acked)
            continue;
        for (size_t off = 0, i = 0; off < rec.size(); off += sizeof(CharChunkMsg::data), ++i) {
            CharChunkMsg m = {};
            m.h = {kMagic, MSG_PROGRESS, ++g_seq};
            m.total = uint32_t(rec.size());
            m.crc = crc;
            m.index = uint16_t(i);
            m.len = uint16_t(std::min<size_t>(sizeof m.data, rec.size() - off));
            memcpy(m.data, rec.data() + off, m.len);
            send_to(peer, &m, int(sizeof(Header) + 12 + m.len));
        }
    }
}

// Guest, every 500 ms until the host's SNAP says it has it: its own character, in pieces.
void guest_char_send(DWORD now)
{
    static DWORD next = 0;
    static std::string ch;
    static uint32_t crc = 0;
    if (next && int(now - next) < 0)
        return;
    next = (now + 500) | 1;
    if (ch.empty()) {
        // coop.ini send_character=0: this friend plays the host's pawn as it is (no own character)
        char opt[8] = "1";
        if (cfg_str("send_character", opt, sizeof opt) && opt[0] == '0') {
            static bool logged = false;
            if (!logged)
                logf("net: send_character=0: this guest plays the host's pawn, its own character stays home");
            logged = true;
            return;
        }
        if (!storage_guest_character(ch))
            return; // its own save not read yet
        crc = crc32(reinterpret_cast<const uint8_t *>(ch.data()), ch.size());
    }
    EnterCriticalSection(&g_snap_lock);
    uint32_t host_has = g_have_snap ? g_snap.char_crc : 0;
    LeaveCriticalSection(&g_snap_lock);
    if (host_has == crc)
        return;
    for (size_t off = 0, i = 0; off < ch.size(); off += sizeof(CharChunkMsg::data), ++i) {
        CharChunkMsg m = {};
        m.h = {kMagic, MSG_CHAR_CHUNK, ++g_seq};
        m.total = uint32_t(ch.size());
        m.crc = crc;
        m.index = uint16_t(i);
        m.len = uint16_t(std::min<size_t>(sizeof m.data, ch.size() - off));
        memcpy(m.data, ch.data() + off, m.len);
        send_raw(&m, int(sizeof(Header) + 12 + m.len));
    }
}

// Guest, 4 times a second: the version it has, and the pieces still missing of the one arriving.
void guest_save_need(DWORD now)
{
    GuestSave &g = g_gsave;
    if (g.next_need && int(now - g.next_need) < 0) // 0 = not yet: int(now - 0) < 0 after 24.8 days
        return;
    g.next_need = (now + 250) | 1;
    SaveNeedMsg m = {};
    m.h = {kMagic, MSG_SAVE_NEED, ++g_seq};
    m.have = g.have;
    m.receiving = g.version != g.have ? g.version : 0;
    for (size_t i = 0; m.receiving && i < g.got.size() && m.count < kSaveNeedMax; ++i)
        if (!g.got[i])
            m.index[m.count++] = uint16_t(i);
    send_raw(&m, int(sizeof(Header) + 10 + 2 * m.count));
}

DWORD WINAPI net_thread(LPVOID)
{
    DWORD next_send = GetTickCount(), next_log = GetTickCount() + 5000; // never 0: int(now - 0) < 0 after 24.8 days of uptime
    DWORD next_tick = GetTickCount();
    bool steam_up = false;
    for (;;) {
        DWORD now = GetTickCount();
        if (g_steam && !steam_up) {
            steam_up = link_steam_start(g_role);
            Sleep(50);
            continue;
        }
        if (g_steam && int(now - next_tick) >= 0) {
            link_steam_tick();
            next_tick = now + 2000;
        }
        receive_all();
        now = GetTickCount();
        for (int k = 0; k < kMaxPeers; ++k) {
            Peer &p = g_peers[k];
            // signed: last_rx is `tick | 1` (never 0) and can be 1 ms ahead of `now`
            if (!p.used || !p.last_rx || int(now - p.last_rx) <= int(kPeerTimeoutMs))
                continue;
            if (g_role == NET_HOST) {
                char why[48];
                _snprintf(why, sizeof why, "silent for %lu ms", now - p.last_rx);
                drop_peer(k, why);
            } else {
                logf("net: peer silent for %lu ms, dropped", now - p.last_rx);
                p.last_rx = 0;
                p.last_seq = 0;
            }
        }
        if (int(now - next_send) >= 0) {
            if (g_role == NET_GUEST) {
                InputMsg m = {};
                m.h = {kMagic, MSG_INPUT, ++g_seq};
                XINPUT_STATE s = {};
                // Only in game: in the guest's own menus its pad must not press the host's menus.
                m.away = net_guest_roaming() ? 1 : 0;
                if (!m.away && possess_arisen() && pads_read_raw(0, &s) == ERROR_SUCCESS)
                    m.pad = s.Gamepad;
                uintptr_t holder = game::at<uintptr_t>(0, 0x018D0B6C);
                uintptr_t cam = holder ? game::at<uintptr_t>(holder, 0x34) : 0;
                if (cam && !m.away) {
                    memcpy(m.eye, reinterpret_cast<void *>(cam + 0x40), sizeof m.eye);
                    memcpy(m.target, reinterpret_cast<void *>(cam + 0x60), sizeof m.target);
                    m.has_camera = 1;
                }
                send_raw(&m, sizeof m);
                next_send = now + 16;
                guest_save_need(now);
                guest_char_send(now);
                if (g_shared) { // its own character, for the host and the other players
                    PStateMsg ps = {};
                    ps.h = {kMagic, MSG_PSTATE, ++g_seq};
                    read_state(0, ps.me);
                    ps.stage = my_stage();
                    ps.exp_total = exp_sync_total();
                    ps.loot_session = g_loot_my_session;
                    memcpy(ps.loot_applied, g_loot_applied, sizeof ps.loot_applied);
                    send_raw(&ps, sizeof ps);
                    static DWORD loot_next = 0;
                    if (g_have_snap && (!loot_next || int(now - loot_next) >= 0)) { // its pickups the host lacks
                        loot_next = (now + 250) | 1;
                        EnterCriticalSection(&g_snap_lock);
                        uint32_t got = g_snap.loot_got;
                        int8_t slot = g_snap.my_slot;
                        LeaveCriticalSection(&g_snap_lock);
                        std::vector<LootEv> evs;
                        loot_net_mine(evs, got + 1, kLootPage);
                        if (!evs.empty()) {
                            LootMsg lm = {};
                            lm.h = {kMagic, MSG_LOOT, ++g_seq};
                            lm.slot = uint8_t(slot);
                            lm.session = g_loot_my_session;
                            lm.first = got + 1;
                            lm.n = uint8_t(evs.size());
                            memcpy(lm.ev, evs.data(), evs.size() * sizeof(LootEv));
                            send_raw(&lm, int(sizeof(Header) + 10 + evs.size() * sizeof(LootEv)));
                        }
                    }
                } else {
                    roam_tick(); // free roam: leave the host's party and come back (roam.cpp)
                }
                bool roam = net_guest_roaming() || roam_busy() || g_shared;
                if (!roam)
                    guest_talk_mirror(now);
                if (g_have_snap && (!roam || g_shared)) { // shared: a guest still reloads when the host's
                                                          // world gets new players' characters
                    EnterCriticalSection(&g_snap_lock);
                    uint32_t host_sig = g_snap.stage.party_sig;
                    LeaveCriticalSection(&g_snap_lock);
                    rejoin_tick(NET_GUEST, host_sig);
                }
            } else {
                SnapMsg m = {};
                m.h = {kMagic, MSG_SNAP, ++g_seq};
                for (int i = 0; i < kParty; ++i)
                    read_state(i, m.ch[i]);
                read_stage(m.stage);
                for (int i = 0; i < kParty; ++i)
                    m.stage_of[i] = m.stage.stage;
                if (g_shared) { // every player's own state; AI pawns: each world runs its own
                    for (int i = 1; i < kParty; ++i)
                        m.ch[i].valid = 0;
                    for (Peer &p : g_peers)
                        if (p.used && p.pawn >= 0 && p.pstate_tick && now - p.pstate_tick < 500) {
                            m.ch[1 + p.pawn] = p.pstate;
                            m.stage_of[1 + p.pawn] = p.pstage;
                            memset(p.pstate.cmd + (0x2C - kCmdCopyFrom), 0, 4); // a press is relayed once
                            memset(p.pstate.cmd + (0x34 - kCmdCopyFrom), 0, 4);
                        }
                }
                m.exp_total[0] = exp_sync_total();
                for (int k = 0; k < kMaxPeers; ++k)
                    m.exp_total[1 + k] = g_peers[k].used ? g_peers[k].exp_total : 0;
                for (const Peer &peer : g_peers) { // each guest is told which pawn is its own
                    m.my_slot = int8_t(1 + (&peer - g_peers));
                    m.loot_got = uint32_t(g_loot_log[m.my_slot].size());
                    m.my_pawn = int8_t(peer.pawn);
                    m.char_crc = peer.char_crc;
                    send_to(peer, &m, sizeof m);
                }
                next_send = now + 33;
                send_enemies(now);
                storage_merge_tick(); // the guests' characters into the save, when something changed
                rejoin_tick(NET_HOST, 0); // and the host's world reloaded with them, during play
                host_save_send(now);
                host_progress_send(now);
                if (g_shared)
                    host_loot_send(now);
            }
        }
        if (int(now - next_log) >= 0) {
            DWORD last = 0;
            for (const Peer &p : g_peers)
                if (p.used && p.last_rx && (!last || int(p.last_rx - last) > 0))
                    last = p.last_rx;
            logf("net: tx %ld rx %ld, last rx %ld ms ago", g_tx, g_rx, last ? long(now - last) : -1L);
            if (g_role == NET_HOST) { // how far each pawn is from the Arisen (free roam tests)
                CharState c[kParty] = {};
                EnterCriticalSection(&g_cap_lock);
                memcpy(c, g_cap, sizeof c);
                LeaveCriticalSection(&g_cap_lock);
                char line[160];
                int len = 0;
                for (int i = 1; i < kParty; ++i)
                    if (c[0].valid && c[i].valid)
                        len += _snprintf(line + len, sizeof line - len, " pawn %d %.0f u;", i - 1,
                                         std::sqrt((c[i].pos[0] - c[0].pos[0]) * (c[i].pos[0] - c[0].pos[0]) +
                                                   (c[i].pos[2] - c[0].pos[2]) * (c[i].pos[2] - c[0].pos[2])));
                if (len)
                    logf("net: spread from the Arisen at (%.0f, %.0f):%s", c[0].pos[0], c[0].pos[2], line);
            }
            log_enemy_stats(now);
            next_log = now + 5000;
        }
        Sleep(4);
    }
}

} // namespace

// Host, game thread (possess_pre, before the character's own update): copy its state for the SNAP.
void net_capture(uintptr_t ch, int i)
{
    if (!(g_role == NET_HOST || (g_shared && i == 0)) || i < 0 || i >= kParty)
        return;
    CharState s = {};
    capture_state(ch, s);
    EnterCriticalSection(&g_cap_lock);
    g_cap[i] = s;
    g_cap_tick[i] = GetTickCount() | 1;
    LeaveCriticalSection(&g_cap_lock);
}

// The current aStage<NNN>, or 0 when the current area is something else (aTitle, aGameMain while a
// game loads, ...): the jump fields below only exist on an aStage.
uintptr_t current_stage_area()
{
    uintptr_t area = game::at<uintptr_t>(game::kArea, 0);
    uintptr_t cur = area ? game::at<uintptr_t>(area, game::kAreaCurrent) : 0;
    return cur && strncmp(enemies_class_of(cur), "aStage", 6) == 0 ? cur : 0;
}


void net_start(NetRole role, const char *peer, int port)
{
    InitializeCriticalSection(&g_cap_lock); // before g_role: net_capture runs once the role is set
    g_shared = early_flag("shared_world");
    g_loot_my_session = (GetTickCount() ^ (GetCurrentProcessId() << 16)) | 1;
    if (g_shared)
        logf("net: shared world: every player plays its own character in its own copy of the host's world");
    MemoryBarrier();
    g_role = role;
    for (Peer &p : g_peers)
        p.pawn = -1;
    char order[32];
    if (cfg_str("pawn_order", order, sizeof order)) {
        int a[kPawns], n = sscanf(order, "%d,%d,%d", &a[0], &a[1], &a[2]);
        for (int i = 0; i < n && i < kPawns; ++i)
            if (a[i] >= 0 && a[i] < kPawns)
                g_pawn_order[i] = a[i];
    }
    if (role == NET_HOST)
        logf("net: pawn order for guests 1-3: %d, %d, %d", g_pawn_order[0], g_pawn_order[1], g_pawn_order[2]);
    InitializeCriticalSection(&g_snap_lock);
    InitializeCriticalSection(&g_enemy_lock);
    InterlockedExchange(&g_enemy_ready, 1);
    char tp[16] = "udp"; // dist/coop.ini says transport=steam; tests never reach real Steam by default
    cfg_str("transport", tp, sizeof tp);
    g_steam = strcmp(tp, "steam") == 0;
    if (g_steam) {
        if (role == NET_GUEST)
            g_peers[0].used = true; // its host, found by steam_link.cpp
        logf("net: %s over Steam P2P, waiting for Steam", role == NET_HOST ? "host" : "guest");
        if (HANDLE t = CreateThread(nullptr, 0, net_thread, nullptr, 0, nullptr))
            CloseHandle(t);
        return;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(role == NET_HOST ? u_short(port) : 0);
    if (bind(g_sock, reinterpret_cast<sockaddr *>(&local), sizeof local) != 0) {
        logf("net: bind failed (%d)", WSAGetLastError());
        return;
    }
    if (role == NET_GUEST) {
        Peer &h = g_peers[0];
        h.used = true;
        h.addr.sin_family = AF_INET;
        h.addr.sin_port = htons(u_short(port));
        inet_pton(AF_INET, peer, &h.addr.sin_addr);
    }
    logf("net: %s, %s port %d", role == NET_HOST ? "host" : "guest", role == NET_HOST ? "listening on" : peer, port);
    if (HANDLE t = CreateThread(nullptr, 0, net_thread, nullptr, 0, nullptr))
        CloseHandle(t);
}

NetRole net_role() { return g_role; }

// Host: holder to use while building the possessed pawn's commands (0 = the real camera).
uintptr_t net_host_guest_camera_holder(int k)
{
    if (g_role != NET_HOST || k < 0 || k >= kMaxPeers || !g_peers[k].cam_valid)
        return 0;
    Peer &p = g_peers[k];
    *reinterpret_cast<uintptr_t *>(p.holder + 0x34) = reinterpret_cast<uintptr_t>(p.camera);
    return reinterpret_cast<uintptr_t>(p.holder);
}

// Host: called after each character's UpdateCommands; remembers presses/releases until sent.
void net_host_record(uintptr_t ch, int i)
{
    if (!(g_role == NET_HOST || (g_shared && i == 0)) || i < 0 || i >= kParty)
        return;
    uintptr_t cmd = ch + game::kCmd;
    if (uint32_t p = game::at<uint32_t>(cmd, 0x2C))
        InterlockedOr(&g_acc_press[i], LONG(p));
    if (uint32_t r = game::at<uint32_t>(cmd, 0x34))
        InterlockedOr(&g_acc_release[i], LONG(r));
}

// Guest, game thread, in a stage: the host's time of day (the host's world decides it: an inn
// rest skips to 6:00, talks stop the clock), and a log of both event states.
void guest_world(uintptr_t gm, const StageState &h)
{
    static int32_t ev_logged = -1;
    int32_t ev = game::at<int32_t>(gm, game::kGsEvent);
    if (ev != ev_logged) {
        logf("world: guest event state %d -> %d (host %d)", ev_logged, ev, h.event);
        ev_logged = ev;
    }
    log_scene_flags("guest", gm);
    if (h.day < 0)
        return;
    // The host's world sets the time: an inn rest jumps to 6:00, talks and messages stop the clock
    // (writing the time speed +0xB877C did not stop the guest's, 2026-10-26). So the guest's clock
    // is set to the host's every frame it is more than 1 s (game time) away; big jumps are logged.
    int32_t d = game::at<int32_t>(gm, game::kGsDay), ms = game::at<int32_t>(gm, game::kGsDayMs);
    int64_t mine = int64_t(d) * 86400000 + ms, host = int64_t(h.day) * 86400000 + h.day_ms;
    int64_t gap = mine - host;
    if (gap > 1000 || gap < -1000) {
        game::at<int32_t>(gm, game::kGsDay) = h.day;
        game::at<int32_t>(gm, game::kGsDayMs) = h.day_ms;
        if (gap > 600000 || gap < -600000) // more than 10 game minutes: a rest, a late join
            logf("world: guest time day %d %02d:%02d set to the host's day %d %02d:%02d", d, ms / 3600000,
                 ms / 60000 % 60, h.day, h.day_ms / 3600000, h.day_ms / 60000 % 60);
    }
    static DWORD next_log = 0;
    if (!next_log || int(GetTickCount() - next_log) >= 0) {
        next_log = GetTickCount() + 5000;
        int32_t md = game::at<int32_t>(gm, game::kGsDayMs);
        logf("world: time guest day %d %d ms, host day %d %d ms", game::at<int32_t>(gm, game::kGsDay), md, h.day,
             h.day_ms);
    }
}

// Stage numbers that name an aStage class (aStage100 .. aStage804). 0 is the death jump and 999 a
// remembered place: never ask for those (a jump to 0 made the guest's game try "aStage000" every
// frame, 2026-10-26).
bool plain_stage(int32_t n) { return n >= 100 && n < 999; }

// Guest, game thread (the Arisen's command update): when the host starts a jump, ask the guest's own
// stage for the same one, the way FUN_00401160 does for a gate (the game then fades, loads and
// places the party at the entry point). If the guest ends up in another stage than the host's for
// 10 s anyway, it jumps to the host's stage and entry point.
void net_guest_stage()
{
    if (g_role != NET_GUEST || !g_have_snap || net_guest_roaming() || roam_busy() || g_shared)
        return;
    EnterCriticalSection(&g_snap_lock);
    StageState h = g_snap.stage;
    LeaveCriticalSection(&g_snap_lock);
    static uint32_t done_seq = 0, done_reload = 0;
    static bool seq_known = false;
    static DWORD apart_since = 0, reload_seen = 0;
    static uint32_t reload_last = 0;
    uintptr_t st = current_stage_area(), gm = game::at<uintptr_t>(game::kGameMain, 0);
    if (!st || !gm || h.stage < 0)
        return;
    int32_t mine = game::at<int32_t>(st, 0x724), my_point = game::at<int32_t>(gm, 0x44);
    guest_world(gm, h);
    if (rejoin_busy())
        return; // the guest reloads the host's save (rejoin.cpp), which puts it where the host saved
    bool idle = game::at<int32_t>(st, 0x808) == 1;
    static int32_t logged = -2;
    if (mine != logged) {
        logged = mine;
        logf("net: stage: guest now in %d (host in %d)", mine, h.stage);
    }
    if (!seq_known) { // jumps the host made before the guest joined are not replayed
        seq_known = true;
        done_seq = h.jump_seq;
        done_reload = h.reload_seq;
    }
    int32_t to = -1, point = 0;
    const char *why = "";
    DWORD now = GetTickCount();
    if (h.reload_seq != reload_last) {
        reload_last = h.reload_seq;
        reload_seen = GetTickCount() | 1;
    }
    int32_t own_stage = -1;
    DWORD own_load = stages_last_load(&own_stage);
    if (h.reload_seq != done_reload && own_load && own_stage == h.stage && int(own_load - reload_seen) > -3000) {
        done_reload = h.reload_seq; // the guest loaded that stage itself meanwhile (both loaded the save)
    }
    if (h.reload_seq != done_reload && !h.jumping && h.stage >= 0) {
        // the host reloaded (retry, last save, loaded game): reload the same stage and point, even
        // when the guest is already in that stage, so the world restarts the same way
        to = h.stage;
        point = h.point;
        why = "host reloaded";
    } else if (h.jump_seq != done_seq && h.jumping && !plain_stage(h.jump_stage)) {
        // a special jump: 0 on death (the game-over screen follows, then a reload), 999 a remembered
        // place resolved by the host's own table. Wait until the host lands, then follow it there.
    } else if (h.jump_seq != done_seq && h.jumping && h.jump_stage == mine && h.jump_point == my_point && idle) {
        done_seq = h.jump_seq; // already there (the guest's puppet went through the same gate)
    } else if (h.jump_seq != done_seq && h.jumping) {
        to = h.jump_stage;
        point = h.jump_point;
        why = "host jump";
    } else if (h.jump_seq != done_seq && !h.jumping) {
        done_seq = h.jump_seq; // the host's jump is over (a special one, or one the guest missed)
        if (h.stage != mine) {
            to = h.stage;
            point = h.point;
            why = "host landed after a jump";
        }
    } else if (!h.jumping && h.stage != mine) {
        if (!apart_since)
            apart_since = now | 1;
        else if (now - apart_since > 10000) {
            to = h.stage;
            point = h.point;
            why = "stages differ for 10 s";
        }
    } else {
        apart_since = 0;
    }
    if (to < 0 || !idle)
        return;
    if (!plain_stage(to)) { // never ask for a stage the game has no class for (aStage000 loops)
        logf("net: stage: not jumping to %d (%s): not a stage number", to, why);
        done_seq = h.jump_seq;
        done_reload = h.reload_seq;
        return;
    }
    game::at<int32_t>(st, 0x80C) = to;
    game::at<int32_t>(st, 0x810) = point;
    game::at<uint8_t>(st, 0x814) = 0;
    game::at<int32_t>(st, 0x808) = 2;
    game::at<int32_t>(st, 4) = 0x13;
    done_seq = h.jump_seq;
    done_reload = h.reload_seq;
    apart_since = 0;
    logf("net: stage: guest in %d jumps to %d (point %d): %s", mine, to, point, why);
}

// Guest, net thread: while the host and the guest are both in a talk (or another event), the
// guest's game reads the host's pad on slot 0, so the lines, menus and choices advance together.
void guest_talk_mirror(DWORD now)
{
    uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0);
    if (!gm || !g_have_snap)
        return;
    EnterCriticalSection(&g_snap_lock);
    StageState h = g_snap.stage;
    LeaveCriticalSection(&g_snap_lock);
    int32_t mine = game::at<int32_t>(gm, game::kGsEvent);
    static bool on = false;
    static DWORD host_out = 0, catch_from = 0; // the host's talk ended / the guest's catch-up began
    static bool followed = false; // the guest's current talk is one it went through with the host
    bool want = h.event != 256 && mine != 256 && h.stage >= 0;
    // the guest's talk must be with the host's NPC: another one (NPCs walk on their own on each PC)
    // is a talk the host does not have (B only, no mirrored pad on another NPC's menus)
    // (judged for 1 s after the guest's talk starts: the host's SNAP with its NPC may come later)
    static uint32_t talks_seen = 0;
    static DWORD talk_at = 0;
    static bool other_npc = false;
    float mine_npc[3];
    uint32_t talks = talk_last(mine_npc);
    if (talks != talks_seen) {
        talks_seen = talks;
        talk_at = now | 1;
    }
    if (talk_at && now - talk_at < 1000) {
        float dx = mine_npc[0] - h.talk_npc[0], dz = mine_npc[2] - h.talk_npc[2];
        bool other = dx * dx + dz * dz > 150.f * 150.f;
        if (other != other_npc || now - talk_at < 40)
            logf("net: talk: guest's NPC %.0f u from the host's%s", std::sqrt(dx * dx + dz * dz), other ? ": another NPC" : "");
        other_npc = other;
    }
    if (mine == 256)
        other_npc = false;
    if (other_npc)
        want = false;
    if (want)
        followed = true;
    if (want != on) {
        on = want;
        logf("net: talk: guest %s the host's pad (host event %d, guest %d)", on ? "mirrors" : "stops mirroring", h.event, mine);
    }
    // The guest's talk can lag a step behind (a press that came while its text was still being
    // written): once the host's talk is over for 1 s and the guest's is not, B (back out of a
    // menu) and A (next line) in turn until it ends, at most 20 s. Its choices stay on its own PC
    // (a guest never saves). A talk the host never had (its puppet reached an NPC the host's
    // Arisen did not) only gets B: an unknown menu could be a Rift pawn's ("Recruter").
    if ((h.event == 256 || other_npc) && mine != 256 && h.stage >= 0) {
        if (!host_out)
            host_out = now | 1;
        if (now - host_out > 1000) {
            if (!catch_from) {
                catch_from = now | 1;
                logf("net: talk: guest still in a talk 1 s after the host's ended: %s until it ends",
                     followed ? "B/A" : "B only (a talk the host did not have)");
            }
            if (now - catch_from < 20000) {
                XINPUT_GAMEPAD g = {};
                DWORD phase = (now - catch_from) / 250 % 4; // B, -, A, -
                g.wButtons = phase == 0 ? XINPUT_GAMEPAD_B : phase == 2 && followed ? XINPUT_GAMEPAD_A : 0;
                pads_override(0, g, now + 100);
            }
        }
        return;
    }
    if (catch_from)
        logf("net: talk: guest's talk ended after %lu ms of catch-up", now - catch_from);
    host_out = catch_from = 0;
    if (mine == 256)
        followed = false;
    if (!on)
        return;
    XINPUT_GAMEPAD g = {};
    g.wButtons = h.pad_buttons & WORD(~(XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START));
    g.sThumbLX = h.pad_lx;
    g.sThumbLY = h.pad_ly;
    pads_override(0, g, now + 150);
}

// Guest: called after the character's own UpdateCommands (local input already hidden).
// Log label of a party member on the guest: "arisen", "pawn" (its own pawn), "party<i>" (others).
const char *party_label(int i, int my_pawn)
{
    static const char *others[kParty] = {"arisen", "party1", "party2", "party3"};
    return i > 0 && i == 1 + my_pawn ? "pawn" : others[i];
}

void net_apply_snapshot(uintptr_t ch, int i)
{
    if (g_shared) {
        shared_apply_commands(ch, i);
        return;
    }
    if (g_role != NET_GUEST || !g_have_snap || i < 0 || i >= kParty || net_guest_roaming())
        return;
    bool is_arisen = i == 0;
    EnterCriticalSection(&g_snap_lock);
    CharState s = g_snap.ch[i];
    uint32_t press = g_pending_press[i], release = g_pending_release[i];
    g_pending_press[i] = g_pending_release[i] = 0;
    LeaveCriticalSection(&g_snap_lock);
    if (is_arisen)
        net_guest_stage();
    if (!s.valid)
        return;
    // 1. commands: the puppet's own action engine reacts to the host's buttons and sticks
    uintptr_t cmd = ch + game::kCmd;
    memcpy(reinterpret_cast<void *>(cmd + kCmdCopyFrom), s.cmd, kCmdCopyLen);
    memcpy(reinterpret_cast<void *>(cmd + kCmdExtraFrom), s.cmd_extra, kCmdExtraLen);
    game::at<uint32_t>(cmd, 0x2C) = press;
    game::at<uint32_t>(cmd, 0x34) = release;
    if (press && is_arisen) // dialogue study: does the host's B (0x1000) reach the puppet?
        logf("net: guest arisen puppet press %08X (event state %d)", press,
             game::at<int32_t>(game::at<uintptr_t>(game::kGameMain, 0), game::kGsEvent));
    static DWORD next_log[kParty] = {}; // 0 = not set yet
    if (!next_log[i] || int(GetTickCount() - next_log[i]) >= 0) {
        next_log[i] = GetTickCount() + 1000;
        logf("net: guest %s commands applied, held %08X", party_label(i, g_snap.my_pawn), game::at<uint32_t>(cmd, 0x30));
    }
}

// Guest: called right after FUN_0084BF60 (uCharacterBase transform finalize: rebuilds the
// quaternion from the angle at +0x257C and writes the position). Writing here, after the action
// engine and that function, is what makes the puppet face and stand where the host says.
void net_apply_transform(uintptr_t ch, int i)
{
    if (g_shared) {
        shared_apply_transform(ch, i);
        return;
    }
    if (g_role != NET_GUEST || !g_have_snap || i < 0 || i >= kParty || net_guest_roaming())
        return;
    EnterCriticalSection(&g_snap_lock);
    CharState s = g_snap.ch[i];
    LeaveCriticalSection(&g_snap_lock);
    if (!s.valid)
        return;
    // drift correction: ease the position toward the host, snap when far
    float *pos = reinterpret_cast<float *>(ch + 0x40);
    float dx = s.pos[0] - pos[0], dy = s.pos[1] - pos[1], dz = s.pos[2] - pos[2];
    float err = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (err > 50.f) {
        memcpy(pos, s.pos, sizeof s.pos);
        for (uintptr_t off : kPosCopies)
            memcpy(reinterpret_cast<void *>(ch + off), s.pos, 12);
    } else {
        pos[0] += dx * 0.2f;
        pos[1] += dy * 0.2f;
        pos[2] += dz * 0.2f;
    }
    // facing: the game rebuilds the quaternion from these angles every frame
    game::at<float>(ch, kYawA) = s.yaw;
    game::at<float>(ch, kYawB) = s.yaw;
    game::at<float>(ch, kYawC) = s.yaw;
    memcpy(reinterpret_cast<void *>(ch + 0x50), s.rot, sizeof s.rot);
    // HP: the host's; local hits on the puppets are dropped (enemy_net_block_damage)
    if (s.hp_max > 0.f)
        if (float *hp = enemies_hp(ch))
            hp[0] = s.hp;
    static DWORD next_log[kParty] = {}; // 0 = not set yet
    if (!next_log[i] || int(GetTickCount() - next_log[i]) >= 0) {
        next_log[i] = GetTickCount() + 1000;
        const float *hp = enemies_hp(ch);
        logf("net: guest %s error %.1f u before correction, HP %.1f (host %.1f)", party_label(i, g_snap.my_pawn), err,
             hp ? hp[0] : -1.f, s.hp);
    }
}

// Guest: the camera follows the guest's own character, the pawn the host gave it. The camera
// ([[0x018D0B6C]+0x34]) copies eye/target from its active mode objects (+0x180/+0x184/+0x188,
// FUN_007D7210, found with a write watchpoint on the camera target); camera +0x70 and mode
// +0x70/+0x24C/+0x250/+0x254 point at the followed character (found by scanning for the
// Arisen's address).
void net_guest_camera()
{
    static int enabled = -1; // read coop.ini once
    if (enabled < 0)
        enabled = early_flag("guest_cam_pawn") ? 1 : 0;
    if (g_role != NET_GUEST || !enabled || net_guest_roaming() || g_shared)
        return;
    static uintptr_t followed = 0; // the pawn the camera was moved to (0 = the Arisen's)
    uintptr_t arisen = possess_arisen(), gm = game::at<uintptr_t>(game::kGameMain, 0);
    int mine = g_have_snap ? g_snap.my_pawn : -1;
    uintptr_t pawn = mine >= 0 ? possess_pawn_ptr(mine) : 0;
    // During a talk, a rest or a cutscene (event state not 256) the camera stays on the Arisen, the
    // way the game frames them; and it never keeps a pawn the game stopped updating (a rest
    // replaces the party: the camera read a freed pawn, crash in FUN_007DCF30, 2026-10-27 04:34).
    bool event = gm && game::at<int32_t>(gm, game::kGsEvent) != 256;
    uintptr_t want = event ? 0 : pawn;
    if (followed && followed != want && arisen) {
        int back = camera_follow(followed, arisen);
        logf("net: guest camera: back on the Arisen (%d references; %s)", back, event ? "an event" : "pawn gone");
        followed = 0;
    }
    if (want && arisen) {
        if (int moved = camera_follow(arisen, want))
            logf("net: guest camera: %d references moved from the Arisen to pawn %d", moved, mine);
        followed = want;
    }
}

// Move the camera's references to `from` (a character) over to `to`; returns how many it moved.
int camera_follow(uintptr_t from, uintptr_t to)
{
    uintptr_t holder = game::at<uintptr_t>(0, 0x018D0B6C);
    uintptr_t cam = holder ? game::at<uintptr_t>(holder, 0x34) : 0;
    if (!from || !to || !cam)
        return 0;
    int changed = 0;
    auto retarget = [&](uintptr_t obj, uintptr_t off) {
        uintptr_t &p = game::at<uintptr_t>(obj, off);
        if (p == from) {
            p = to;
            ++changed;
        }
    };
    retarget(cam, 0x70);
    const uintptr_t mode_slots[] = {0x180, 0x184, 0x188};
    const uintptr_t mode_fields[] = {0x70, 0x24C, 0x250, 0x254};
    for (uintptr_t slot : mode_slots)
        if (uintptr_t mode = game::at<uintptr_t>(cam, slot))
            for (uintptr_t f : mode_fields)
                retarget(mode, f);
    return changed;
}

// Game thread, right after an enemy's transform finalize (enemies.cpp, inside its table lock).
// Host: record its state. Guest: follow the host's enemy paired with it. Returns an enemy whose
// death enemies.cpp must run (enemy_net_death) once its lock is released, or 0.
uintptr_t enemy_net_after_finalize(EnemySlot &slot, int i)
{
    uintptr_t pending_death = 0;
    if (!g_enemy_ready || g_role == NET_OFF || i < 0 || i >= kEnemySlots || net_guest_roaming())
        return 0;
    if (g_shared && g_role == NET_GUEST && !shared_in_host_area())
        return 0; // elsewhere: this guest's own enemies
    uintptr_t em = slot.ptr;
    DWORD now = GetTickCount();
    if (!slot.settled) // no spawn point to pair on yet
        return 0;
    if (g_role == NET_HOST) {
        EnemyState s = {};
        s.slot = uint8_t(i);
        s.cls = class_hash(slot.name);
        memcpy(s.spawn, slot.spawn, sizeof s.spawn);
        memcpy(s.pos, reinterpret_cast<void *>(em + 0x40), sizeof s.pos);
        memcpy(s.rot, reinterpret_cast<void *>(em + 0x50), sizeof s.rot);
        s.yaw = game::at<float>(em, kYawA);
        if (float *hp = enemies_hp(em)) {
            s.hp = hp[0];
            s.hp_max = hp[1];
        }
        s.died = slot.died != 0;
        EnterCriticalSection(&g_enemy_lock);
        g_enemy_state[i] = s;
        g_enemy_tick[i] = now;
        LeaveCriticalSection(&g_enemy_lock);
        return pending_death;
    }
    EnterCriticalSection(&g_enemy_lock);
    Follow &f = g_follow[i];
    if (f.first_seen != slot.first_seen) // a new enemy in this local slot
        f = {slot.first_seen, -1, now, em, 0, false, 0, 0}; // never 0: int(now - 0) < 0 after 24.8 days of uptime
    f.last = now;
    if (f.host_slot >= 0 && (!g_enemy_tick[f.host_slot] || now - g_enemy_tick[f.host_slot] > 3000))
        f.host_slot = -1; // the host's enemy is gone (or the host stopped sending it)
    if (f.host_slot < 0 && int(now - f.next_try) >= 0) {
        f.next_try = now + 500;
        uint32_t cls = class_hash(slot.name);
        bool claimed[kEnemySlots] = {};
        for (int k = 0; k < kEnemySlots; ++k)
            if (k != i && g_follow[k].host_slot >= 0 && now - g_follow[k].last < 1000)
                claimed[g_follow[k].host_slot] = true;
        // 1. same class, spawn within 40 u; 2. same class, standing within 40 u now (critters such as
        // uEm8100 move away from their spawn after their first frames, not at the same moment on both PCs)
        const float *here = reinterpret_cast<const float *>(em + 0x40);
        const char *how = "spawns";
        float best = 40.f * 40.f;
        for (int pass = 0; pass < 2 && f.host_slot < 0; ++pass) {
            const float *mine = pass == 0 ? slot.spawn : here;
            for (int k = 0; k < kEnemySlots; ++k) {
                const EnemyState &h = g_enemy_state[k];
                if (!g_enemy_tick[k] || now - g_enemy_tick[k] > 3000 || h.cls != cls || claimed[k])
                    continue;
                const float *theirs = pass == 0 ? h.spawn : h.pos;
                float dx = theirs[0] - mine[0], dy = theirs[1] - mine[1], dz = theirs[2] - mine[2];
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < best) {
                    best = d2;
                    f.host_slot = k;
                }
            }
            if (pass == 0 && f.host_slot < 0)
                how = "positions";
        }
        if (f.host_slot >= 0) {
            ++g_enemy_stats.matched_new;
            logf("net: enemy #%d %s paired with host #%d (%s %.1f u apart)", i, slot.name, f.host_slot, how,
                 std::sqrt(best));
        } else if (now - slot.first_seen > 15000 && (now / 500) % 20 == 0) {
            // every ~10 s while unpaired: the closest host enemy of its class, and why it was not taken
            int near_k = -1;
            float near_d2 = 1e30f;
            for (int k = 0; k < kEnemySlots; ++k) {
                const EnemyState &h = g_enemy_state[k];
                if (!g_enemy_tick[k] || h.cls != cls)
                    continue;
                float dx = h.spawn[0] - slot.spawn[0], dy = h.spawn[1] - slot.spawn[1], dz = h.spawn[2] - slot.spawn[2];
                if (dx * dx + dy * dy + dz * dz < near_d2) {
                    near_d2 = dx * dx + dy * dy + dz * dz;
                    near_k = k;
                }
            }
            if (near_k >= 0)
                logf("net: enemy #%d %s unpaired: closest host #%d spawned %.1f u away, last received %lu ms ago%s", i,
                     slot.name, near_k, std::sqrt(near_d2), now - g_enemy_tick[near_k], claimed[near_k] ? ", taken" : "");
            else
                logf("net: enemy #%d %s unpaired: no host enemy of its class", i, slot.name);
        }
    }
    if (f.host_slot < 0) {
        ++g_enemy_stats.unmatched;
        // The host decides which enemies exist: one the guest's game spawned on its own (no host
        // enemy of its class spawned there for 10 s, while the host is sending enemies) is removed
        // with the game's own death.
        bool host_sending = g_last_enemies_rx && now - g_last_enemies_rx < 1000;
        // and no host enemy of its class spawned there in the last 30 s (it may be one the host's
        // game updates rarely, or one taken by another local enemy for now)
        bool host_knows = false;
        uint32_t cls = class_hash(slot.name);
        for (int k = 0; k < kEnemySlots && !host_knows; ++k) {
            const EnemyState &h = g_enemy_state[k];
            float dx = h.spawn[0] - slot.spawn[0], dy = h.spawn[1] - slot.spawn[1], dz = h.spawn[2] - slot.spawn[2];
            host_knows = g_enemy_tick[k] && now - g_enemy_tick[k] < 30000 && h.cls == cls &&
                         dx * dx + dy * dy + dz * dz < 40.f * 40.f;
        }
        if (!host_sending || host_knows || slot.died || f.forced) {
            f.unpaired_since = 0;
        } else if (!f.unpaired_since) {
            f.unpaired_since = now;
        } else if (now - f.unpaired_since > 10000) {
            f.forced = true;
            ++g_enemy_stats.removed;
            pending_death = em;
            logf("net: enemy #%d %s at (%.0f, %.0f, %.0f): not on the host for 10 s, removed", i, slot.name,
                 slot.spawn[0], slot.spawn[1], slot.spawn[2]);
        }
        LeaveCriticalSection(&g_enemy_lock);
        return pending_death;
    }
    f.unpaired_since = 0;
    EnemyState s = g_enemy_state[f.host_slot];
    float *pos = reinterpret_cast<float *>(em + 0x40);
    float dx = s.pos[0] - pos[0], dy = s.pos[1] - pos[1], dz = s.pos[2] - pos[2];
    float err = std::sqrt(dx * dx + dy * dy + dz * dz);
    ++g_enemy_stats.applied;
    if (err > g_enemy_stats.worst_err)
        g_enemy_stats.worst_err = err;
    LeaveCriticalSection(&g_enemy_lock);
    // same correction as the players' puppets: snap when far (with every copy the collision pass
    // keeps), ease otherwise; facing and quaternion forced
    if (err > 50.f) {
        memcpy(pos, s.pos, sizeof s.pos);
        for (uintptr_t off : kPosCopies)
            memcpy(reinterpret_cast<void *>(em + off), s.pos, 12);
    } else {
        pos[0] += dx * 0.3f;
        pos[1] += dy * 0.3f;
        pos[2] += dz * 0.3f;
    }
    game::at<float>(em, kYawA) = s.yaw;
    game::at<float>(em, kYawB) = s.yaw;
    game::at<float>(em, kYawC) = s.yaw;
    memcpy(reinterpret_cast<void *>(em + 0x50), s.rot, sizeof s.rot);
    // HP: the host's value wins. Local hits are dropped while the host's enemy lives
    // (enemy_net_block_damage); once it is dead they go through, so the guest's puppets, which
    // replay the host's attacks, land the killing blow with the game's own death. If none lands
    // within 1.5 s the death is forced (HP 0 and the game's death handler, enemy_net_death).
    float *hp = s.hp_max > 0.f ? enemies_hp(em) : nullptr;
    if (!hp)
        return pending_death;
    bool local_dead = slot.died != 0;
    if (!s.died) {
        if (s.hp > 0.f) // a host enemy at 0 HP not yet dead: leave the local one be until it dies
            hp[0] = s.hp;
        return pending_death;
    }
    EnterCriticalSection(&g_enemy_lock);
    if (local_dead || f.forced) {
        f.dead_since = 0;
    } else if (!f.dead_since) {
        f.dead_since = now;
    } else if (now - f.dead_since > 1500) {
        f.forced = true;
        f.dead_since = 0;
        ++g_enemy_stats.forced;
        hp[0] = 0.f;
        pending_death = em; // run by enemy_net_death, outside both locks (returned to enemies.cpp)
        logf("net: enemy #%d %s: dead on the host, no local killing blow in 1.5 s, death forced", i, slot.name);
    }
    LeaveCriticalSection(&g_enemy_lock);
    return pending_death;
}

// The game's own death for one character, as FUN_0076E7C0 runs it when a hit takes the HP to 0 (and
// HP entry+4 is 0): the damage manager (ch+0x10E0) callback at +0xC90 is the thunk 0x0088CF50, which
// jumps to vtable slot 0x120 (uEnemy FUN_00AAB3B0, uEm9000 FUN_00AA50B0, uEm0503 FUN_0092E340),
// thiscall (ch, entry+4, hit info). Every handler read checks the hit info for null first, and only
// the knock-back and kill credit use it, so it is passed as 0.
void enemy_net_death(uintptr_t em)
{
    using Death_t = void(__thiscall *)(uintptr_t, uint32_t, uint32_t);
    auto death = reinterpret_cast<Death_t>(game::at<uintptr_t>(game::at<uintptr_t>(em, 0), 0x120));
    death(em, 0, 0);
}

// Guest: the host table slot a local enemy slot follows (-1 = none), for test dumps.
int enemy_net_follow(int local_slot)
{
    if (g_role != NET_GUEST || !g_enemy_ready || local_slot < 0 || local_slot >= kEnemySlots)
        return -1;
    EnterCriticalSection(&g_enemy_lock);
    int k = g_follow[local_slot].host_slot;
    LeaveCriticalSection(&g_enemy_lock);
    return k;
}

// Damage hook (enemies.cpp, any game thread): on the guest, a hit on an enemy paired with a living
// host enemy is dropped; hits on enemies the host says are dead, or on unpaired ones, go through.
// Hits on the guest's puppets (the Arisen and the main pawn) are dropped too: their HP is the host's.
bool enemy_net_block_damage(uintptr_t ch)
{
    if (g_role != NET_GUEST || !g_enemy_ready || net_guest_roaming())
        return false;
    bool block = false;
    EnterCriticalSection(&g_enemy_lock);
    bool party = !g_shared && ch == possess_arisen(); // shared world: its own character takes its hits
    for (int p = 0; p < kPawns; ++p)
        party |= ch == possess_pawn_ptr(p) && (!g_shared || net_puppet(1 + p));
    if (party) {
        ++g_enemy_stats.puppet_blocked;
        LeaveCriticalSection(&g_enemy_lock);
        return true;
    }
    for (int i = 0; i < kEnemySlots; ++i) {
        const Follow &f = g_follow[i];
        if (f.ptr != ch || f.host_slot < 0 || GetTickCount() - f.last > 1000)
            continue;
        block = !g_enemy_state[f.host_slot].died;
        ++(block ? g_enemy_stats.blocked : g_enemy_stats.allowed);
        break;
    }
    LeaveCriticalSection(&g_enemy_lock);
    return block;
}

// Guest: the pawn (mCmcId) the host gave it, -1 = none yet (roam.cpp).
int net_guest_my_pawn()
{
    if (g_role != NET_GUEST || !g_have_snap)
        return -1;
    EnterCriticalSection(&g_snap_lock);
    int p = g_snap.my_pawn;
    LeaveCriticalSection(&g_snap_lock);
    return p;
}

bool net_shared() { return g_shared; }

// Shared world, game threads: local party index i shows another player in this area now.
bool net_puppet(int i)
{
    if (!g_shared || i < 1 || i >= kParty)
        return false;
    EnterCriticalSection(&g_cap_lock);
    bool on = (g_rp[i].valid || g_rp_away[i]) && g_rp_tick[i] && GetTickCount() - g_rp_tick[i] < 500;
    LeaveCriticalSection(&g_cap_lock);
    return on;
}
