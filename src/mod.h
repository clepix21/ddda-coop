#pragma once
#include <windows.h>

#include <xinput.h>

#include <cstdint>
#include <string>
#include <vector>

void mod_start(HINSTANCE self);
void mod_stop();

// Log to coop.log next to DDDA.exe (printf-style, one line per call).
void logf(const char *fmt, ...);

// save_guard.cpp: Steam Cloud hooks: block writes (tests, guest), keep the host's latest save, and
// on the guest serve the host's save to the game's reads.
bool save_guard_install(bool block);
bool storage_guest_patch();
// savecodec.cpp: DDDA.sav unpack/pack with the game's zlib, and character blocks (pawn -1 = Arisen)
bool save_unpack(const std::vector<uint8_t> &file, std::string &xml, uint32_t head[8]);
bool save_pack(const std::string &xml, const uint32_t base_head[8], std::vector<uint8_t> &file);
bool save_character(const std::string &xml, int pawn, std::string &out);
bool save_put_character(std::string &xml, int pawn, const std::string &character);
std::string save_character_name(const std::string &character); // guest: accept a save owned by another Steam account (the host's)
bool storage_own_save(std::vector<uint8_t> &out, uint32_t &version, uint32_t *sig = nullptr);
void storage_set_peer_save(const uint8_t *data, size_t size, uint32_t version, uint32_t sig);
// Join during play (rejoin.cpp): which guests' characters a save holds, as a signature (0 = none).
// Host: the one the game should show (the merged save; stable = rebuilt from the latest own save) and
// the one it last read. Guest: the host's save it has and the one its game last read.
uint32_t storage_host_sigs(uint32_t *loaded, bool *stable, uint32_t *own_version, bool *arrivals);
uint32_t storage_guest_sigs(uint32_t *read, bool *read_any);
uint32_t save_crc(const void *p, size_t n); // savecodec.cpp: CRC-32 without the final inversion
uintptr_t current_stage_area();            // net.cpp: the current aStage, 0 elsewhere
// rejoin.cpp, net thread: host saves and reloads when the guests' characters in its world changed;
// a guest reloads when the host's save it has holds other characters than the one its game read
// (host_loaded_sig: what the host's game shows, from its SNAP).
void rejoin_tick(int role, uint32_t host_loaded_sig);
void net_capture(uintptr_t ch, int i); // host, game thread: party member i (0 = Arisen, 1 + pawn) for the SNAP
bool rejoin_busy(); // a reload of rejoin_tick is under way (guests: no stage following meanwhile)
uint32_t rejoin_loaded_sig(); // host: the signature of the characters its game shows
void storage_set_party_character(int pawn, const std::string &character); // host; empty = the host's own pawn
void storage_merge_tick();                                                 // host, net thread
bool storage_guest_character(std::string &out);                            // guest: its own Arisen
// progress.cpp: guest progress (a guest's character played as a hired pawn gets the party's exp,
// discipline and the items its pawn picked up, added to the guest's own save later)
void progress_host_loaded(const std::vector<uint8_t> &file, const uint32_t crc[]); // host: a load with guests' characters
void progress_host_write(const std::string &xml);         // host, a write with guests' characters (before un-merging)
bool progress_host_record(int pawn, std::string &out);   // host, net thread: the record for that pawn's guest
bool progress_guest_received(const std::string &rec);    // guest, net thread: keep it in coop_progress\pending.txt
bool progress_patch(std::vector<uint8_t> &bytes, bool load); // not a guest, keep_progress=1: add the pending gains
void progress_after_write();                              // not a guest: the game wrote its save
void save_guard_poll();
bool save_guard_active();
long save_guard_blocked();

// probe.cpp: hook on the per-character command update (possession + optional debug log).
bool probe_install();
int probe_npcs(uintptr_t *out, int cap); // uNpc updated in the last second (tests)

// pads.cpp: XInput hook (test bot, player 2 Back/Start filter, raw reads for the mod).
bool pads_install(int filtered_pad);
DWORD pads_read_raw(DWORD idx, XINPUT_STATE *s);
void pads_override(unsigned slot, XINPUT_GAMEPAD g, DWORD until);

// bridge.cpp: drive a running game from bridge_in.txt (tests only).
void bridge_start();

