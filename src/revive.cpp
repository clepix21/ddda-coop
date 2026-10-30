// Shared world: a player at 0 HP goes down instead of dying while another player is up (owner's
// choice, 2026-10-27): it stays down (no movement, no more damage); another player in the same area
// walks up and holds B for 3 s to help it up (30 % of its HP). After 60 s down, or at once when no
// player is up in its area, it comes back next to a player who is up (an area change first if that
// player is elsewhere) with 30 % of its HP. When every player is down the game's own death runs
// ("Réessayer").
//
// Down looks down: the game's own near-death action (0x80, cPlActCmcNeardeath, the one a pawn at
// 0 HP plays: on the ground, a skull over its name) on this player's Arisen, and on the pawn that
// shows a downed player in the other worlds (net.cpp, shared_down_look); action 0 when up again.
//
// The blow that would take the Arisen to 0 HP is cut in the game's damage function (enemies.cpp's
// hook: HP entry, damage) so 1 HP is left; the HP entry is [[Arisen+0x1D18]]+0 (HP at +8, max at +0xC).
//
// Deaths that are no blow (friends' evening, 2026-10-28: a fall, deep water and Death's scythe in
// Bitterblack killed players nobody could help) come through revive_on_fatal (actions.cpp): the fall's
// HP change and the game's death actions (cPlAct*Dead, cPlActDipWater). The player goes down where
// it last stood on the ground (a second ago or more), not at the bottom of the cliff or in the water.
#include <windows.h>

#include <cmath>
#include <cstring>

#include "game.h"
#include "mod.h"

namespace {

constexpr DWORD kDownMs = 60000, kHelpMs = 3000;
constexpr float kHelpReach = 250.f, kBackShare = 0.3f;
volatile LONG g_down = 0;          // this player is down
DWORD g_down_at = 0;
volatile LONG g_get_up = 0;        // game thread: 1 = helped up here, 2 = come back next to `g_to`
volatile LONG g_die = 0;           // game thread: every player is down, let the game's death run
volatile LONG g_move_back = 0;     // game thread: down by a fall, water or an instant death: put it back
volatile LONG g_shore = 0;         // game thread: out of deep water, back on its feet where it last stood
float g_to[3] = {};
int32_t g_to_stage = -1;
DWORD g_help_since = 0;
int g_help_target = -1;

float *arisen_hp(uintptr_t a) { return a ? enemies_hp(a) : nullptr; }

// Someone else up: in this area (here = true) or anywhere.
int someone_up(bool here, PlayerInfo *out)
{
    int me = net_my_slot(), found = -1;
    int32_t stage = net_my_stage();
    for (int s = 0; s < 4 && found < 0; ++s) {
        PlayerInfo p;
        if (s == me || !net_player_info(s, p) || !p.present || p.downed)
            continue;
        if (here && p.stage != stage)
            continue;
        if (out && p.pos[0] == 0.f && p.pos[2] == 0.f)
            continue; // no place known yet (a load): not a place to come back to
        found = s;
        if (out)
            *out = p;
    }
    return found;
}

} // namespace

bool revive_down() { return g_down != 0; }
// Holding B next to a downed player: that B must not reach the game (it would talk to the pawn
// showing that player).
bool revive_helping() { return g_help_target >= 0; }

// enemies.cpp's damage hook, game thread: true = drop this blow. A blow that would take this
// player's Arisen to 0 HP while another player is up leaves 1 HP and puts it down.
bool revive_on_damage(uintptr_t ch, float *dmg)
{
    if (!net_shared() || !dmg || ch != possess_arisen())
        return false;
    if (g_down)
        return true; // down: no more damage
    float *hp = arisen_hp(ch);
    if (!hp || hp[0] - *dmg > 0.f)
        return false;
    if (someone_up(false, nullptr) < 0)
        return false; // nobody to help: the game's own death
    *dmg = hp[0] > 1.f ? hp[0] - 1.f : 0.f;
    g_down_at = GetTickCount() | 1;
    InterlockedExchange(&g_down, 1);
    logf("revive: down (HP %.0f, the blow cut to %.0f): another player can help with B, or back in 60 s", hp[0], *dmg);
    return false;
}

// actions.cpp, game thread: a death that is no blow (fall, deep water, instant death). True = this
// player goes down instead (another player is up); `move_back`: back where it last stood.
bool revive_on_fatal(uintptr_t ch, const char *why, bool move_back)
{
    if (!net_shared() || !ch || ch != possess_arisen())
        return false;
    if (g_down)
        return true;
    // deep water (the sea gives cPlActDmgHugebleDead, ocean test 2026-10-30): no death and no wait, back on
    // the shore at once with a little less HP (owner: 60 s down in the sea was too long), alone or not
    if (why && (strstr(why, "DipWater") || strstr(why, "HugebleDead"))) {
        if (!InterlockedExchange(&g_shore, 1))
            logf("revive: deep water (%s): back on the shore", why);
        return true;
    }
    if (someone_up(false, nullptr) < 0)
        return false; // nobody to help: the game's own death
    g_down_at = GetTickCount() | 1;
    InterlockedExchange(&g_move_back, move_back ? 1 : 0);
    InterlockedExchange(&g_down, 1);
    logf("revive: down (%s)%s: another player can help with B, or back in 60 s", why,
         move_back ? ", back where it last stood" : "");
    return true;
}

