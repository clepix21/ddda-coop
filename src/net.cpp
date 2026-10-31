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
#include <cstddef>
#include <cstring>
#include <string>

#include "game.h"
#include "mod.h"

void guest_talk_mirror(DWORD now); // below: the guest mirrors the host's pad during talks

namespace {

enum MsgType : uint8_t { MSG_INPUT = 1, MSG_SNAP = 2, MSG_ENEMIES = 3, MSG_SAVE_CHUNK = 4, MSG_SAVE_NEED = 5, MSG_CHAR_CHUNK = 6,
                       MSG_PROGRESS = 7, MSG_PROGRESS_ACK = 8, MSG_PSTATE = 9, MSG_LOOT = 10, MSG_FLAGS = 11, MSG_REVIVE = 12, MSG_QUEST = 13,
                       MSG_AREA_ENEMIES = 14, MSG_PING = 15, MSG_PONG = 16 };

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
    uint32_t magic; // kMagic: 'DDC2'
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
struct PingMsg { // the launcher's salon: a round trip, timed by the sender (PONG echoes t)
    Header h;
    uint32_t t;
};
struct CharState {
    uint8_t valid;
    float pos[4];  // +0x40
    float rot[4];  // +0x50 quaternion
    float yaw;     // +0x2578
    uint8_t cmd[kCmdCopyLen];      // command object +0x0C..+0x5F (sticks, move state, buttons)
    uint8_t cmd_extra[kCmdExtraLen]; // command object +0x88..+0x8F
    float hp, hp_max;               // [[ch+0x1D18]]+8, +0xC (hp_max 0 = unknown)
    int32_t action;                 // shared world: its action number (ch+0x2DD4, actions.cpp; weapon
                                    // actions have high bits: 0x0101001E, so not 16 bits)
    int8_t hold_slot;               // holding another player (action 0xA9): that player's place, else -1
    int16_t hold_enemy;             // holding an enemy: its number in the group leader's table, else -1
    int8_t hold_kind;               // ch+0x3938 (lift kind)
    float hold_off[3];              // holding an enemy: where it is from the holder (forward, up, right)
    uint8_t lantern;                // shared world: its lantern is lit (lantern_on)
    int32_t look[16];               // shared world: its model numbers, [ch+0x3DEC] +0x10..+0x50 (look_of)
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
    uint32_t flags_got;       // and its world flag changes (flags.cpp)
    uint8_t down[4];          // shared world: player down, by place (revive.cpp)
    int8_t pawn_of[4];        // the pawn showing each guest (by place), -1 = none
    uint64_t steam_of[4];     // each player's Steam id (by place), 0 = none or no Steam (the salon)
    uint16_t ping_of[4];      // the host's round trip to each guest, ms (0 = unknown), for the guests' salon
};
struct QuestChunkMsg { // shared world: the quest state of the player at `origin`, deflated, in pieces
    Header h;
    uint8_t origin;
    uint32_t crc, total; // crc of the inflated state (quest_sync.cpp), deflated size
    uint16_t index, len;
    uint8_t data[1024];
};
struct ReviveMsg { // shared world: the player at `target` was helped up
    Header h;
    uint8_t target;
};
struct PStateMsg { // shared world, guest -> host: its own Arisen
    Header h;
    CharState me;
    int32_t stage;
    int64_t exp_total; // exp_sync.cpp
    uint32_t loot_session;     // this game run (pickups restart at 1 with a new one)
    uint32_t loot_applied[4];  // the other players' pickups this guest applied, by place
    uint32_t flags_applied[4]; // the same for world flag changes
    uint8_t downed;            // revive.cpp
};
constexpr int kFlagsPage = 80;
struct FlagsMsg { // shared world: world flag changes of the player at `slot`, numbered from `first`
    Header h;
    uint8_t slot;
    uint32_t session, first;
    uint8_t n;
    FlagEv ev[kFlagsPage];
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
// Shared world: the enemies of an area the host is not in, from the player who runs them there (the
// leader), relayed by the host to the other players in that stage.
constexpr int kAreaPerPage = 16;
struct AreaEnemiesMsg {
    Header h;
    int32_t stage;
    uint8_t from; // the leader's slot (the host sets it from the sender)
    uint8_t count;
    EnemyState e[kAreaPerPage];
};
constexpr int area_msg_size(int count) { return int(offsetof(AreaEnemiesMsg, e) + count * sizeof(EnemyState)); }
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

// The message format's version: 'DDC2' since 2026-10-29 (32-bit action numbers), 'DDC3' (the held
// monster's place), 'DDC4' (players' Steam ids in the SNAP, for the launcher's salon), 'DDC5' (pings),
// 'DDC6' (the lantern). Messages of another format ('DDCO', 'DDC2'...) come from a player with another mod
// version: dropped, and said (the badge, the log).
constexpr uint32_t kMagic = 0x36434444;
bool other_version(uint32_t magic) { return (magic & 0x00FFFFFF) == 0x00434444 && magic != kMagic; }
volatile LONG g_old_peer = 0;
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
    uint16_t rtt;     // the last round trip to it, ms (PING / PONG once a second), 0 = none yet
    DWORD rtt_at;     // when it was measured
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
    uint32_t flags_applied[4], flags_joined[4];
    uint8_t downed;
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
// a pawn slot no player uses (a hired pawn): kept out of sight in every world (owner, after the first
// real test: only the players and the host's main pawn; hired pawns were doubled and odd)
bool g_rp_hidden[kParty] = {};
// each player's speed, from its last two states, and when the last one came: over the internet a
// state is 50-150 ms old when it arrives, which at a sprint put the pawn 60-100 u behind and the old
// 50 u snap made it jump every few seconds (first real test, 2026-10-27)
// Since 2026-10-30 the speed is taken over the states of the last 150-600 ms, not the last two: the
// states' arrival times are all the receiver has, and over the internet they come in bunches (two 15 ms
// apart, then nothing for 120 ms), so two states gave a speed up to 3 times too high or too low, and the
// pawn ran ahead and was pulled back ("the friends slide and jump", first evening, 150 u behind; three
// copies with 80 ms latency and 0-120 ms jitter: p90 168-185 u)
float g_rp_vel[kParty][3] = {};
DWORD g_rp_at[kParty] = {};
struct RpSample {
    DWORD t;
    float pos[3];
};
constexpr int kRpSamples = 16; // ~0.8 s at 20 states a second
RpSample g_rp_hist[kParty][kRpSamples] = {};
int g_rp_hist_n[kParty] = {};

// shared world loot (loot.cpp): host, the guests' pickups by place (1 + guest index) and their run;
// guest, what it applied of each place's pickups and from which run
std::vector<LootEv> g_loot_log[4];
uint32_t g_loot_session[4] = {};
uint32_t g_loot_applied[4] = {}, g_loot_known[4] = {};
uint32_t g_loot_my_session = 0;
std::vector<FlagEv> g_flags_log[4];
uint32_t g_flags_session[4] = {};
uint32_t g_flags_applied[4] = {}, g_flags_known[4] = {};
// shared world quests: the latest quest state of each place (this player's own, and on the host the
// ones it passes on), sent again every 3 s for 30 s; and the pieces arriving from each place
struct QuestBlob {
    std::vector<uint8_t> data;
    uint32_t crc = 0;
    DWORD since = 0, next = 0;
    std::vector<uint8_t> got;
    std::vector<uint8_t> in;
    uint32_t in_crc = 0;
} g_qb[4];
CRITICAL_SECTION g_qb_lock;
// that far under the local Arisen (out of sight). Parked 3000 u above instead, the host's kills in the
// underground stopped (shared exp suite 4/6 twice, 6/6 under, same code otherwise; 2026-10-28): kept
// under. The pawns parked there died of the fall check (actions.cpp now skips them).
constexpr float kParkHeight = 3000.f;

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

// A pawn is hidden once nobody used it for 3 s: on a guest the host's main pawn went hidden and back
// several times a second whenever one of its states came late (special deaths test, 2026-10-29),
// so the pawn showing a player vanished and fell each time.
void shared_set_hidden(int i, bool hidden)
{
    if (i < 1 || i >= kParty)
        return;
    static DWORD unused_since[kParty] = {};
    DWORD now = GetTickCount();
    if (!hidden)
        unused_since[i] = 0;
    else if (!unused_since[i])
        unused_since[i] = now | 1;
    if (hidden && !g_rp_hidden[i] && int(now - unused_since[i]) < 3000)
        return;
    EnterCriticalSection(&g_cap_lock);
    if (g_rp_hidden[i] != hidden)
        logf("net: shared: party %d %s", i, hidden ? "is a pawn no player uses: kept out of sight" : "is used again");
    g_rp_hidden[i] = hidden;
    if (hidden)
        g_rp_tick[i] = GetTickCount() | 1;
    LeaveCriticalSection(&g_cap_lock);
}

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
    DWORD t = GetTickCount();
    if (c.valid && here) {
        RpSample *h = g_rp_hist[i];
        int &n = g_rp_hist_n[i];
        if (n) { // a gap or a jump (a load, a teleport): the old states say nothing about the speed now
            const RpSample &last = h[n - 1];
            float jx = c.pos[0] - last.pos[0], jz = c.pos[2] - last.pos[2];
            if (t - last.t > 500 || jx * jx + jz * jz > 400.f * 400.f)
                n = 0;
        }
        if (n == kRpSamples) {
            memmove(h, h + 1, sizeof(RpSample) * (kRpSamples - 1));
            --n;
        }
        h[n].t = t;
        memcpy(h[n].pos, c.pos, sizeof h[n].pos);
        ++n;
        // the oldest state between 150 and 600 ms back: the speed over that span
        int from = -1;
        for (int k = 0; k < n - 1 && from < 0; ++k)
            if (t - h[k].t <= 600 && t - h[k].t >= 150)
                from = k;
        if (from >= 0) {
            float dt = (t - h[from].t) / 1000.f;
            for (int a = 0; a < 3; ++a)
                g_rp_vel[i][a] = (c.pos[a] - h[from].pos[a]) / dt;
        } else if (n == 1) {
            memset(g_rp_vel[i], 0, sizeof g_rp_vel[i]);
        }
        g_rp_at[i] = t | 1;
    }
    if (!c.valid) {
        // a state without a character (its game missed a beat): the last one stays. Taking it made a
        // parked pawn "here" for a frame, at no place, and the host's Arisen fell out of the map with
        // it (area lead suite 2026-10-28 15:24)
        LeaveCriticalSection(&g_cap_lock);
        return;
    }
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
    bool used[kParty] = {};
    for (int i = 0; i < kParty; ++i) {
        if (i == 1 + me)
            continue; // this guest itself
        int local = i == 0 ? 1 + me : i;
        used[local] = true; // the host on this guest's pawn; the other guests; the host's main pawn
        if (m.ch[i].valid)
            shared_take(local, m.ch[i], here >= 0 && m.stage_of[i] == here);
        else if (i >= 1)
            used[local] = false;
    }
    for (int s = 1; s < 4; ++s) // a guest who has a pawn is a player even between its states
        if (m.pawn_of[s] >= 0 && m.pawn_of[s] < kPawns && m.pawn_of[s] != me)
            used[1 + m.pawn_of[s]] = true;
    used[1 + me] = true;
    for (int i = 1; i < kParty; ++i)
        shared_set_hidden(i, !used[i]);
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
    bool away = g_rp_away[i] || g_rp_hidden[i];
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

// Local party index i (1..3, a pawn) shows which player (place 0 = host, 1 + guest), -1 = none.
int party_to_slot(int i)
{
    if (i < 1 || i >= kParty)
        return -1;
    if (g_role == NET_HOST) {
        for (int k = 0; k < kMaxPeers; ++k)
            if (g_peers[k].used && g_peers[k].pawn == i - 1)
                return 1 + k;
        return -1;
    }
    if (!g_have_snap)
        return -1;
    EnterCriticalSection(&g_snap_lock);
    int slot = -1, me = g_snap.my_pawn;
    if (i == 1 + me)
        slot = 0;
    else
        for (int s = 1; s < 4; ++s)
            if (g_snap.pawn_of[s] == i - 1)
                slot = s;
    LeaveCriticalSection(&g_snap_lock);
    return slot;
}

// The character showing player `slot` in this world: its own Arisen, or the pawn showing it.
uintptr_t slot_character(int slot)
{
    if (slot < 0)
        return 0;
    if (slot == net_my_slot())
        return possess_arisen();
    for (int i = 1; i < kParty; ++i)
        if (party_to_slot(i) == slot)
            return possess_pawn_ptr(i - 1);
    return 0;
}

bool shared_party_down(int i);          // below

// An action that holds something (grab start, a hold, the lift actions): +0x38F0 keeps its last
// target after a hold ends, and a guest reported "holding the host" while waiting (area lead suite
// 2026-10-28 15:24: the host's Arisen was tied to a parked pawn and fell out of the map)
bool hold_action(int a)
{
    return a == 0x39 || a == 0xA9 || a == 0xE3 || (a >= 0x3B && a <= 0x6F) || a == 0xDB || a == 0xE5;
}
int enemy_share_id(uintptr_t em);       // below: an enemy as the group's leader numbers it
uintptr_t enemy_from_share_id(int id);

// Game thread: another player holding something (RT: a player from behind, 0xA9 with the held one
// in 0xAA; a small monster lifted, 0x5F then the lift actions; ...; the held one at ch+0x38F0, the
// lift kind at +0x3938) is rebuilt here on the pawn showing it: the held one is this world's
// character for that player, or this world's copy of that enemy (the group leader's numbering,
// enemy_share_id). In the other worlds the holder's pawn starts a grab (RT reaches it) but a pawn's
// grab does not catch a player's Arisen (pair test 2026-10-28), and a monster may stand elsewhere, so
// the hold is set the way a grab ends: target and kind, the holder's action, the held one's links
// (+0x2104 and +0x30 = holder, seen on a held pawn and on a lifted uEm9000). Afterwards the holder's
// replayed RT keeps it and its release ends it, as in its world.
// In front of a character at distance d: its facing from the quaternion at +0x50 (x, y, z, w), the
// rotated +z axis (GRAB_FRONT_SIGN=-1 in coop.ini flips it, tests).
// A character's facing on the ground plane (from its quaternion +0x50), unit length; (0, 1) if unknown.
void facing(uintptr_t ch, float &fx, float &fz)
{
    float qx = game::at<float>(ch, 0x50), qy = game::at<float>(ch, 0x54), qz = game::at<float>(ch, 0x58), qw = game::at<float>(ch, 0x5C);
    fx = 2.f * (qx * qz + qw * qy);
    fz = 1.f - 2.f * (qx * qx + qy * qy);
    float n = std::sqrt(fx * fx + fz * fz);
    if (!(n > 0.01f)) {
        fx = 0.f, fz = 1.f;
        return;
    }
    fx /= n, fz /= n;
}

// The pawn carrying a monster here plays its player's lift action (arms up, LiftGeneric 0x61...),
// target and lift kind set, once a second while it plays something else; never on a pawn down or
// being hit. Tests first (test_carry_pose=1): the monster's own links are left alone (forcing them
// gave NaN places, 2026-10-28).
bool carry_pose_on()
{
    static int on = -1;
    if (on < 0)
        on = early_flag("test_carry_pose") ? 1 : 0;
    return on != 0;
}

// Monsters a player lifts in its world that its pawn here could not catch: carried where the holder
// has them (CharState.hold_off), and left alone by the enemy following meanwhile (enemy_net_after_finalize).
uintptr_t g_carried[kParty] = {};
bool carried_here(uintptr_t em)
{
    for (uintptr_t c : g_carried)
        if (c && c == em)
            return true;
    return false;
}
void carry(uintptr_t ch, uintptr_t em, const float off[3])
{
    float fx, fz;
    facing(ch, fx, fz);
    float to[3] = {game::at<float>(ch, 0x40) + fx * off[0] + fz * off[2], game::at<float>(ch, 0x44) + off[1],
                   game::at<float>(ch, 0x48) + fz * off[0] - fx * off[2]};
    if (!(std::isfinite(to[0]) && std::isfinite(to[1]) && std::isfinite(to[2])) || !(std::fabs(off[0]) + std::fabs(off[1]) + std::fabs(off[2]) < 400.f))
        return;
    memcpy(reinterpret_cast<void *>(em + 0x40), to, 12);
    for (uintptr_t o : kPosCopies)
        memcpy(reinterpret_cast<void *>(em + o), to, 12);
}

void carry_pose(uintptr_t ch, uintptr_t em, const CharState &s)
{
    static DWORD next[4] = {};
    if (!carry_pose_on() || !strstr(action_class(s.action), "Lift"))
        return;
    int k = 0;
    for (int p = 0; p < kPawns; ++p)
        if (possess_pawn_ptr(p) == ch)
            k = 1 + p;
    DWORD t = GetTickCount();
    if (next[k] && int(t - next[k]) < 0)
        return;
    next[k] = (t + 1000) | 1;
    const char *now = action_name(ch);
    if (strstr(now, "Lift") || strstr(now, "Dead") || strncmp(now, "cPlActDmg", 9) == 0)
        return;
    game::at<uintptr_t>(ch, 0x38F0) = em;
    game::at<int32_t>(ch, 0x3938) = s.hold_kind;
    action_set(ch, s.action);
    logf("net: shared: carrying pawn %p: %s -> %s (0x%X)", (void *)ch, now, action_name(ch), unsigned(s.action));
}

void place_in_front(uintptr_t ch, uintptr_t other, float d)
{
    float qx = game::at<float>(ch, 0x50), qy = game::at<float>(ch, 0x54), qz = game::at<float>(ch, 0x58), qw = game::at<float>(ch, 0x5C);
    float fx = 2.f * (qx * qz + qw * qy), fz = 1.f - 2.f * (qx * qx + qy * qy);
    float n = std::sqrt(fx * fx + fz * fz);
    if (!(n > 0.01f))
        return;
    static const float sign = early_flag("grab_front_flip") ? -1.f : 1.f;
    float to[3] = {game::at<float>(ch, 0x40) + sign * fx / n * d, game::at<float>(other, 0x44), game::at<float>(ch, 0x48) + sign * fz / n * d};
    memcpy(reinterpret_cast<void *>(other + 0x40), to, 12);
    for (uintptr_t off : kPosCopies)
        memcpy(reinterpret_cast<void *>(other + off), to, 12);
}

// Game thread: a weapon action (skill, attack: the high-bit numbers of the second action table) the
// player started that its pawn here did not start from the replayed buttons within 0.2 s is started
// directly. Skills test 2026-10-29: RB+Y (0x01010021) came out as the strong attack (0x01010004).
void shared_skill_look(uintptr_t ch, int i, const CharState &s)
{
    static int32_t seen[kParty] = {};
    static DWORD due[kParty] = {};
    DWORD now = GetTickCount();
    unsigned table = unsigned(s.action) >> 24; // 1, 2...: weapon actions; 0xFF: none (-1, it froze the game)
    // and the lantern lit or put away (cPlActUseLantern 0x99, cPlActPutAwayLantern 0x9A): the pawn showing that
    // player did not play them, so the others never saw the lantern (owner, 2026-10-30)
    bool weapon = (table >= 1 && table <= 0x0F) || s.action == 0x99 || s.action == 0x9A;
    if (s.action != seen[i]) {
        seen[i] = s.action;
        due[i] = weapon ? (now + 200) | 1 : 0;
        return;
    }
    int32_t mine = game::at<int32_t>(ch, 0x2DD4);
    // the same weapon action again (a second shot while aiming): the number does not change, so a pawn
    // that left it while its player stays in it is put back after 400 ms (the 4th shot of the bow test)
    // (weapon actions only: a lantern action stays in a player's state while its menu is open, and was played
    // again and again on the guest, lantern test 2026-10-31 00:03)
    if (!due[i] && table >= 1 && table <= 0x0F && mine != s.action)
        due[i] = (now + 400) | 1;
    if (!due[i] || int(now - due[i]) < 0)
        return;
    due[i] = 0;
    if (mine == s.action)
        return;
    logf("net: shared: party %d plays its player's 0x%X (it was in %s 0x%X)", i, unsigned(s.action), action_name(ch),
         unsigned(mine));
    action_set(ch, s.action);
}

// The lantern (owner, 2026-10-30: the others never saw a player's lantern lit; playing the lantern action
// on the pawn showing that player gave the gesture but no light). The game lights a party member's lantern
// when the lantern item it carries has bit 1 of its flags (+0x20) set: the Arisen's at [ch+0x3D70]; with
// none there, FUN_00460900 looks in the member's item list, [0x018FA4D4] + 0x178 + member * 0x11E8 (count
// at +8, items at +0x14, an item's number at +0x10) for the lantern numbers listed at 0x01539D94; the
// light itself is made by FUN_00432A10 for each lit member, from the lantern object at [ch+0x3D6C] that
// the lantern action (FUN_00B05DD0) makes with FUN_00B83480 (Ghidra, 2026-10-31).
constexpr int16_t kLanternItems[] = {64, 65, 76, 0x72B, 0x72E, 0x72F};

int party_member(uintptr_t ch) // 0 the Arisen, 1 + mCmcId a pawn, -1 neither (FUN_00B836A0's choice)
{
    uint8_t f = game::at<uint8_t>(ch, 0x206D);
    return (f & 2) ? 0 : (f & 4) ? 1 + game::at<int32_t>(ch, 0x5854) : -1;
}

uintptr_t lantern_item(uintptr_t ch)
{
    if (uintptr_t it = game::at<uintptr_t>(ch, 0x3D70))
        return it;
    int m = party_member(ch);
    uintptr_t mgr = game::at<uintptr_t>(0x018FA4D4, 0);
    if (m < 0 || m > 3 || !mgr)
        return 0;
    uintptr_t list = mgr + 0x178 + uintptr_t(m) * 0x11E8;
    uint32_t n = game::at<uint32_t>(list, 8);
    uintptr_t *items = game::at<uintptr_t *>(list, 0x14);
    if (n > 512 || !items || !mem_readable(items, n * sizeof *items))
        return 0;
    for (int16_t want : kLanternItems)
        for (uint32_t k = 0; k < n; ++k)
            if (items[k] && game::at<int16_t>(items[k], 0x10) == want)
                return items[k];
    return 0;
}

bool lantern_on(uintptr_t ch)
{
    uintptr_t it = lantern_item(ch);
    return it && (game::at<uint32_t>(it, 0x20) & 2);
}

// The pawn showing a player usually carries no lantern (the test save's pawns have none in their lists), so
// the lantern action played on it (shared_skill_look) makes the lantern object but finds no item to light:
// it gets a lantern item of its own here, outside the game's lists, lit while its player's is. The pawn's own
// items are left alone. Its object is made by the lantern action, played again if it has none (a player
// whose lantern was lit before this world saw it); put away, the object goes the way FUN_00B835B0 sends it.
void shared_lantern_look(uintptr_t ch, int i, const CharState &s)
{
    static uint8_t item[kParty][0x100];
    static DWORD again[kParty] = {};
    static int tries[kParty] = {}; // lantern actions played for this lighting (3 at most)
    static bool lit[kParty] = {};
    uintptr_t mine = uintptr_t(item[i]);
    uintptr_t it = game::at<uintptr_t>(ch, 0x3D70);
    if (it && it != mine) // a lantern of its own, lit by the game (the pawn's own lantern action)
        return;
    if (s.lantern) {
        if (!lit[i]) {
            lit[i] = true;
            tries[i] = 0;
            logf("net: shared: party %d: lantern lit, as its player's", i);
        }
        // given again while the pawn has no lantern object: the game takes it back each frame then
        // (FUN_00B766F0, outside the lantern actions)
        if (!it) {
            memset(item[i], 0, sizeof item[i]);
            *reinterpret_cast<int16_t *>(item[i] + 0x10) = kLanternItems[0];
            *reinterpret_cast<uint32_t *>(item[i] + 0x20) = 2;
            game::at<uintptr_t>(ch, 0x3D70) = mine;
        }
        int32_t act = game::at<int32_t>(ch, 0x2DD4);
        DWORD t = GetTickCount();
        if (!game::at<uintptr_t>(ch, 0x3D6C) && act != 0x99 && act != 0x9A && (!again[i] || int(t - again[i]) >= 0) && tries[i] < 3) {
            again[i] = (t + 3000) | 1;
            ++tries[i];
            action_set(ch, 0x99);
            logf("net: shared: party %d: no lantern in hand: lantern action played", i);
        }
        return;
    }
    if (!lit[i])
        return;
    lit[i] = false;
    if (it == mine) {
        game::at<uintptr_t>(ch, 0x3D70) = 0;
        if (uintptr_t obj = game::at<uintptr_t>(ch, 0x3D6C)) {
            game::at<uint8_t>(obj, 0x29B1) = 1;
            game::at<uintptr_t>(ch, 0x3D6C) = 0;
        }
    }
    logf("net: shared: party %d: lantern put away, as its player's", i);
}

// The gear (owner, 2026-10-30: a player's new gear was not seen by the others). A character's parameter
// copy [ch+0x3DEC] holds one model number per worn piece at +0x14..+0x4C (24-bit number, kind in the
// high byte, -1 = none; the menu's equip changes them); FUN_00B5CE10 (__thiscall(ch), a character
// virtual) remakes the weapons and the armour from them (equip27, 2026-10-31: armour off and on again live).
// Called directly, it opens a model this game has not loaded as a loose file: "Fatal error. Failed open
// file ...\m_helm_a102.mod", a message box behind the window (look1-look6, 2026-10-31). So the game's own
// refresh is asked instead (below), which loads the pieces' archives first. no_look_sync=1 turns it off.
bool look_of(uintptr_t ch, int32_t *out)
{
    uintptr_t prm = game::at<uintptr_t>(ch, 0x3DEC);
    if (!prm || !mem_readable(reinterpret_cast<void *>(prm + 0x10), 0x40))
        return false;
    memcpy(out, reinterpret_cast<void *>(prm + 0x10), 0x40);
    return true;
}

void shared_gear_look(uintptr_t ch, int i, const CharState &s)
{
    static const bool on = !early_flag("no_look_sync");
    static DWORD next[kParty] = {}, same_since[kParty] = {};
    if (!on || !s.look[1])
        return; // nothing sent (a state from before the look)
    int32_t mine[16];
    if (!look_of(ch, mine))
        return;
    // +0x10 is the vocation (2 Strider, 4 Mystic Knight, 1 Fighter in the test saves). A pawn with another one
    // is not that player's character yet: a guest joining late still shows a hired pawn there until its
    // world reloads, and that pawn remade with the host's gear asked for a mage's sound bank it did not have
    // ("Fatal error. Failed open file ...\mgc_pl_fireball.srq", lantern_late, 2026-10-31). Only the gear
    // (+0x14..+0x4C) is copied, and only 10 s after the pawn shows that player's vocation.
    DWORD t = GetTickCount();
    if (mine[0] != s.look[0]) {
        same_since[i] = 0;
        return;
    }
    if (!same_since[i])
        same_since[i] = t | 1;
    if (int(t - same_since[i]) < 10000 || !memcmp(mine + 1, s.look + 1, sizeof mine - 4))
        return;
    if (next[i] && int(t - next[i]) < 0)
        return;
    next[i] = (t + 1000) | 1;
    int m = party_member(ch);
    uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0);
    if (m < 0 || m > 3 || !gs)
        return;
    // the game's own look refresh (FUN_0043D4F0, each frame): a member whose word at sGameSys + 0xAC3C0 +
    // member * 4 is 1 gets its gear's archives loaded (FUN_0044A010, then 2 until FUN_0044A080 says they
    // are in), then 3: its look remade (FUN_00B80A50, which calls FUN_00B5CE10's pieces); the menu's equip
    // goes this way (arc3, 2026-10-31: m_a_helm1208.arc opened, then the look remade)
    uintptr_t prm = game::at<uintptr_t>(ch, 0x3DEC);
    uintptr_t mprm = m == 0 ? gs + 0xA76D0 : gs + 0xA6860 + uintptr_t(m) * 0x1660; // the member's own parameter
    memcpy(reinterpret_cast<void *>(prm + 0x14), s.look + 1, sizeof mine - 4);
    memcpy(reinterpret_cast<void *>(mprm + 0x14), s.look + 1, sizeof mine - 4);
    int32_t &state = game::at<int32_t>(gs, 0xAC3C0 + 4 * m);
    logf("net: shared: party %d (member %d): gear changed, as its player's: look refresh asked (state was %d)", i, m, state);
    if (state == 0)
        state = 1;
}

void shared_grab_look(uintptr_t ch, int i, const CharState &s)
{
    static int applied[kParty] = {-1, -1, -1, -1}; // held here: 0..3 a player, 0x100 + id an enemy
    static DWORD again[kParty] = {}, released[kParty] = {};
    static uintptr_t held_by[kParty] = {}, old_2104[kParty] = {}, old_30[kParty] = {};
    // a lifted monster also points at its holder at +0x2B98, +0x5CA4 and +0x60E4 (pointer scan of a
    // uEm9000 lifted by the Arisen, 2026-10-28)
    static constexpr uintptr_t kEmLinks[3] = {0x2B98, 0x5CA4, 0x60E4};
    static uintptr_t old_em[kParty][3] = {};
    static bool held_player[kParty] = {};
    DWORD t = GetTickCount();
    int want = !hold_action(s.action) ? -1 : s.hold_slot >= 0 ? s.hold_slot : s.hold_enemy >= 0 ? 0x100 + s.hold_enemy : -1;
    if (want < 0) {
        // let go in the holder's world: the links set here go, and a held player stands up if its
        // release action is still running 1.2 s later (it stayed in DmgHagaijimeEnd for 10 s)
        if (applied[i] >= 0 && held_by[i] && !held_player[i]) {
            uintptr_t h = held_by[i];
            for (int k = 0; k < 3; ++k)
                if (game::at<uintptr_t>(h, kEmLinks[k]) == ch)
                    game::at<uintptr_t>(h, kEmLinks[k]) = old_em[i][k];
        }
        if (applied[i] >= 0 && held_by[i]) {
            uintptr_t h = held_by[i];
            if (game::at<uintptr_t>(h, 0x2104) == ch)
                game::at<uintptr_t>(h, 0x2104) = old_2104[i];
            if (game::at<uintptr_t>(h, 0x30) == ch)
                game::at<uintptr_t>(h, 0x30) = old_30[i]; // 0 made the Arisen fall through the floor
            if (game::at<uintptr_t>(ch, 0x38F0) == h)
                game::at<uintptr_t>(ch, 0x38F0) = 0;
            released[i] = t | 1;
        }
        applied[i] = -1;
        if (g_carried[i]) {
            logf("net: shared: party %d let go of the monster it carried here", i);
            g_carried[i] = 0;
        }
        if (released[i] && int(t - released[i]) > 1200) {
            uintptr_t h = held_by[i];
            int32_t a = h && held_player[i] ? game::at<int32_t>(h, 0x2DD4) : 0;
            if (h && (a == 0xAA || a == 0xAB))
                action_set(h, 0);
            released[i] = 0;
            held_by[i] = 0;
        }
        return;
    }
    bool player = want < 0x100;
    uintptr_t held = player ? slot_character(want) : enemy_from_share_id(want - 0x100);
    float reach = -1.f; // how far the held one is from the pawn here (for the log), -1 = not found
    float far_at[3] = {}; // where it was, for the log (`held` is cleared when it is too far)
    if (held && held != ch) { // only within reach: never tie a character to a pawn somewhere else
        float rx = game::at<float>(held, 0x40) - game::at<float>(ch, 0x40), ry = game::at<float>(held, 0x44) - game::at<float>(ch, 0x44),
              rz = game::at<float>(held, 0x48) - game::at<float>(ch, 0x48);
        reach = std::sqrt(rx * rx + ry * ry + rz * rz);
        far_at[0] = game::at<float>(held, 0x40);
        far_at[1] = game::at<float>(held, 0x44);
        far_at[2] = game::at<float>(held, 0x48);
        // a monster is carried to the pawn wherever it is: the holder's world sends a lifted monster's
        // place as it stands there, ~6000 u off (the line before a guest's crash, lift suite 2026-10-29)
        if (!(reach < 300.f) && player)
            held = 0;
    }
    static DWORD next_why[kParty] = {};
    bool tell = !next_why[i] || int(t - next_why[i]) >= 0;
    if (!held || held == ch) {
        if (tell) {
            next_why[i] = (t + 2000) | 1;
            logf("net: shared: party %d holds %s %d in its world: not found here (%s)", i, player ? "player" : "enemy",
                 player ? want : want - 0x100, reach < 0.f ? "no such one" : "too far from the pawn");
            if (reach >= 0.f)
                // from far_at: `held` is 0 here when a player was too far (reading it closed three games, 2026-10-29)
                logf("net: shared:   it is %.0f u from the pawn: it at (%.0f, %.0f, %.0f), the pawn at (%.0f, %.0f, %.0f)", reach,
                     far_at[0], far_at[1], far_at[2], game::at<float>(ch, 0x40), game::at<float>(ch, 0x44), game::at<float>(ch, 0x48));
        }
        return;
    }
    // never on a pawn that is down, dead or being hit here: forcing a lift on a dead one
    // (cPlActDmgCrumbleDead) crashed the host at 0x00B11C7B
    const char *now_act = action_name(ch);
    if (shared_party_down(i) || strstr(now_act, "Dead") || strstr(now_act, "Neardeath") || strncmp(now_act, "cPlActDmg", 9) == 0) {
        if (tell) {
            next_why[i] = (t + 2000) | 1;
            logf("net: shared: party %d holds something in its world: not here, the pawn is in %s", i, now_act);
        }
        return;
    }
    if (applied[i] == want) {
        // still held in the holder's world: put the hold back when it ended here (it ended after
        // 1.5 s on the held player's own Arisen while the holder kept RT held)
        bool holding = game::at<uintptr_t>(ch, 0x38F0) == held &&
                       (player ? game::at<uintptr_t>(held, 0x2104) == ch && (s.action != 0xA9 || game::at<int32_t>(held, 0x2DD4) == 0xAA)
                               : g_carried[i] == held); // a monster: carried here
        static DWORD next_log[kParty] = {};
        if (!next_log[i] || int(t - next_log[i]) >= 0) { // tests: does the held one follow the pawn here?
            next_log[i] = (t + 1000) | 1;
            float dx = game::at<float>(held, 0x40) - game::at<float>(ch, 0x40), dy = game::at<float>(held, 0x44) - game::at<float>(ch, 0x44),
                  dz = game::at<float>(held, 0x48) - game::at<float>(ch, 0x48);
            logf("net: shared: party %d holding %s %d: %s, %.0f u from the pawn (height %+.0f), pawn in %s", i,
                 player ? "player" : "enemy", player ? want : want - 0x100, holding ? "held" : "lost",
                 std::sqrt(dx * dx + dz * dz), dy, action_name(ch));
        }
        if (!player) { // carried where the holder has it, for as long as the hold lasts
            carry(ch, held, s.hold_off);
            carry_pose(ch, held, s);
            return;
        }
        if (holding || int(t - again[i]) < 1000)
            return;
    }
    again[i] = t | 1;
    if (held_by[i] != held) { // what the held one pointed at before the hold
        old_2104[i] = game::at<uintptr_t>(held, 0x2104) == ch ? 0 : game::at<uintptr_t>(held, 0x2104);
        old_30[i] = game::at<uintptr_t>(held, 0x30) == ch ? 0 : game::at<uintptr_t>(held, 0x30);
        if (!player)
            for (int k = 0; k < 3; ++k)
                old_em[i][k] = game::at<uintptr_t>(held, kEmLinks[k]) == ch ? 0 : game::at<uintptr_t>(held, kEmLinks[k]);
    }
    applied[i] = want;
    held_by[i] = held;
    held_player[i] = player;
    released[i] = 0;
    game::at<uintptr_t>(ch, 0x38F0) = held;
    game::at<int32_t>(ch, 0x3938) = s.hold_kind;
    if (!player) {
        // a monster: carried where the holder has it (CharState.hold_off: forward, up, right of the
        // holder), the enemy following leaving it alone meanwhile. Tried before: forcing the lift
        // (the holder's action, the monster's links +0x2104, +0x30, +0x2B98, +0x5CA4, +0x60E4) gave
        // NaN places and killed the pawn; a grab start aimed at it 30 u in front caught it in 2 of
        // 19 tries, and its NaN place afterwards crashed the guest twice (lift suite, 2026-10-29).
        game::at<uintptr_t>(ch, 0x38F0) = 0;
        g_carried[i] = held;
        carry(ch, held, s.hold_off);
        logf("net: shared: party %d (player %d) holds enemy %d: carried here where its player has it (%.0f, %+.0f, %.0f)", i,
             party_to_slot(i), want - 0x100, s.hold_off[0], s.hold_off[1], s.hold_off[2]);
        return;
    }
    action_set(ch, s.action);
    if (s.action == 0xA9)
        action_set(held, 0xAA);
    game::at<uintptr_t>(held, 0x2104) = ch;
    game::at<uintptr_t>(held, 0x30) = ch;
    logf("net: shared: party %d (player %d) holds %s %d (action 0x%X, kind %d): hold rebuilt here", i, party_to_slot(i),
         player ? "player" : "enemy", player ? want : want - 0x100, unsigned(s.action), int(s.hold_kind));
}

// Local party index i shows a player who is down (revive.cpp): the host's peers, or the SNAP's down[].
bool shared_party_down(int i)
{
    if (i < 1 || i >= kParty)
        return false;
    if (g_role == NET_HOST) {
        for (const Peer &p : g_peers)
            if (p.used && p.pawn == i - 1)
                return p.downed != 0;
        return false;
    }
    if (!g_have_snap)
        return false;
    EnterCriticalSection(&g_snap_lock);
    bool down = false;
    int me = g_snap.my_pawn;
    if (i == 1 + me)
        down = g_snap.down[0] != 0; // the host, on this guest's pawn
    else
        for (int s = 1; s < 4; ++s)
            if (g_snap.pawn_of[s] == i - 1)
                down = g_snap.down[s] != 0;
    LeaveCriticalSection(&g_snap_lock);
    return down;
}

// Game thread: a pawn showing a downed player plays the game's near-death action (0x80) as long as
// that player is down, and stands up (action 0) when it is up again.
void shared_down_look(uintptr_t ch, int i)
{
    static DWORD next[kParty] = {};
    static bool forced[kParty] = {};
    DWORD now = GetTickCount();
    // the pawn lies "asleep" (0xB7 cPlActDmgSleep), not in the pawn's near-death action: that one starts
    // the game's own pawn-down state, whose skull stayed next to the name once its player was up
    // (screenshot review 2026-10-28). It wakes on its own after 2-5 s: put back while the player is
    // down; up with 0xB8 cPlActDmgSleepEnd.
    bool down = shared_party_down(i);
    bool lying = strcmp(action_name(ch), "cPlActDmgSleep") == 0;
    if (down && !lying && (!next[i] || int(now - next[i]) >= 0)) {
        next[i] = (now + 500) | 1;
        forced[i] = true;
        action_set(ch, 0xB7);
    } else if (!down && forced[i]) {
        forced[i] = false;
        if (lying)
            action_set(ch, 0xB8);
    } else if (!down && strstr(action_name(ch), "Dead") && (!next[i] || int(now - next[i]) >= 0)) {
        // a pawn showing a player who is up must not stay dead here (one was killed by spiders on
        // the host before its hits were blocked): the dead flag off, standing again
        next[i] = (now + 1000) | 1;
        uintptr_t e = game::at<uintptr_t>(ch, 0x1D18) ? game::at<uintptr_t>(game::at<uintptr_t>(ch, 0x1D18), 0) : 0;
        if (e)
            game::at<uint32_t>(e, 0x110) &= ~1u;
        logf("net: shared: party %d shows a player who is up but was in %s here: standing again", i, action_name(ch));
        action_set(ch, 0);
    }
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
    shared_down_look(ch, i);
    EnterCriticalSection(&g_cap_lock);
    CharState s = g_rp[i];
    bool away = g_rp_away[i] || g_rp_hidden[i];
    bool elsewhere = g_rp_away[i] && !g_rp_hidden[i]; // a player in another area (not an unused pawn)
    LeaveCriticalSection(&g_cap_lock);
    if (!s.valid && !away)
        return; // a player with no state yet (just connected): its pawn is left where it is
    if (!away) {
        shared_grab_look(ch, i, s);
        shared_skill_look(ch, i, s);
        shared_lantern_look(ch, i, s);
        shared_gear_look(ch, i, s);
    }
    EnterCriticalSection(&g_cap_lock);
    float vel[3];
    memcpy(vel, g_rp_vel[i], sizeof vel);
    float age = g_rp_at[i] ? (GetTickCount() - g_rp_at[i]) / 1000.f : 0.f;
    LeaveCriticalSection(&g_cap_lock);
    float *pos = reinterpret_cast<float *>(ch + 0x40);
    // the last place each player was seen in this area: when it goes to another area its pawn waits
    // under that place (usually the door it took), so its arrow on the map ("Carte" shows the pawns)
    // tells where it went; an unused pawn, or a player never seen here, waits under this Arisen
    static float last_here[kParty][3] = {};
    static int32_t last_stage[kParty] = {-1, -1, -1, -1};
    if (!away) {
        memcpy(last_here[i], s.pos, sizeof last_here[i]);
        last_stage[i] = my_stage();
    }
    static bool was_parked[kParty] = {};
    if (away) { // parked out of sight
        static bool watched = false; // tests (test_watch_dead=1): who kills a parked pawn
        if (!watched && !g_rp_hidden[i] && early_flag("test_watch_dead")) {
            uintptr_t e = game::at<uintptr_t>(ch, 0x1D18) ? game::at<uintptr_t>(game::at<uintptr_t>(ch, 0x1D18), 0) : 0;
            if (e) {
                watched = true;
                logf("net: shared: watching party %d's HP entry %08X (+0x110 dead flag, +8 HP)", i, unsigned(e));
                watch_start(e + 0x110, 6, 0);
            }
        }
        was_parked[i] = true;
        // a parked pawn falls the whole time; the fall check (actions.cpp) no longer kills it there.
        // Restarting its fall from a stand every second let the game bring an unused pawn back next
        // to the Arisen, upside down in the air (exp suite shot 2026-10-28 15:1x): not done
        uintptr_t me = possess_arisen();
        bool at_door = elsewhere && last_stage[i] >= 0 && last_stage[i] == my_stage();
        if (me) {
            float park[3] = {game::at<float>(me, 0x40), game::at<float>(me, 0x44) - kParkHeight, game::at<float>(me, 0x48)};
            if (at_door) {
                park[0] = last_here[i][0];
                park[1] = last_here[i][1] - kParkHeight;
                park[2] = last_here[i][2];
            }
            memcpy(pos, park, sizeof park);
            for (uintptr_t off : kPosCopies)
                memcpy(reinterpret_cast<void *>(ch + off), park, 12);
        }
        static DWORD next_park_log[kParty] = {};
        if (!next_park_log[i] || int(GetTickCount() - next_park_log[i]) >= 0) {
            next_park_log[i] = GetTickCount() + 5000;
            logf("net: shared: party %d shows a player in another area: kept out of sight at y %.0f%s", i, pos[1],
                 at_door ? ", under the last place it was seen here" : "");
        }
        return;
    }
    if (was_parked[i]) {
        // back from under the ground: a parked pawn falls the whole time (cPlActDmgFallHeight), and
        // put back on the floor it died of that fall (cPlActDmgDownDead, test 2026-10-28 10:04):
        // the fall is cancelled first, and its dead flag cleared
        was_parked[i] = false;
        memcpy(pos, s.pos, 12);
        for (uintptr_t off : kPosCopies)
            memcpy(reinterpret_cast<void *>(ch + off), s.pos, 12);
        uintptr_t e = game::at<uintptr_t>(ch, 0x1D18) ? game::at<uintptr_t>(game::at<uintptr_t>(ch, 0x1D18), 0) : 0;
        if (e)
            game::at<uint32_t>(e, 0x110) &= ~1u;
        logf("net: shared: party %d back in this area (was %s): its fall cancelled", i, action_name(ch));
        action_set(ch, 0);
    }
    // where that player is now: its last place, moved on at its speed for the age of that state plus
    // the time it took to come (about 60 ms), at most 0.3 s ahead; ground height as received
    float ahead = age + 0.06f > 0.3f ? 0.3f : age + 0.06f;
    float target[3] = {s.pos[0] + vel[0] * ahead, s.pos[1], s.pos[2] + vel[2] * ahead};
    float dx = target[0] - pos[0], dy = target[1] - pos[1], dz = target[2] - pos[2];
    float err = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (err > 400.f) { // far off (a load, a fall): straight there
        memcpy(pos, target, sizeof target);
        for (uintptr_t off : kPosCopies)
            memcpy(reinterpret_cast<void *>(ch + off), target, 12);
    } else { // its own replayed moves carry it; a gentle pull keeps it on the player's path
        float k = err > 120.f ? 0.3f : 0.12f;
        pos[0] += dx * k;
        pos[1] += dy * k;
        pos[2] += dz * k;
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
        // age and speed: a steady ~150 u error with three players on Steam (evening test 2026-10-30) is
        // unexplained; a state 0.24 s old or more puts the target the whole 0.3 s ahead
        logf("net: shared: party %d shows another player, error %.1f u before correction, HP %.1f (state %d ms old, %.0f u/s)",
             i, err, s.hp, int(age * 1000.f), std::sqrt(vel[0] * vel[0] + vel[2] * vel[2]));
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
    int paired_to;        // host_slot of the first update applied (the pairing's own jump is not an error)
};
Follow g_follow[kEnemySlots] = {};
struct EnemyStats {
    int applied, matched_new, unmatched;
    float worst_err;
    int blocked, allowed, forced, removed, puppet_blocked, felt;
} g_enemy_stats = {};
DWORD g_last_enemies_rx = 0; // guest: last ENEMIES page received (0 = none)

// Shared world: whose game runs the enemies around this player (its group: the same stage, within
// kLeadRange): the host when it is there, else the lowest slot there. The others follow that game's
// enemies the way guests follow the host's. -1 = none (classic mode, or no one known yet). Stage 200
// is the whole open world: in the first real test guests 5 km from the host had their own enemies
// removed as "not on the host" (55 times), so removal only applies near the leader (kRemoveRange).
constexpr float kLeadRange = 5000.f, kRemoveRange = 2500.f;
volatile LONG g_lead = -1;
volatile LONG g_lead_others = 0; // other players following this one (guest leader: send its pages)
float g_lead_pos[3] = {};        // the leader's position (g_enemy_lock)
int g_area_cursor = 0;

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
    if (!peer.used || (g_role == NET_GUEST && storage_guest_refused())) // a guest before the end of its tutorial
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
        // the host's 9-byte hello (steam_link.cpp) keeps the first format, 'DDCO' type 0: not another version
        // (it raised the alarm on every guest, evening test 2026-10-30)
        bool hello = h->magic == 0x4F434444 && h->type == 0 && n == 9;
        if (!hello && other_version(h->magic) && !InterlockedExchange(&g_old_peer, 1))
            logf("net: a player has an older version of the mod (its messages are not read): every player needs this one"
                 " (format %08X, %d bytes, type %u, from ...%04u)",
                 unsigned(h->magic), n, unsigned(h->type), unsigned(from_steam % 10000));
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
        if (k < 0 && g_role == NET_HOST && h->type == MSG_INPUT && n == sizeof(InputMsg) && !storage_host_ready()) {
            static DWORD next_wait_log = 0; // a guest knocks while this game is in the tutorial: not let in yet
            if (!next_wait_log || int(GetTickCount() - next_wait_log) >= 0) {
                next_wait_log = (GetTickCount() + 10000) | 1;
                logf("net: a guest waits: this game is before the end of the tutorial (no main pawn yet), co-op starts after it");
            }
            continue;
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
            for (int s = 0; s < 4; ++s) { // pickups and world changes made before this guest came are not its
                p.loot_joined[s] = s == 0 ? loot_mine_count() : uint32_t(g_loot_log[s].size());
                p.flags_joined[s] = s == 0 ? flags_mine_count() : uint32_t(g_flags_log[s].size());
            }
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
            memcpy(peer.flags_applied, m->flags_applied, sizeof peer.flags_applied);
            if (peer.downed != m->downed)
                logf("net: guest %d is %s", k + 1, m->downed ? "down" : "up again");
            peer.downed = m->downed;
            if (g_shared)
                exp_sync_seen(1 + k, m->exp_total);
            if (g_shared && peer.pawn >= 0)
                shared_take(1 + peer.pawn, m->me, m->stage == my_stage());
        } else if (h->type == MSG_QUEST && n >= int(sizeof(Header) + 15)) {
            auto m = reinterpret_cast<const QuestChunkMsg *>(buf);
            int o = m->origin;
            if (o >= 4 || m->total == 0 || m->total > (4u << 20) || m->len > sizeof m->data)
                continue;
            if (g_role == NET_HOST)
                o = 1 + k; // a guest speaks for itself only
            QuestBlob &q = g_qb[o];
            EnterCriticalSection(&g_qb_lock);
            bool done_already = q.crc == m->crc;
            if (!done_already && (q.in_crc != m->crc || q.in.size() != m->total)) {
                q.in_crc = m->crc;
                q.in.assign(m->total, 0);
                q.got.assign((m->total + sizeof m->data - 1) / sizeof m->data, 0);
            }
            bool complete = false;
            if (!done_already) {
                size_t off = size_t(m->index) * sizeof m->data;
                if (m->index < q.got.size() && off + m->len <= m->total) {
                    memcpy(q.in.data() + off, m->data, m->len);
                    q.got[m->index] = 1;
                }
                complete = true;
                for (uint8_t b : q.got)
                    complete &= b != 0;
            }
            std::vector<uint8_t> blob;
            if (complete) {
                blob.swap(q.in);
                q.got.clear();
            }
            LeaveCriticalSection(&g_qb_lock);
            std::string state;
            if (complete && save_inflate(blob, state) && (save_crc(state.data(), state.size()) | 1) == m->crc) {
                EnterCriticalSection(&g_qb_lock);
                q.crc = m->crc;
                if (g_role == NET_HOST) { // passed on to the other guests
                    q.data = blob;
                    q.since = GetTickCount() | 1;
                    q.next = 0;
                }
                LeaveCriticalSection(&g_qb_lock);
                quest_sync_received(state, m->crc);
            }
        } else if ((h->type == MSG_PING || h->type == MSG_PONG) && n == int(sizeof(PingMsg))) {
            PingMsg m = *reinterpret_cast<const PingMsg *>(buf);
            if (h->type == MSG_PING) { // echoed at once
                m.h = {kMagic, MSG_PONG, ++g_seq};
                send_to(peer, &m, sizeof m);
            } else {
                DWORD rtt = GetTickCount() - m.t;
                peer.rtt = uint16_t(rtt > 9999 ? 9999 : rtt < 1 ? 1 : rtt);
                peer.rtt_at = GetTickCount() | 1;
            }
        } else if (h->type == MSG_REVIVE && n == int(sizeof(ReviveMsg))) {
            int t = reinterpret_cast<const ReviveMsg *>(buf)->target;
            if (g_role == NET_GUEST || t == 0)
                revive_helped();
            else if (t >= 1 && t <= kMaxPeers && g_peers[t - 1].used) // the host passes it on
                send_to(g_peers[t - 1], buf, n);
        } else if (h->type == MSG_FLAGS && n >= int(sizeof(Header) + 10)) {
            auto m = reinterpret_cast<const FlagsMsg *>(buf);
            if (m->n > kFlagsPage || m->slot >= 4 || n < int(sizeof(Header) + 10 + m->n * sizeof(FlagEv)))
                continue;
            if (g_role == NET_HOST) { // a guest's world changes: made here too, and logged for the others
                int s = 1 + k;
                if (g_flags_session[s] != m->session) {
                    g_flags_session[s] = m->session;
                    g_flags_log[s].clear();
                }
                for (int e = 0; e < m->n; ++e)
                    if (m->first + e == g_flags_log[s].size() + 1) {
                        g_flags_log[s].push_back(m->ev[e]);
                        flags_net_apply(m->ev[e]);
                    }
            } else {
                int s = m->slot;
                if (g_flags_known[s] != m->session) {
                    g_flags_known[s] = m->session;
                    g_flags_applied[s] = m->first - 1;
                }
                for (int e = 0; e < m->n; ++e)
                    if (m->first + e == g_flags_applied[s] + 1) {
                        flags_net_apply(m->ev[e]);
                        ++g_flags_applied[s];
                    }
            }
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
        } else if (h->type == MSG_AREA_ENEMIES && n >= area_msg_size(0)) {
            auto m = reinterpret_cast<AreaEnemiesMsg *>(buf);
            int count = m->count;
            if (count > kAreaPerPage || n != area_msg_size(count))
                continue;
            if (g_role == NET_HOST) { // to the other players in that stage
                m->from = uint8_t(1 + k);
                for (int q = 0; q < kMaxPeers; ++q)
                    if (q != k && g_peers[q].used && g_peers[q].pstate_tick && g_peers[q].pstage == m->stage)
                        send_to(g_peers[q], m, n);
                continue;
            }
            if (!g_shared || m->from != g_lead || int(g_lead) == net_my_slot() || m->stage != my_stage())
                continue;
            DWORD now = GetTickCount();
            EnterCriticalSection(&g_enemy_lock);
            for (int e = 0; e < count; ++e)
                if (m->e[e].slot < kEnemySlots) {
                    g_enemy_state[m->e[e].slot] = m->e[e];
                    g_enemy_tick[m->e[e].slot] = now;
                }
            g_last_enemies_rx = now;
            LeaveCriticalSection(&g_enemy_lock);
        } else if (g_role == NET_GUEST && h->type == MSG_ENEMIES && n >= int(sizeof(Header) + 1)) {
            auto m = reinterpret_cast<const EnemiesMsg *>(buf);
            int count = m->count;
            if (count > kEnemiesPerPage || n != int(sizeof(Header) + 1 + count * sizeof(EnemyState)))
                continue;
            if (g_shared && g_lead != 0)
                continue; // the host's enemies are not the ones around this guest
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
             "guest-only removed, %d hits on the puppets blocked; %d blocked hits played here without the kill",
             fresh, st.applied, st.matched_new, st.unmatched, st.worst_err, st.blocked, st.allowed, st.forced, st.removed,
             st.puppet_blocked, st.felt);
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

// An enemy of this world by the number the group's leader gives it (its own enemy table slot; a
// follower's enemies follow the leader's slots, g_follow): the same number in every world of a group.
bool lead_self() { return g_role == NET_HOST || (g_shared && int(g_lead) == net_my_slot()); }

int enemy_share_id(uintptr_t em)
{
    if (!g_enemy_ready || !em)
        return -1;
    if (lead_self()) {
        EnemySlot list[128];
        int n = enemies_snapshot(list, 128);
        for (int k = 0; k < n; ++k)
            if (list[k].ptr == em)
                return list[k].index;
        return -1;
    }
    EnterCriticalSection(&g_enemy_lock);
    int id = -1;
    for (int k = 0; k < kEnemySlots && id < 0; ++k)
        if (g_follow[k].ptr == em && g_follow[k].host_slot >= 0)
            id = g_follow[k].host_slot;
    LeaveCriticalSection(&g_enemy_lock);
    return id;
}

uintptr_t enemy_from_share_id(int id)
{
    if (!g_enemy_ready || id < 0 || id >= kEnemySlots)
        return 0;
    if (lead_self()) {
        EnemySlot list[128];
        int n = enemies_snapshot(list, 128);
        for (int k = 0; k < n; ++k)
            if (list[k].index == id) // dead ones too: a body can be lifted
                return list[k].ptr;
        return 0;
    }
    EnterCriticalSection(&g_enemy_lock);
    uintptr_t em = 0;
    DWORD now = GetTickCount();
    for (int k = 0; k < kEnemySlots && !em; ++k)
        if (g_follow[k].host_slot == id && now - g_follow[k].last < 1000)
            em = g_follow[k].ptr;
    LeaveCriticalSection(&g_enemy_lock);
    return em;
}

// Shared world, guest, net thread (~30/s): who leads this guest's group. When the leader changes the
// enemy table starts over with the new leader's pages (or this guest's own enemies when it leads).
void area_lead_update()
{
    int me = net_my_slot();
    PlayerInfo mine;
    int lead = -1, others = 0;
    float lp[3] = {};
    if (g_shared && me >= 0 && net_player_info(me, mine) && mine.stage >= 0) {
        for (int s = 0; s < 4; ++s) {
            PlayerInfo o;
            if (!net_player_info(s, o) || !o.present || o.stage != mine.stage)
                continue;
            float dx = o.pos[0] - mine.pos[0], dz = o.pos[2] - mine.pos[2];
            if (s != me && dx * dx + dz * dz > kLeadRange * kLeadRange)
                continue;
            if (s != me)
                ++others;
            if (lead < 0) {
                lead = s;
                memcpy(lp, o.pos, sizeof lp);
            }
        }
        if (!others)
            lead = -1; // alone: its own enemies, nothing to send
    }
    // a change holds 1.5 s first: during a load the other players' areas are briefly unknown, and each
    // change throws the pairings away (seen flipping four times in 200 ms when the host changed area)
    static int pending = -2;
    static DWORD pending_since = 0;
    DWORD now = GetTickCount();
    if (lead != int(g_lead)) {
        if (lead != pending) {
            pending = lead;
            pending_since = now;
        }
        if (now - pending_since < 1500)
            return;
    }
    pending = -2;
    InterlockedExchange(&g_lead_others, lead == me ? others : 0);
    EnterCriticalSection(&g_enemy_lock);
    memcpy(g_lead_pos, lp, sizeof lp);
    if (lead != int(g_lead)) {
        memset(g_enemy_tick, 0, sizeof g_enemy_tick);
        for (Follow &f : g_follow) {
            f.host_slot = -1;
            f.unpaired_since = 0;
            f.dead_since = 0;
        }
        g_last_enemies_rx = 0;
        InterlockedExchange(&g_lead, lead);
        logf("net: enemies around this player: %s", lead < 0 ? "its own (no other player near)"
                                                    : lead == me ? "this game runs them (it leads its group)"
                                                    : lead == 0  ? "the host's"
                                                                 : "another guest's");
        if (lead > 0 && lead != me)
            logf("net: enemies: player %d leads this group (stage %d)", lead, mine.stage);
    }
    LeaveCriticalSection(&g_enemy_lock);
}

// Shared world, guest leading its group: one page of its live enemies to the host, which relays it.
void send_area_enemies(DWORD now)
{
    AreaEnemiesMsg m = {};
    m.h = {kMagic, MSG_AREA_ENEMIES, ++g_seq};
    m.stage = my_stage();
    m.from = uint8_t(net_my_slot());
    EnterCriticalSection(&g_enemy_lock);
    int next = 0;
    for (int c = 0; c < kEnemySlots && m.count < kAreaPerPage; ++c) {
        int i = (g_area_cursor + c) % kEnemySlots;
        if (g_enemy_tick[i] && now - g_enemy_tick[i] < 5000)
            m.e[m.count++] = g_enemy_state[i];
        next = (i + 1) % kEnemySlots;
    }
    g_area_cursor = m.count == kAreaPerPage ? next : 0;
    LeaveCriticalSection(&g_enemy_lock);
    if (m.count)
        send_raw(&m, area_msg_size(m.count));
}

// Shared world, net thread, every second: coop_status.txt in the game folder for DDDA-Coop.exe's
// "Joueurs" panel. One "player" line per place: slot, connected, present, down, stage, ms since its
// last news (-1 = unknown), same area as this player, name (the guest's character on the host).
// The salon's picture of player k (place) as coop_avatar_<k>.bmp next to coop_status.txt: a 32-bit
// top-down BMP of the Steam avatar, written once per Steam id (and again while Steam still loads it);
// no Steam id: the file goes. The Steam name comes back in `persona`.
void salon_profile(int k, uint64_t id, std::string &persona)
{
    static uint64_t written[4] = {};
    static std::string names[4];
    static DWORD next_try[4] = {};
    char path[32];
    _snprintf(path, sizeof path, "coop_avatar_%d.bmp", k);
    if (!id) {
        if (written[k])
            DeleteFileA(path);
        written[k] = 0;
        names[k].clear();
        persona.clear();
        return;
    }
    DWORD now = GetTickCount();
    if (written[k] != id && (!next_try[k] || int(now - next_try[k]) >= 0)) {
        next_try[k] = (now + 2000) | 1;
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        std::string name;
        if (link_steam_profile(id, name, rgba, w, h)) {
            names[k] = name;
            if (!rgba.empty()) {
                BITMAPFILEHEADER fh = {};
                BITMAPINFOHEADER ih = {};
                ih.biSize = sizeof ih;
                ih.biWidth = LONG(w);
                ih.biHeight = -LONG(h); // top-down
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                ih.biCompression = BI_RGB;
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof fh + sizeof ih;
                fh.bfSize = DWORD(fh.bfOffBits + rgba.size());
                for (size_t i = 0; i + 3 < rgba.size(); i += 4)
                    std::swap(rgba[i], rgba[i + 2]); // RGBA -> BGRA
                char tmp[40];
                _snprintf(tmp, sizeof tmp, "%s.tmp", path);
                if (FILE *f = fopen(tmp, "wb")) {
                    fwrite(&fh, sizeof fh, 1, f);
                    fwrite(&ih, sizeof ih, 1, f);
                    fwrite(rgba.data(), 1, rgba.size(), f);
                    fclose(f);
                    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
                    written[k] = id;
                    logf("net: salon: player %d's Steam picture saved (%ux%u)", k, unsigned(w), unsigned(h));
                }
            }
        }
    }
    persona = names[k];
}

int host_level = 0; // write_status: the host's Arisen's level (from its own save)

void write_status(DWORD now)
{
    std::string out = "version 1\n";
    out += g_role == NET_HOST ? "role host\n" : "role guest\n";
    int me = net_my_slot();
    int32_t here = my_stage();
    char line[256];
    _snprintf(line, sizeof line, "me %d %d\n", me, int(here));
    out += line;
    for (int s = 0; s < 4; ++s) {
        bool connected = false;
        long ago = -1;
        std::string name;
        if (s == me) {
            connected = true;
            ago = 0;
            std::string own;
            if (g_role == NET_GUEST && storage_guest_character(own))
                name = save_character_name(own);
            static uint32_t host_version = 0; // host: its Arisen's name and level, read again when its save changes
            static std::string host_name;
            std::vector<uint8_t> file;
            uint32_t v = 0;
            if (g_role == NET_HOST && storage_own_save(file, v) && v != host_version) {
                std::string xml, ch;
                uint32_t head[8];
                if (save_unpack(file, xml, head) && save_character(xml, -1, ch)) {
                    host_name = save_character_name(ch);
                    host_level = save_character_level(ch);
                }
                host_version = v;
            }
            if (g_role == NET_HOST)
                name = host_name;
        } else if (g_role == NET_HOST) {
            const Peer &p = g_peers[s - 1];
            connected = s >= 1 && p.used;
            if (connected && p.last_rx)
                ago = long(now - p.last_rx);
            if (connected && !p.char_data.empty())
                name = save_character_name(p.char_data);
        } else if (g_have_snap) {
            EnterCriticalSection(&g_snap_lock);
            connected = s == 0 || g_snap.pawn_of[s] >= 0;
            int pawn = s == 0 ? net_guest_my_pawn() : g_snap.pawn_of[s]; // where that player shows in this world
            LeaveCriticalSection(&g_snap_lock);
            if (connected)
                name = roam_pawn_name(pawn);
            if (s == 0 && g_peers[0].last_rx)
                ago = long(now - g_peers[0].last_rx);
        }
        PlayerInfo info;
        bool known = connected && net_player_info(s, info);
        for (char &c : name)
            if (c == '\n' || c == '\r')
                c = ' ';
        _snprintf(line, sizeof line, "player %d %d %d %d %d %ld %d %s\n", s, connected ? 1 : 0, known && info.present ? 1 : 0,
                  known && info.downed ? 1 : 0, known ? int(info.stage) : -1, ago, known && here >= 0 && info.stage == here ? 1 : 0,
                  name.c_str());
        out += line;
    }
    // the launcher's salon: each player's level and ping, this player's Steam id (the host's code)
    std::string pings;
    for (int k = 0; k < 4; ++k) {
        int level = 0, ping = 0;
        if (k == me) {
            std::string own;
            if (g_role == NET_GUEST && storage_guest_character(own))
                level = save_character_level(own);
            if (g_role == NET_HOST)
                level = host_level;
        } else if (g_role == NET_HOST) {
            const Peer &q = g_peers[k - 1];
            if (k >= 1 && q.used) {
                level = q.char_data.empty() ? 0 : save_character_level(q.char_data);
                ping = q.rtt_at && now - q.rtt_at < 5000 ? q.rtt : 0;
            }
        } else if (g_have_snap) {
            EnterCriticalSection(&g_snap_lock);
            int pawn = k == 0 ? net_guest_my_pawn() : g_snap.pawn_of[k];
            uint16_t relayed = g_snap.ping_of[k];
            LeaveCriticalSection(&g_snap_lock);
            level = pawn >= 0 ? roam_pawn_level(pawn) : 0;
            ping = k == 0 ? (g_peers[0].rtt_at && now - g_peers[0].rtt_at < 5000 ? g_peers[0].rtt : 0) : relayed;
        }
        if (level > 0) {
            _snprintf(line, sizeof line, "level %d %d\n", k, level);
            out += line;
        }
        if (ping > 0) {
            _snprintf(line, sizeof line, "ping %d %d\n", k, ping);
            out += line;
            _snprintf(line, sizeof line, " %d: %d ms", k, ping);
            pings += line;
        }
    }
    static DWORD next_ping_log = 0; // in the journal too, every 10 s (players' own reports are vague)
    // 0 = not yet: int(now - 0) is negative after 24.8 days of uptime (no line at all in the 0.9.1 tests)
    if (!pings.empty() && (!next_ping_log || int(now - next_ping_log) >= 0)) {
        next_ping_log = (now + 10000) | 1;
        logf("net: pings (player: round trip):%s", pings.c_str());
    }
    if (g_steam && link_steam_self()) {
        _snprintf(line, sizeof line, "self %llu\n", (unsigned long long)link_steam_self());
        out += line;
    }
    // the launcher's salon: the game (mode, the world's clock), each player's Steam name and avatar
    uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0);
    _snprintf(line, sizeof line, "game %s %s %d %d\n", g_shared ? "shared" : "classic", g_steam ? "steam" : "lan",
              gm ? game::at<int32_t>(gm, game::kGsDay) : -1, gm ? game::at<int32_t>(gm, game::kGsDayMs) : -1);
    out += line;
    for (int k = 0; k < 4; ++k) {
        uint64_t id = 0;
        if (g_steam && g_role == NET_HOST)
            id = k == 0 ? link_steam_self() : g_peers[k - 1].used ? g_peers[k - 1].steam : 0;
        else if (g_steam && k == me)
            id = link_steam_self();
        else if (g_steam && g_have_snap) {
            EnterCriticalSection(&g_snap_lock);
            id = g_snap.steam_of[k];
            LeaveCriticalSection(&g_snap_lock);
        }
        std::string persona;
        salon_profile(k, id, persona);
        for (char &c : persona)
            if (c == '\n' || c == '\r')
                c = ' ';
        if (id) {
            _snprintf(line, sizeof line, "steam %d %s\n", k, persona.c_str());
            out += line;
        }
    }
    if (FILE *f = fopen("coop_status.tmp", "wb")) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        MoveFileExA("coop_status.tmp", "coop_status.txt", MOVEFILE_REPLACE_EXISTING);
    }
}

