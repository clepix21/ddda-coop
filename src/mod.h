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
bool save_put_character_at(std::string &xml, int pawn, const std::string &character, size_t from);
bool save_swap_arisen_pawn(std::string &xml, int pawn);                       // in every player data copy
bool save_put_arisen_everywhere(std::string &xml, const std::string &character);
std::string save_copy_names(const std::string &xml); // for logs
bool save_bag(const std::string &xml, int who, std::string &out);             // 0 = Arisen, 1 + mCmcId
bool save_put_bag_everywhere(std::string &xml, int who, const std::string &bag);
bool save_quest_state(const std::string &xml, std::string &out);        // shared world: quests, NPCs, talks
bool save_put_quest_state(std::string &xml, const std::string &state);
uint32_t save_quest_list_crc(const std::string &state);              // its quest list only
bool save_deflate(const std::string &in, std::vector<uint8_t> &out);    // the game's zlib, length first
bool save_inflate(const std::vector<uint8_t> &in, std::string &out);
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
// roam.cpp: free roam, a guest explores on its own in its copy of the host's world, then comes back
void roam_tick();          // guest, net thread
bool roam_busy();          // leaving or coming back
bool net_guest_roaming();  // the guest plays its own Arisen, no host sync
bool net_shared();         // net.cpp: shared_world=1, every player its own character in its own world copy
bool net_puppet(int i);    // shared world: local party index i shows another player in this area
bool roam_world_from_host(const std::vector<uint8_t> &host, std::vector<uint8_t> &out); // roam.cpp
bool roam_refresh_party(const std::vector<uint8_t> &own, const std::vector<uint8_t> &host, std::vector<uint8_t> &out);
std::vector<size_t> player_data_starts(const std::string &x); // savecodec.cpp
void storage_set_override_save(const std::vector<uint8_t> &file); // save_guard.cpp
bool storage_own_raw(std::vector<uint8_t> &out, uint32_t &version);
bool storage_peer_raw(std::vector<uint8_t> &out);
void storage_guest_mark_read(uint32_t sig);
bool net_shared_in_host_area(); // shared world, guest: in the host's area now
bool net_enemy_follower();      // shared world, guest: another player's game runs the enemies around it
// exp_sync.cpp: shared world, one exp for all players
bool exp_sync_install();
long long exp_sync_total();                 // exp this world gave its Arisen
void exp_sync_seen(int slot, long long total); // another player's total (net thread)
void exp_sync_apply();                      // game thread: give what the others earned
bool loot_install();                        // loot.cpp: shared world loot
void progress_shared_gain(long long exp, long long jp, int item, int count, unsigned flags); // guest: keep for its own save
struct LootEv { uint16_t item, count; uint32_t flags; };
void loot_net_mine(std::vector<LootEv> &out, uint32_t from_seq, int max); // this player's pickups from seq
uint32_t loot_mine_count();
void loot_net_apply(const LootEv &e);       // net thread: another player's pickup, applied on the game thread
const char *action_name(uintptr_t ch);       // actions.cpp: the class of a character's current action
const char *action_class(int n);             // actions.cpp: the class of action number n ("" if none)
void action_set(uintptr_t ch, int number);
void action_stand_up(uintptr_t ch);
void action_lie_down(uintptr_t ch);          // into near death (0x80), the HP entry flags remembered          // up from near death (0x80), its flags cleared   // game thread: change it (vtable slot 0x210)
void action_queue(int who, int number);      // tests: 0 = the Arisen, 1..3 = pawn 0..2
void action_tick(uintptr_t arisen);
void action_queue_grab(int pawn);
bool actions_install();
void actions_quiet_save();                    // the next seconds of saving show no save icon (mod snapshots)                       // the fall check skips the pawns showing players
bool net_puppet_ptr(uintptr_t ch);
bool net_hidden_pawn_ptr(uintptr_t ch);       // net.cpp: a hired pawn no player uses (shared world)            // net.cpp: a pawn showing another player (shared world)            // tests: the Arisen holds that pawn (0xA9 / 0xAA)          // game thread
void quest_live_queue(uint32_t id);        // quest_sync.cpp: tests, add a quest to the journal live
void quest_live_tick();                    // game thread
int quest_live_count();
void loot_apply_queued();                   // game thread
// flags.cpp: shared world, quest and world flags
#pragma pack(push, 1)
struct FlagEv { uint16_t index; uint32_t set, clr; };
#pragma pack(pop)
bool flags_install();
void flags_scan();                           // net thread: this world's changes
uint32_t flags_mine_count();
void flags_net_mine(std::vector<FlagEv> &out, uint32_t from_seq, int max);
void flags_net_apply(const FlagEv &e);       // net thread: another world's change
// revive.cpp: shared world, down and helped up instead of dying
struct PlayerInfo {
    bool present, downed;
    int32_t stage;
    float pos[3];
};
bool net_player_info(int slot, PlayerInfo &out); // net.cpp: player by place (0 = host, 1 + guest)
int net_my_slot();
int32_t net_my_stage();
void net_send_revive(int slot);
bool net_stage_jump(int32_t to, int32_t point);  // game thread
bool revive_down();
bool revive_helping();                             // B held next to a downed player
bool revive_on_damage(uintptr_t ch, float *dmg);  // game thread, the damage hook
std::string roam_pawn_name(int p);               // roam.cpp, guest: the name on pawn p of its world
bool net_old_peer();                             // net.cpp: a player with an older mod was heard
void overlay_install();                           // overlay.cpp: the "mod active" badge on the title screen
bool revive_on_fatal(uintptr_t ch, const char *why, bool move_back); // game thread: a death that is not a blow
void revive_tick();                               // net thread
void revive_helped();                             // net thread
void revive_game_tick(uintptr_t arisen);          // game thread
// quest_sync.cpp: shared world, the quest list in every world (a short reload)
void quest_sync_init();
void quest_sync_local_change();                   // flags.cpp: a quest flag changed in this world
void quest_sync_received(const std::string &state, uint32_t crc); // net thread
void quest_sync_tick();                           // net thread
bool quest_sync_busy();
void net_quest_publish(const std::vector<uint8_t> &blob, uint32_t crc); // net.cpp
int net_guest_my_pawn();   // net.cpp: the pawn the host gave this guest, -1 = none
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
bool rift_install();   // rift.cpp: the Rift stone does nothing while co-op is on
uint32_t rift_blocked();
uint32_t talk_last(float pos[3]); // the NPC of the latest talk start (position); talks started so far
uint32_t stages_other_loads(); // stage loads not made by a jump request (retry, load game)
DWORD stages_last_load(int32_t *stage); // tick (0 = none) and stage of the last stage load
bool enemies_install();
float *enemies_hp(uintptr_t ch);
void enemies_queue_arisen_damage(float dmg); // tests: the game's damage function, on the game thread
void enemies_run_queued(uintptr_t arisen);
void enemies_queue_pawn_kill(int pawn);     // tests: a pawn's HP 0 and dead flag, on the game thread
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