// Net thread, ~every 33 ms.
void revive_tick()
{
    if (!net_shared())
        return;
    DWORD now = GetTickCount();
    if (g_down) {
        PlayerInfo p;
        bool here = someone_up(true, &p) >= 0;
        // every player is down, and still so 3 s later (a player's news can come late)
        static DWORD all_down_since = 0;
        bool all_down = !here && someone_up(false, nullptr) < 0; // present at all, place known or not
        all_down_since = all_down ? (all_down_since ? all_down_since : now | 1) : 0;
        if (all_down && int(now - all_down_since) < 3000)
            return;
        if (all_down) {
            if (!g_die) {
                for (int q = 0; q < 4; ++q) {
                    PlayerInfo i;
                    bool ok = net_player_info(q, i);
                    logf("revive:   place %d%s: %s present %d down %d area %d", q, q == net_my_slot() ? " (me)" : "",
                         ok ? "known" : "unknown", i.present, i.downed, i.stage);
                }
                logf("revive: every player is down: the game's death");
                InterlockedExchange(&g_die, 1);
            }
            return;
        }
        if (!here || int(now - g_down_at) > int(kDownMs)) {
            if (!here && someone_up(false, &p) < 0)
                return; // the players up have no known place yet: wait
            memcpy(g_to, p.pos, sizeof g_to);
            g_to_stage = p.stage;
            if (!g_get_up) {
                logf("revive: back next to a player in area %d (%s)", p.stage, here ? "60 s down" : "nobody up in this area");
                InterlockedExchange(&g_get_up, 2);
            }
        }
        return;
    }
    // up: help a downed player in this area by holding B next to it
    XINPUT_STATE s = {};
    bool b = pads_read_raw(0, &s) == ERROR_SUCCESS && (s.Gamepad.wButtons & XINPUT_GAMEPAD_B);
    uintptr_t me = possess_arisen();
    int target = -1;
    for (int t = 0; t < 4 && b && me && target < 0; ++t) {
        PlayerInfo p;
        if (t == net_my_slot() || !net_player_info(t, p) || !p.present || !p.downed || p.stage != net_my_stage())
            continue;
        float dx = p.pos[0] - game::at<float>(me, 0x40), dz = p.pos[2] - game::at<float>(me, 0x48);
        if (std::sqrt(dx * dx + dz * dz) < kHelpReach)
            target = t;
    }
    static DWORD next_dbg = 0;
    if (b && (!next_dbg || int(now - next_dbg) >= 0)) {
        next_dbg = (now + 1000) | 1;
        for (int t = 0; t < 4; ++t) {
            PlayerInfo p;
            bool ok = t != net_my_slot() && net_player_info(t, p);
            if (ok)
                logf("revive: B held: place %d present %d down %d area %d (mine %d) at (%.0f, %.0f), me at (%.0f, %.0f)", t,
                     p.present, p.downed, p.stage, net_my_stage(), p.pos[0], p.pos[2], me ? game::at<float>(me, 0x40) : 0.f,
                     me ? game::at<float>(me, 0x48) : 0.f);
        }
    }
    // the same downed player while B stays held; a short miss (its state arrives in bursts) is not
    // a new start
    static DWORD last_ok = 0;
    if (target >= 0 && target == g_help_target) {
        last_ok = now | 1;
    } else if (target >= 0) {
        g_help_target = target;
        g_help_since = last_ok = now | 1;
        return;
    } else if (g_help_target >= 0 && b && last_ok && int(now - last_ok) < 500) {
        // keep counting
    } else {
        g_help_target = -1;
        g_help_since = 0;
        return;
    }
    if (g_help_since && int(now - g_help_since) >= int(kHelpMs)) {
        logf("revive: helped player %d up", g_help_target);
        net_send_revive(g_help_target); // the target kept through short misses, not this tick's
        g_help_since = 0;
        g_help_target = -1;
    }
}

// Net thread: another player helped this one up.
void revive_helped()
{
    if (g_down && !g_get_up) {
        logf("revive: helped up by another player");
        InterlockedExchange(&g_get_up, 1);
    }
}