void host_save_pieces(Peer &peer);

// Host, each 33 ms: pick up a new save version, queue it when the guest lacks it, send 24 pieces.
void host_save_send(DWORD now)
{
    HostSave &s = g_hsave;
    std::vector<uint8_t> data;
    uint32_t version = 0, sig = 0;
    // a game back in the tutorial (a new game started with guests still connected): its save is not sent
    if (storage_own_save(data, version, &sig) && version != s.version && storage_host_ready()) {
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

// Shared world: each place's latest quest state, every 3 s for 30 s after it changed (the guest
// sends its own to the host; the host sends its own and the guests' to every other guest).
void quest_send(DWORD now)
{
    int me = g_role == NET_HOST ? 0 : (g_have_snap ? g_snap.my_slot : -1);
    for (int o = 0; o < 4; ++o) {
        if (g_role == NET_GUEST && o != me)
            continue;
        EnterCriticalSection(&g_qb_lock);
        QuestBlob &q = g_qb[o];
        bool due = !q.data.empty() && q.since && int(now - q.since) < 30000 && (!q.next || int(now - q.next) >= 0);
        std::vector<uint8_t> data = due ? q.data : std::vector<uint8_t>();
        uint32_t crc = q.crc;
        if (due)
            q.next = (now + 3000) | 1;
        LeaveCriticalSection(&g_qb_lock);
        if (data.empty())
            continue;
        for (size_t off = 0, i = 0; off < data.size(); off += sizeof(QuestChunkMsg::data), ++i) {
            QuestChunkMsg m = {};
            m.h = {kMagic, MSG_QUEST, ++g_seq};
            m.origin = uint8_t(o);
            m.crc = crc;
            m.total = uint32_t(data.size());
            m.index = uint16_t(i);
            m.len = uint16_t(std::min<size_t>(sizeof m.data, data.size() - off));
            memcpy(m.data, data.data() + off, m.len);
            if (g_role == NET_GUEST) {
                send_raw(&m, int(sizeof(Header) + 15 + m.len));
            } else {
                for (int k = 0; k < kMaxPeers; ++k)
                    if (g_peers[k].used && 1 + k != o)
                        send_to(g_peers[k], &m, int(sizeof(Header) + 15 + m.len));
            }
        }
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
        for (int s = 0; s < 4; ++s) { // world flag changes, the same way
            if (s == 1 + k)
                continue;
            uint32_t size = s == 0 ? flags_mine_count() : uint32_t(g_flags_log[s].size());
            uint32_t from = std::max(peer.flags_applied[s], peer.flags_joined[s]) + 1;
            if (from > size)
                continue;
            std::vector<FlagEv> evs;
            if (s == 0) {
                flags_net_mine(evs, from, kFlagsPage);
            } else {
                for (uint32_t q = from; q <= size && int(evs.size()) < kFlagsPage; ++q)
                    evs.push_back(g_flags_log[s][q - 1]);
            }
            FlagsMsg fm = {};
            fm.h = {kMagic, MSG_FLAGS, ++g_seq};
            fm.slot = uint8_t(s);
            fm.session = s == 0 ? g_loot_my_session : g_flags_session[s];
            fm.first = from;
            fm.n = uint8_t(evs.size());
            memcpy(fm.ev, evs.data(), evs.size() * sizeof(FlagEv));
            send_to(peer, &fm, int(sizeof(Header) + 10 + evs.size() * sizeof(FlagEv)));
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
        static DWORD next_status = 0;
        if (g_shared && (!next_status || int(now - next_status) >= 0)) {
            next_status = (now + 1000) | 1;
            write_status(now);
            for (Peer &p : g_peers) // the salon's pings: a round trip to each player once a second
                if (p.used && p.last_rx) {
                    PingMsg m = {{kMagic, MSG_PING, ++g_seq}, GetTickCount()};
                    send_to(p, &m, sizeof m);
                }
        }
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
                if (g_shared) {
                    revive_tick();
                    quest_sync_tick();
                    quest_send(now);
                }
                static DWORD flags_next = 0; // shared world: quest and world flags (flags.cpp)
                if (g_shared && (!flags_next || int(now - flags_next) >= 0)) {
                    flags_next = (now + 250) | 1;
                    flags_scan();
                }
                if (g_shared) { // its own character, for the host and the other players
                    PStateMsg ps = {};
                    ps.h = {kMagic, MSG_PSTATE, ++g_seq};
                    read_state(0, ps.me);
                    ps.stage = my_stage();
                    ps.exp_total = exp_sync_total();
                    ps.loot_session = g_loot_my_session;
                    memcpy(ps.loot_applied, g_loot_applied, sizeof ps.loot_applied);
                    memcpy(ps.flags_applied, g_flags_applied, sizeof ps.flags_applied);
                    ps.downed = revive_down() ? 1 : 0;
                    send_raw(&ps, sizeof ps);
                    area_lead_update();
                    if (g_lead_others && g_lead == net_my_slot())
                        send_area_enemies(now);
                    static DWORD loot_next = 0;
                    if (g_have_snap && (!loot_next || int(now - loot_next) >= 0)) { // its pickups the host lacks
                        loot_next = (now + 250) | 1;
                        EnterCriticalSection(&g_snap_lock);
                        uint32_t got = g_snap.loot_got, fgot = g_snap.flags_got;
                        int8_t slot = g_snap.my_slot;
                        LeaveCriticalSection(&g_snap_lock);
                        std::vector<FlagEv> fevs;
                        flags_net_mine(fevs, fgot + 1, kFlagsPage);
                        if (!fevs.empty()) {
                            FlagsMsg fm = {};
                            fm.h = {kMagic, MSG_FLAGS, ++g_seq};
                            fm.slot = uint8_t(slot);
                            fm.session = g_loot_my_session;
                            fm.first = fgot + 1;
                            fm.n = uint8_t(fevs.size());
                            memcpy(fm.ev, fevs.data(), fevs.size() * sizeof(FlagEv));
                            send_raw(&fm, int(sizeof(Header) + 10 + fevs.size() * sizeof(FlagEv)));
                        }
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
                if (g_shared) { // every player's own state and the host's main pawn; no hired pawns
                    bool taken[kParty] = {};
                    for (const Peer &p : g_peers)
                        if (p.used && p.pawn >= 0 && p.pawn < kPawns)
                            taken[1 + p.pawn] = true;
                    for (int i = 2; i < kParty; ++i) {
                        m.ch[i].valid = 0;
                        shared_set_hidden(i, !taken[i]);
                    }
                    if (taken[1])
                        m.ch[1].valid = 0; // the main pawn shows a guest (4 players)
                    for (Peer &p : g_peers)
                        if (p.used && p.pawn >= 0 && p.pstate_tick && now - p.pstate_tick < 2000) { // 2 s: states come in bursts over the internet
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
                    m.flags_got = uint32_t(g_flags_log[m.my_slot].size());
                    m.down[0] = revive_down() ? 1 : 0;
                    m.pawn_of[0] = -1;
                    for (int q = 0; q < kMaxPeers; ++q) {
                        m.down[1 + q] = g_peers[q].used ? g_peers[q].downed : 0;
                        m.pawn_of[1 + q] = int8_t(g_peers[q].used ? g_peers[q].pawn : -1);
                    }
                    m.steam_of[0] = g_steam ? link_steam_self() : 0;
                    for (int q = 0; q < kMaxPeers; ++q)
                        m.steam_of[1 + q] = g_steam && g_peers[q].used ? g_peers[q].steam : 0;
                    for (int q = 0; q < kMaxPeers; ++q)
                        m.ping_of[1 + q] = g_peers[q].used && g_peers[q].rtt_at && now - g_peers[q].rtt_at < 5000 ? g_peers[q].rtt : 0;
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
                if (g_shared) {
                    revive_tick();
                    quest_sync_tick();
                    quest_send(now);
                }
                static DWORD flags_next = 0;
                if (g_shared && (!flags_next || int(now - flags_next) >= 0)) {
                    flags_next = (now + 250) | 1;
                    flags_scan();
                }
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

// Tests (bridge "fakehold <slot> <ms>"): for that long the Arisen's state sent says it holds that player
// (RT from behind, 0xA9), wherever the two stand: the other worlds' "too far" path, whose log closed three
// games in the evening test (2026-10-30), without a pad on each PC.
volatile LONG g_fake_hold_slot = -1;
volatile LONG g_fake_hold_until = 0;
void net_test_fake_hold(int slot, int ms)
{
    g_fake_hold_slot = slot;
    g_fake_hold_until = LONG(GetTickCount() + DWORD(ms));
}

// Host, game thread (possess_pre, before the character's own update): copy its state for the SNAP.
void net_capture(uintptr_t ch, int i)
{
    if (!(g_role == NET_HOST || (g_shared && i == 0)) || i < 0 || i >= kParty)
        return;
    CharState s = {};
    capture_state(ch, s);
    s.hold_slot = -1;
    s.hold_enemy = -1;
    if (g_shared && i == 0) { // its action, and what it holds (shared_grab_look)
        s.action = game::at<int32_t>(ch, 0x2DD4);
        s.hold_kind = int8_t(game::at<int32_t>(ch, 0x3938));
        s.lantern = lantern_on(ch);
        look_of(ch, s.look);
        uintptr_t held = hold_action(s.action) ? game::at<uintptr_t>(ch, 0x38F0) : 0;
        for (int p = 0; held && p < kPawns; ++p)
            if (possess_pawn_ptr(p) == held)
                s.hold_slot = int8_t(party_to_slot(1 + p));
        if (held && s.hold_slot < 0)
            s.hold_enemy = int16_t(enemy_share_id(held));
        if (g_fake_hold_slot >= 0 && int(GetTickCount() - DWORD(g_fake_hold_until)) < 0) {
            s.action = 0xA9;
            s.hold_slot = int8_t(g_fake_hold_slot);
            held = 0;
        }
        if (held && s.hold_slot < 0) {
            float fx, fz;
            facing(ch, fx, fz);
            float dx = game::at<float>(held, 0x40) - game::at<float>(ch, 0x40), dz = game::at<float>(held, 0x48) - game::at<float>(ch, 0x48);
            s.hold_off[0] = dx * fx + dz * fz;
            s.hold_off[1] = game::at<float>(held, 0x44) - game::at<float>(ch, 0x44);
            s.hold_off[2] = dx * fz - dz * fx;
            // lifted, a monster's place (+0x40) is no longer where it is drawn: about 6000 u away and
            // 700 u up (lift suite, 2026-10-29: 64 u ahead in the grab, then -496/+737/6165 lifted);
            // the other worlds carry it above the holder's head instead
            if (strstr(action_name(ch), "Lift") || !(std::fabs(s.hold_off[0]) + std::fabs(s.hold_off[2]) < 300.f)) {
                s.hold_off[0] = 15.f;
                s.hold_off[1] = 180.f;
                s.hold_off[2] = 0.f;
            }
            static DWORD next_hold_log = 0;
            if (!next_hold_log || int(GetTickCount() - next_hold_log) >= 0) {
                next_hold_log = (GetTickCount() + 1000) | 1;
                logf("net: shared: holding enemy %d (%s): %.0f ahead, %+.0f up, %.0f right", s.hold_enemy, action_name(ch), s.hold_off[0],
                     s.hold_off[1], s.hold_off[2]);
            }
        }
    }
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
    InitializeCriticalSectionAndSpinCount(&g_cap_lock, 20000); // before g_role: net_capture runs once the role is set; spins (enemies_install)
    g_shared = early_flag("shared_world");
    InitializeCriticalSection(&g_qb_lock);
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
    InitializeCriticalSectionAndSpinCount(&g_snap_lock, 20000); // spinning: see enemies_install
    InitializeCriticalSectionAndSpinCount(&g_enemy_lock, 20000);
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

// The class hashes an enemy pairs with: its own and its variants (the last digit changed). The two
// worlds of the friends' evening (2026-10-28) often had another variant on each side (uEm0100 here,
// uEm0101 there) and those never paired.
struct ClassSet {
    uint32_t h[11];
    int n;
    bool has(uint32_t c) const
    {
        for (int k = 0; k < n; ++k)
            if (h[k] == c)
                return true;
        return false;
    }
};
static ClassSet class_family(const char *name)
{
    ClassSet out = {{class_hash(name)}, 1};
    size_t len = strlen(name);
    if (len == 0 || len >= 63 || name[len - 1] < '0' || name[len - 1] > '9')
        return out;
    char v[64];
    memcpy(v, name, len + 1);
    for (char d = '0'; d <= '9'; ++d)
        if (d != name[len - 1]) {
            v[len - 1] = d;
            out.h[out.n++] = class_hash(v);
        }
    return out;
}

// Game thread, right after an enemy's transform finalize (enemies.cpp, inside its table lock).
// Host: record its state. Guest: follow the host's enemy paired with it. Returns an enemy whose
// death enemies.cpp must run (enemy_net_death) once its lock is released, or 0.
uintptr_t enemy_net_after_finalize(EnemySlot &slot, int i)
{
    uintptr_t pending_death = 0;
    if (!g_enemy_ready || g_role == NET_OFF || i < 0 || i >= kEnemySlots || net_guest_roaming())
        return 0;
    int lead = int(g_lead);
    if (g_shared && g_role == NET_GUEST && lead < 0)
        return 0; // no one else around: this guest's own enemies
    bool lead_self = g_role == NET_HOST || (g_shared && lead == net_my_slot());
    uintptr_t em = slot.ptr;
    DWORD now = GetTickCount();
    if (!slot.settled) // no spawn point to pair on yet
        return 0;
    if (lead_self) {
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
    int64_t tw = perf_now();
    EnterCriticalSection(&g_enemy_lock);
    perf_add(PERF_WAIT, tw);
    Follow &f = g_follow[i];
    if (f.first_seen != slot.first_seen) // a new enemy in this local slot
        f = {slot.first_seen, -1, now, em, 0, false, 0, 0}; // never 0: int(now - 0) < 0 after 24.8 days of uptime
    f.last = now;
    if (f.host_slot >= 0 && (!g_enemy_tick[f.host_slot] || now - g_enemy_tick[f.host_slot] > 3000))
        f.host_slot = -1; // the host's enemy is gone (or the host stopped sending it)
    // never a dead one: a guest-only enemy removed (dead here) was paired 4 s later with a living host
    // enemy, and the two stayed "dead here, alive there" (enemy suite 2026-10-30 06:01, 9/10)
    if (f.host_slot < 0 && !slot.died && int(now - f.next_try) >= 0) {
        f.next_try = now + 500;
        uint32_t cls = class_hash(slot.name);
        ClassSet fam = class_family(slot.name);
        bool claimed[kEnemySlots] = {};
        for (int k = 0; k < kEnemySlots; ++k)
            if (k != i && g_follow[k].host_slot >= 0 && now - g_follow[k].last < 1000)
                claimed[g_follow[k].host_slot] = true;
        // 1. same class, spawn within 40 u; 2. same class, standing within 40 u now (critters such as
        // uEm8100 move away from their spawn after their first frames, not at the same moment on both PCs);
        // 3. once it has been here 6 s: same class or a variant, the closest standing within 800 u.
        // Packs (wolves, bats, harpies) spawn 100 to 1500 u apart in the two worlds (friends' logs,
        // 2026-10-28) and stayed apart: one of the same kind is as good as another.
        const float *here = reinterpret_cast<const float *>(em + 0x40);
        const char *how = "spawns";
        float best = 40.f * 40.f;
        for (int pass = 0; pass < 3 && f.host_slot < 0; ++pass) {
            if (pass == 2) {
                if (now - slot.first_seen < 6000) // passes 1 and 2 first: at 2 s it snapped critters (uEm8100) 784 u
                    break;
                best = 800.f * 800.f;
                how = "kind, closest";
            }
            const float *mine = pass == 0 ? slot.spawn : here;
            for (int k = 0; k < kEnemySlots; ++k) {
                const EnemyState &h = g_enemy_state[k];
                if (!g_enemy_tick[k] || now - g_enemy_tick[k] > 3000 || claimed[k] || (pass < 2 ? h.cls != cls : !fam.has(h.cls)))
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
                if (!g_enemy_tick[k] || !fam.has(h.cls))
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
        if (g_shared) { // the leader only has the enemies near it
            float lx = slot.spawn[0] - g_lead_pos[0], lz = slot.spawn[2] - g_lead_pos[2];
            host_sending &= lx * lx + lz * lz < kRemoveRange * kRemoveRange;
        }
        // and no host enemy of its class spawned there in the last 30 s (it may be one the host's
        // game updates rarely, or one taken by another local enemy for now)
        bool host_knows = false;
        ClassSet fam = class_family(slot.name);
        for (int k = 0; k < kEnemySlots && !host_knows; ++k) {
            const EnemyState &h = g_enemy_state[k];
            float dx = h.spawn[0] - slot.spawn[0], dy = h.spawn[1] - slot.spawn[1], dz = h.spawn[2] - slot.spawn[2];
            host_knows = g_enemy_tick[k] && now - g_enemy_tick[k] < 30000 && fam.has(h.cls) &&
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
    if (g_shared && (game::at<uintptr_t>(em, 0x2104) || carried_here(em))) { // held, lifted or carried here: it goes with its hands
        LeaveCriticalSection(&g_enemy_lock);
        return pending_death;
    }
    EnemyState s = g_enemy_state[f.host_slot];
    float *pos = reinterpret_cast<float *>(em + 0x40);
    float dx = s.pos[0] - pos[0], dy = s.pos[1] - pos[1], dz = s.pos[2] - pos[2];
    float err = std::sqrt(dx * dx + dy * dy + dz * dz);
    ++g_enemy_stats.applied;
    bool first = f.paired_to != f.host_slot + 1; // the pairing's own jump, logged with the pair
    f.paired_to = f.host_slot + 1;
    if (!first && err > g_enemy_stats.worst_err)
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
    // shared world, any role: a pawn showing another player takes no hits here (its HP is that
    // player's; on the host a downed guest's pawn was killed by spiders: cPlActDmgCrumbleDead)
    if (g_shared && g_role != NET_OFF)
        for (int p = 0; p < kPawns; ++p)
            if (ch == possess_pawn_ptr(p) && net_puppet(1 + p))
                return true;
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
        block = !g_enemy_state[f.host_slot].died && !(g_shared && g_lead == net_my_slot());
        ++(block ? g_enemy_stats.blocked : g_enemy_stats.allowed);
        break;
    }
    LeaveCriticalSection(&g_enemy_lock);
    return block;
}

// Guest, for a hit enemy_net_block_damage blocked: true when it is an enemy (not a player's pawn) whose
// HP is the host's. The hit then plays here at once (the stagger, the sparks, the sound), its damage cut
// so the HP stays above 0 (enemies.cpp); the host's HP comes back with the next state. Before, the hit
// did nothing here until the host's HP arrived: "the hits land late" (evening test 2026-10-30).
// no_hit_feel=1 in coop.ini turns it off.
bool enemy_net_feel_hit(uintptr_t ch)
{
    static int off = -1;
    if (off < 0)
        off = early_flag("no_hit_feel") ? 1 : 0;
    if (off || g_role != NET_GUEST || !g_enemy_ready)
        return false;
    for (int p = 0; p < kPawns; ++p)
        if (ch == possess_pawn_ptr(p))
            return false;
    if (ch == possess_arisen())
        return false;
    bool enemy = false;
    EnterCriticalSection(&g_enemy_lock);
    for (int i = 0; i < kEnemySlots && !enemy; ++i)
        enemy = g_follow[i].ptr == ch && g_follow[i].host_slot >= 0;
    if (enemy)
        ++g_enemy_stats.felt;
    LeaveCriticalSection(&g_enemy_lock);
    return enemy;
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

bool net_hidden_pawn_ptr(uintptr_t ch)
{
    if (!g_shared || g_role == NET_OFF || !ch)
        return false;
    for (int p = 0; p < kPawns; ++p)
        if (ch == possess_pawn_ptr(p))
            return g_rp_hidden[1 + p];
    return false;
}

bool net_puppet_ptr(uintptr_t ch)
{
    if (!g_shared || g_role == NET_OFF || !ch)
        return false;
    for (int p = 0; p < kPawns; ++p)
        if (ch == possess_pawn_ptr(p) && net_puppet(1 + p))
            return true;
    return false;
}

// A pawn showing a player whose last state is a weapon action (action number with a table in the high
// byte: 0x2060000 aiming the shortbow). actions.cpp refuses that pawn a grab then: RT shoots while aiming
// and grabs otherwise, and the replayed RT came a moment after the pawn's aim ended (bow test on the
// owner's Strider, 2026-10-30: cPlActGrabStart instead of the 4th shot).
bool net_puppet_in_weapon_action(uintptr_t ch)
{
    if (!g_shared || g_role == NET_OFF || !ch)
        return false;
    for (int p = 0; p < kPawns; ++p)
        if (ch == possess_pawn_ptr(p) && net_puppet(1 + p)) {
            EnterCriticalSection(&g_cap_lock);
            unsigned table = unsigned(g_rp[1 + p].action) >> 24;
            bool valid = g_rp[1 + p].valid;
            LeaveCriticalSection(&g_cap_lock);
            return valid && table >= 1 && table <= 0x0F;
        }
    return false;
}

// Shared world, guest: another player's game runs the enemies around it (its kills come from there).
bool net_enemy_follower()
{
    int lead = int(g_lead);
    return g_shared && g_role == NET_GUEST && lead >= 0 && lead != net_my_slot();
}

// Shared world, game threads: local party index i shows another player in this area now.
bool net_puppet(int i)
{
    if (!g_shared || i < 1 || i >= kParty)
        return false;
    // a pawn given to a connected player stays that player's, news or not: while a guest loaded an area it
    // sent no character for seconds, its pawn on the host went back to plain AI, the fall check ran on
    // it under the ground and it died (cPlActDmgDownDead, dead flag watch 2026-10-28 19:46)
    if (party_to_slot(i) >= 0)
        return true;
    EnterCriticalSection(&g_cap_lock);
    // 2 s without news before the pawn goes back to its AI: over the internet a player's states came in
    // bursts and its pawn walked off on its own meanwhile (first real test)
    bool on = (g_rp[i].valid || g_rp_away[i] || g_rp_hidden[i]) && g_rp_tick[i] && GetTickCount() - g_rp_tick[i] < 2000;
    LeaveCriticalSection(&g_cap_lock);
    return on;
}

// ---- shared world: players by place, for revive.cpp ----

int net_my_slot()
{
    if (g_role == NET_HOST)
        return 0;
    if (g_role != NET_GUEST || !g_have_snap)
        return -1;
    EnterCriticalSection(&g_snap_lock);
    int s = g_snap.my_slot;
    LeaveCriticalSection(&g_snap_lock);
    return s;
}

int32_t net_my_stage() { return my_stage(); }

bool net_player_info(int slot, PlayerInfo &out)
{
    out = PlayerInfo{};
    if (slot < 0 || slot >= 4 || !g_shared)
        return false;
    if (slot == net_my_slot()) {
        CharState c = {};
        read_state(0, c);
        out.present = true;
        out.downed = revive_down();
        out.stage = my_stage();
        memcpy(out.pos, c.pos, sizeof out.pos);
        return true;
    }
    if (g_role == NET_HOST) {
        const Peer &p = g_peers[slot - 1];
        // present while connected (packets heard in the last 10 s): over the internet its state came
        // in bursts, and a host down saw nobody up for a moment and died (first real test, 23:47)
        if (slot < 1 || !p.used || !p.pstate_tick || !p.last_rx || GetTickCount() - p.last_rx > 10000)
            return false;
        out.present = true;
        out.downed = p.downed;
        out.stage = p.pstage;
        memcpy(out.pos, p.pstate.pos, sizeof out.pos);
        return true;
    }
    if (!g_have_snap)
        return false;
    // the last time each place was seen: a player is still there for 3 s without news (its game
    // stops sending its state during a talk or a load)
    static PlayerInfo last[4];
    static DWORD last_at[4] = {};
    EnterCriticalSection(&g_snap_lock);
    int i = slot == 0 ? 0 : (g_snap.pawn_of[slot] >= 0 ? 1 + g_snap.pawn_of[slot] : -1);
    bool ok = i >= 0 && i < kParty && g_snap.ch[i].valid;
    if (ok) {
        out.present = true;
        out.downed = g_snap.down[slot] != 0;
        out.stage = g_snap.stage_of[i];
        memcpy(out.pos, g_snap.ch[i].pos, sizeof out.pos);
        last[slot] = out;
        last_at[slot] = GetTickCount() | 1;
    } else if (last_at[slot] && (slot == 0 || g_snap.pawn_of[slot] >= 0)) {
        // the host lists that player: still there, at the last place seen
        out = last[slot];
        out.downed = g_snap.down[slot] != 0;
        ok = true;
    }
    if (!ok) {
        static DWORD next = 0;
        if (!next || int(GetTickCount() - next) >= 0) {
            next = GetTickCount() + 2000;
            logf("net: player %d unknown: entry %d, valid %d, pawn_of %d %d %d %d, my_slot %d", slot, i,
                 i >= 0 && i < kParty ? g_snap.ch[i].valid : -1, g_snap.pawn_of[0], g_snap.pawn_of[1], g_snap.pawn_of[2],
                 g_snap.pawn_of[3], g_snap.my_slot);
        }
    }
    LeaveCriticalSection(&g_snap_lock);
    return ok;
}

void net_send_revive(int slot)
{
    ReviveMsg m = {{kMagic, MSG_REVIVE, ++g_seq}, uint8_t(slot)};
    if (g_role == NET_GUEST)
        send_raw(&m, sizeof m);
    else if (slot >= 1 && slot <= kMaxPeers && g_peers[slot - 1].used)
        send_to(g_peers[slot - 1], &m, sizeof m);
}

// Game thread: an area change to `to` (entry point `point`), the way the guests follow the host.
bool net_stage_jump(int32_t to, int32_t point)
{
    uintptr_t st = current_stage_area();
    if (!st || !plain_stage(to) || game::at<int32_t>(st, 0x808) != 1)
        return false;
    game::at<int32_t>(st, 0x80C) = to;
    game::at<int32_t>(st, 0x810) = point;
    game::at<uint8_t>(st, 0x814) = 0;
    game::at<int32_t>(st, 0x808) = 2;
    game::at<int32_t>(st, 4) = 0x13;
    logf("net: stage: jump to %d (point %d) to come back next to a player", to, point);
    return true;
}

// Shared world (quest_sync.cpp): this world's new quest state, for the other players.
void net_quest_publish(const std::vector<uint8_t> &blob, uint32_t crc)
{
    int me = g_role == NET_HOST ? 0 : net_my_slot();
    if (me < 0 || me >= 4)
        return;
    EnterCriticalSection(&g_qb_lock);
    g_qb[me].data = blob;
    g_qb[me].crc = crc;
    g_qb[me].since = GetTickCount() | 1;
    g_qb[me].next = 0;
    LeaveCriticalSection(&g_qb_lock);
}

// overlay.cpp, launcher status: a player with an older mod tried to talk to this one.
bool net_old_peer() { return g_old_peer != 0; }