// possess.cpp: player 2 drives the main pawn; also reads coop.ini.
void possess_load_config();
bool cfg_block_saves();
bool cfg_debug_log();
bool cfg_bridge();
int possess_bridge_sign();
uintptr_t possess_arisen();
DWORD possess_last_update(); // tick of the Arisen's last update (0 = never)
void hang_note_main_thread(); // hang_watch.cpp: called on the game's main thread
void hang_watch_start();
uintptr_t possess_main_pawn();
void possess_set(bool on);
bool possess_on();
DWORD possess_auto_ms();
int possess_p2_pad();
int possess_pre(uintptr_t ch, uint8_t **copy);
void possess_post(int who, uint8_t *copy);
constexpr int kPawns = 3;                  // party pawns by mCmcId: 0 main, 1 and 2 hired
constexpr int kParty = 1 + kPawns;         // party index: 0 = Arisen, 1 + mCmcId = a pawn
int possess_party_index(int who);          // possess_pre token -> party index, -1 = none
void possess_pawn(int pawn, bool on, int slot, int camera_peer);
bool possess_pawn_on(int pawn);
uintptr_t possess_pawn_ptr(int pawn);      // by mCmcId, 0 = not seen

// input_redirect.cpp: per-thread input object for the command builder.
bool input_redirect_install();
bool input_redirect_active();
void input_redirect_set(uintptr_t input, bool hide_kbm, uintptr_t camera_holder = 0);

// multi_instance.cpp: tests only, let several copies run on one PC.
void multi_instance_install();
void keep_active_install();
void private_appdata_install();
bool window_place(int x, int y, int w, int h);
bool cfg_window(int &x, int &y, int &w, int &h);
bool early_flag(const char *key);

// net.cpp: two-PC co-op transport (milestone 1: host sends transforms, guest sends its pad).
enum NetRole { NET_OFF = 0, NET_HOST, NET_GUEST };
void net_start(NetRole role, const char *peer, int port);
NetRole net_role();
void net_apply_snapshot(uintptr_t ch, int party_index);
void net_host_record(uintptr_t ch, int party_index);
void net_apply_transform(uintptr_t ch, int party_index);
void net_guest_camera();
int camera_follow(uintptr_t from, uintptr_t to); // the camera follows `to` instead of `from` (guest, tests)
uintptr_t net_host_guest_camera_holder(int peer);
NetRole cfg_role();
const char *cfg_peer();
int cfg_port();
void pads_plug(unsigned slot);

// watch.cpp: tests only, hardware write watchpoint that logs the writing code.
void watch_start(uintptr_t addr, long count, uintptr_t owner = 0, bool down_only = false);

// multi_instance.cpp: string option from coop.ini.
bool cfg_str(const char *key, char *out, size_t cap);

// steam_link.cpp: Steam P2P transport for net.cpp (net thread only).
bool link_steam_start(NetRole role);
void link_steam_tick();
bool link_steam_send(const void *p, int n);                 // guest -> its host
bool link_steam_send_to(uint64_t to, const void *p, int n); // host -> one guest
int link_steam_recv(void *buf, int cap, uint64_t *from);
unsigned link_steam_short(uint64_t id);                     // last 4 digits, for logs

// enemies.cpp: live enemy table, fed by the transform finalize hook (calls from uEnemy slot 91).
struct EnemySlot {
    uintptr_t ptr, vt;
    char name[24];
    float spawn[4]; // position when first seen
    DWORD first_seen, last_seen;
    int index;
    DWORD died; // tick of its death (uEnemy death handler FUN_00AAB3B0), 0 = alive
    bool settled;      // spawn final: some classes are placed after their first frames (uEm8100)
};
bool mem_readable(const void *p, size_t n); // committed, readable memory
bool stages_install(); // stages.cpp: log every stage load
bool talk_install();   // talk.cpp: the talk start
uint32_t talk_last(float pos[3]); // the NPC of the latest talk start (position); talks started so far
uint32_t stages_other_loads(); // stage loads not made by a jump request (retry, load game)
DWORD stages_last_load(int32_t *stage); // tick (0 = none) and stage of the last stage load
bool enemies_install();
float *enemies_hp(uintptr_t ch);
void enemies_queue_arisen_damage(float dmg); // tests: the game's damage function, on the game thread
void enemies_run_queued(uintptr_t arisen);
void enemies_queue_arisen_kill();           // tests: the Arisen's dead flag, on the game thread  // possess_pre (game thread), for the Arisen // [[ch+0x1D18]]+8: {current HP, max HP}, or null
void enemies_on_finalize(uintptr_t em);
int enemies_snapshot(EnemySlot *out, int cap);
const char *enemies_class_of(uintptr_t obj);
// net.cpp: called under the enemy table lock, right after the enemy's own finalize.
bool enemy_net_block_damage(uintptr_t ch);
void net_guest_stage(); // guest, game thread: follow the host's stage changes
int enemy_net_follow(int local_slot); // guest: host slot it follows, -1 = none // guest: drop local damage to an enemy the host says is alive
void enemy_net_death(uintptr_t ch); // the game's own death handler (vtable slot 0x120), no hit info
uintptr_t enemy_net_after_finalize(EnemySlot &s, int index);
void enemies_dump(const char *name);