// Game thread, the Arisen's update: getting up, coming back next to a player, or the game's death.
void revive_game_tick(uintptr_t arisen)
{
    if (!arisen)
        return;
    if (InterlockedExchange(&g_die, 0)) {
        uintptr_t e = game::at<uintptr_t>(game::at<uintptr_t>(arisen, 0x1D18), 0);
        if (e) {
            game::at<float>(e, 8) = 0.f;
            game::at<uint32_t>(e, 0x110) |= 1; // the dead flag the game's death code reads
        }
        InterlockedExchange(&g_down, 0);
        return;
    }
    static DWORD next_look = 0;
    DWORD now = GetTickCount();
    // where it last stood (waiting, walking, running), every second; the older of the last two
    static float stood[2][3] = {};
    static DWORD stood_at = 0;
    static int stood_n = 0;
    int act = game::at<int32_t>(arisen, 0x2DD4);
    if (!g_down && (act == 0 || act == 1 || act == 2) && (!stood_at || int(now - stood_at) >= 1000)) {
        memcpy(stood[0], stood[1], sizeof stood[0]);
        memcpy(stood[1], reinterpret_cast<void *>(arisen + 0x40), sizeof stood[1]);
        stood_at = now | 1;
        ++stood_n;
    }
    if (InterlockedExchange(&g_shore, 0)) {
        const float *here = reinterpret_cast<const float *>(arisen + 0x40);
        const float *to = here;
        for (int k = 0; k < 2 && to == here; ++k) { // the older place first: the newer one may be in the water
            const float *p = stood[k];
            float dx = p[0] - here[0], dy = p[1] - here[1], dz = p[2] - here[2];
            if (stood_n > 1 - k && dx * dx + dy * dy + dz * dz < 2000.f * 2000.f)
                to = p;
        }
        float at[3] = {to[0], to[1], to[2]};
        for (uintptr_t off : {uintptr_t(0x40), uintptr_t(0xE70), uintptr_t(0xE80), uintptr_t(0x1990)})
            memcpy(reinterpret_cast<void *>(arisen + off), at, 12);
        action_set(arisen, 0); // standing
        if (float *hp = enemies_hp(arisen)) { // a tenth of its HP, never the last one
            float cut = hp[1] > 0.f ? hp[1] * 0.1f : 0.f;
            hp[0] = hp[0] - cut < 1.f ? 1.f : hp[0] - cut;
        }
        logf("revive: back on the shore at (%.0f, %.0f, %.0f)%s", at[0], at[1], at[2], to == here ? " (no place it stood near)" : "");
    }
    if (g_down && InterlockedExchange(&g_move_back, 0) && stood_n > 0) {
        // the older of the last two, or the newer, whichever is within 2000 u: a sample far from here (a load, a
        // teleport in between) is not a place to put it back. (Stage 100's ground is near y 29850: the sea test
        // put it back at y 29849, which is right there.)
        const float *here = reinterpret_cast<const float *>(arisen + 0x40);
        auto close_by = [&](const float *p) {
            float dx = p[0] - here[0], dy = p[1] - here[1], dz = p[2] - here[2];
            return dx * dx + dy * dy + dz * dz < 2000.f * 2000.f;
        };
        const float *to = stood_n >= 2 && close_by(stood[0]) ? stood[0] : close_by(stood[1]) ? stood[1] : nullptr;
        if (!to) {
            logf("revive: no place it stood near (%.0f, %.0f, %.0f): left where it is", here[0], here[1], here[2]);
            to = here;
        }
        memcpy(reinterpret_cast<void *>(arisen + 0x40), to, 12);
        for (uintptr_t off : {uintptr_t(0xE70), uintptr_t(0xE80), uintptr_t(0x1990)})
            memcpy(reinterpret_cast<void *>(arisen + off), to, 12);
        logf("revive: put back where it last stood (%.0f, %.0f, %.0f)", to[0], to[1], to[2]);
    }
    if (g_down) { // an instant death may have left 0 HP and the dead flag: down is alive
        uintptr_t e = game::at<uintptr_t>(game::at<uintptr_t>(arisen, 0x1D18), 0);
        if (e && game::at<float>(e, 8) < 1.f)
            game::at<float>(e, 8) = 1.f;
        if (e && (game::at<uint32_t>(e, 0x110) & 1))
            game::at<uint32_t>(e, 0x110) &= ~1u;
    }
    if (g_down && !g_get_up && (!next_look || int(now - next_look) >= 0)) {
        next_look = (now + 500) | 1;
        if (strcmp(action_name(arisen), "cPlActCmcNeardeath") != 0)
            action_lie_down(arisen);
    }
    LONG how = g_get_up;
    if (!how)
        return;
    if (how == 2 && g_to_stage != net_my_stage()) { // the other player is elsewhere: go there first
        static DWORD asked = 0;
        if (!asked || int(GetTickCount() - asked) > 15000) {
            asked = GetTickCount() | 1;
            net_stage_jump(g_to_stage, 0);
        }
        return;
    }
    if (how == 2) {
        float to[3] = {g_to[0] + 80.f, g_to[1], g_to[2]};
        memcpy(reinterpret_cast<void *>(arisen + 0x40), to, sizeof to);
        for (uintptr_t off : {uintptr_t(0xE70), uintptr_t(0xE80), uintptr_t(0x1990)})
            memcpy(reinterpret_cast<void *>(arisen + off), to, sizeof to);
    }
    if (float *hp = arisen_hp(arisen))
        hp[0] = hp[1] * kBackShare;
    InterlockedExchange(&g_get_up, 0);
    InterlockedExchange(&g_down, 0);
    if (strcmp(action_name(arisen), "cPlActCmcNeardeath") == 0)
        action_stand_up(arisen);
    logf("revive: up again (%s)", how == 1 ? "helped" : "next to another player");
}
