// Steam P2P link for the two-PC co-op (milestone 3): no IP address, no port to open.
//
// The game has already called SteamAPI_Init with its own AppID (367500, both players own the game),
// so the mod reuses that session: SteamNetworking() / SteamUser() / SteamFriends() from the game's
// steam_api.dll, and the old ISteamNetworking P2P calls (Valve relays the traffic, NAT is handled).
//
// Finding each other, without codes:
//  - each side sets the rich presence key ddda_coop = host | guest (visible to Steam friends only);
//  - the guest looks for a friend in game 367500 whose ddda_coop is "host" (or peer_steamid=);
//  - the host accepts a P2P session from every friend in game 367500 (or only peer_steamid=)
//    and talks to each of them (up to 3 guests, net.cpp keeps the list).
// No callback object is registered with the game's callback dispatcher, so sessions are opened by
// sending: Steam accepts a session with a user one has sent to. The guest sends first (its INPUT);
// the host also sends a 9-byte hello every 2 s to each friend whose ddda_coop says guest (only the
// one-PC stand-in had tested the accept-before-request way, 2026-10-27). Friends' rich presence is
// requested every 10 s, and the friends seen in the game are logged when that list changes.
//
// A session that went quiet (no packet from that friend for 6 s, after it had talked): its state is
// logged and the session closed, at most every 20 s per friend; the next packets open a fresh one.
// First real test (2026-10-27 23:05): both guests and the host stopped hearing each other at the same
// moment while all three kept sending, and the link only came back 2.5 min later.
//
// Only the net thread calls into this file.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "mod.h"
#include "steam_link.h"

namespace {

constexpr uint32_t kAppId = 367500;
constexpr int kChannel = 3; // the game itself uses no P2P; a channel of our own anyway
constexpr int kSendUnreliable = 0;
constexpr int kFriendFlagImmediate = 4;

SteamFns g_f;
bool g_mock = false;
bool g_ready = false;
NetRole g_role = NET_OFF;
uint64_t g_self = 0, g_fixed_peer = 0, g_peer = 0;
DWORD g_next_try = 0;

// Steam ids in the log: last 4 digits only.
unsigned short_id(uint64_t id) { return unsigned(id % 10000); }

template <class T> bool resolve(HMODULE m, const char *name, T &out)
{
    out = reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(m, name)));
    if (!out)
        logf("steam: %s missing from steam_api.dll", name);
    return out != nullptr;
}

bool fill_real()
{
    HMODULE m = GetModuleHandleA("steam_api.dll");
    if (!m)
        return false;
    using Getter = void *(*)();
    Getter net, user, friends;
    bool ok = resolve(m, "SteamNetworking", net) && resolve(m, "SteamUser", user) && resolve(m, "SteamFriends", friends) &&
              resolve(m, "SteamAPI_ISteamUser_GetSteamID", g_f.GetSteamID) &&
              resolve(m, "SteamAPI_ISteamUser_BLoggedOn", g_f.BLoggedOn) &&
              resolve(m, "SteamAPI_ISteamNetworking_SendP2PPacket", g_f.SendP2PPacket) &&
              resolve(m, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable", g_f.IsP2PPacketAvailable) &&
              resolve(m, "SteamAPI_ISteamNetworking_ReadP2PPacket", g_f.ReadP2PPacket) &&
              resolve(m, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser", g_f.AcceptP2PSessionWithUser) &&
              resolve(m, "SteamAPI_ISteamFriends_GetFriendCount", g_f.GetFriendCount) &&
              resolve(m, "SteamAPI_ISteamFriends_GetFriendByIndex", g_f.GetFriendByIndex) &&
              resolve(m, "SteamAPI_ISteamFriends_GetFriendGamePlayed", g_f.GetFriendGamePlayed) &&
              resolve(m, "SteamAPI_ISteamFriends_GetFriendRichPresence", g_f.GetFriendRichPresence) &&
              resolve(m, "SteamAPI_ISteamFriends_SetRichPresence", g_f.SetRichPresence);
    if (!ok)
        return false;
    g_f.RequestFriendRichPresence = reinterpret_cast<decltype(g_f.RequestFriendRichPresence)>(
        reinterpret_cast<void *>(GetProcAddress(m, "SteamAPI_ISteamFriends_RequestFriendRichPresence")));
    g_f.CloseP2PSessionWithUser = reinterpret_cast<decltype(g_f.CloseP2PSessionWithUser)>(
        reinterpret_cast<void *>(GetProcAddress(m, "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser")));
    g_f.GetP2PSessionState = reinterpret_cast<decltype(g_f.GetP2PSessionState)>(
        reinterpret_cast<void *>(GetProcAddress(m, "SteamAPI_ISteamNetworking_GetP2PSessionState")));
    // the launcher's salon: names and avatars (missing ones only leave the salon without them)
    auto opt = [&](const char *name, auto &out) {
        out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(reinterpret_cast<void *>(GetProcAddress(m, name)));
    };
    opt("SteamAPI_ISteamFriends_GetPersonaName", g_f.GetPersonaName);
    opt("SteamAPI_ISteamFriends_GetFriendPersonaName", g_f.GetFriendPersonaName);
    opt("SteamAPI_ISteamFriends_GetMediumFriendAvatar", g_f.GetMediumFriendAvatar);
    opt("SteamAPI_ISteamUtils_GetImageSize", g_f.GetImageSize);
    opt("SteamAPI_ISteamUtils_GetImageRGBA", g_f.GetImageRGBA);
    Getter utils = nullptr;
    opt("SteamUtils", utils);
    g_f.utils = utils ? utils() : nullptr;
    g_f.net = net();
    g_f.user = user();
    g_f.friends = friends();
    return g_f.net && g_f.user && g_f.friends; // null until the game has initialised Steam
}

bool in_our_game(uint64_t id)
{
    FriendGameInfo info = {};
    return g_f.GetFriendGamePlayed(g_f.friends, id, &info) && uint32_t(info.game_id & 0xFFFFFF) == kAppId;
}

// Friends that can be the other player right now.
template <class Fn> void for_each_candidate(Fn fn)
{
    if (g_fixed_peer) {
        fn(g_fixed_peer);
        return;
    }
    int n = g_f.GetFriendCount(g_f.friends, kFriendFlagImmediate);
    for (int i = 0; i < n; ++i) {
        uint64_t id = g_f.GetFriendByIndex(g_f.friends, i, kFriendFlagImmediate);
        if (id && in_our_game(id))
            fn(id);
    }
}

// Friends heard from: when last, and when their session was last reset.
struct Heard {
    uint64_t id;
    DWORD last_rx, last_reset;
};
Heard g_heard[8] = {};
constexpr DWORD kQuietMs = 6000, kResetEveryMs = 20000;

void heard_from(uint64_t id)
{
    Heard *slot = nullptr, *free_slot = nullptr;
    for (Heard &h : g_heard) {
        if (h.id == id)
            slot = &h;
        else if (!h.id && !free_slot)
            free_slot = &h;
    }
    if (!slot)
        slot = free_slot ? free_slot : &g_heard[0];
    if (slot->id != id)
        *slot = Heard{id, 0, 0};
    slot->last_rx = GetTickCount() | 1;
}

// A friend that talked and went quiet: log its session, close it so the next send starts afresh.
void reset_quiet_sessions()
{
    DWORD now = GetTickCount();
    for (Heard &h : g_heard) {
        if (!h.id || !h.last_rx || int(now - h.last_rx) < int(kQuietMs))
            continue;
        if (h.last_reset && int(now - h.last_reset) < int(kResetEveryMs))
            continue;
        h.last_reset = now | 1;
        P2PSessionState st = {};
        bool known = g_f.GetP2PSessionState && g_f.GetP2PSessionState(g_f.net, h.id, &st);
        if (known)
            logf("steam: friend ...%04u quiet for %lu ms: session active %u, connecting %u, error %u, relay %u, "
                 "%d packets queued; closed, the next packets open a new one",
                 short_id(h.id), now - h.last_rx, st.active, st.connecting, st.error, st.using_relay, st.packets_queued);
        else
            logf("steam: friend ...%04u quiet for %lu ms: no session state; closed, the next packets open a new one",
                 short_id(h.id), now - h.last_rx);
        if (g_f.CloseP2PSessionWithUser)
            g_f.CloseP2PSessionWithUser(g_f.net, h.id);
        g_f.AcceptP2PSessionWithUser(g_f.net, h.id);
    }
}

} // namespace

// Returns true once Steam is up; call again until it does.
bool link_steam_start(NetRole role)
{
    if (g_ready)
        return true;
    DWORD now = GetTickCount();
    if (g_next_try && int(now - g_next_try) < 0)
        return false;
    g_next_try = now + 1000; // never 0 (see NOTES: int(now - 0) < 0 after 24.8 days of uptime)
    g_role = role;
    g_mock = early_flag("steam_mock");
    if (!(g_mock ? steam_mock_fill(g_f) : fill_real()))
        return false;
    if (!g_f.BLoggedOn(g_f.user))
        return false;
    g_self = g_f.GetSteamID(g_f.user);
    char buf[32];
    if (cfg_str("peer_steamid", buf, sizeof buf))
        g_fixed_peer = strtoull(buf, nullptr, 10);
    g_f.SetRichPresence(g_f.friends, "ddda_coop", role == NET_HOST ? "host" : "guest");
    g_ready = true;
    logf("steam: link up (%s), own id ...%04u, %s", g_mock ? "MOCK" : "real Steam", short_id(g_self),
         g_fixed_peer ? "fixed peer from coop.ini" : "peer found through the friends list");
    return true;
}

// Every ~2 s: keep presence fresh, accept candidates, the guest picks its host.
void link_steam_tick()
{
    if (!g_ready)
        return;
    g_f.SetRichPresence(g_f.friends, "ddda_coop", g_role == NET_HOST ? "host" : "guest");
    static DWORD next_request = 0;
    static std::string seen;
    DWORD now = GetTickCount();
    if (!next_request || int(now - next_request) >= 0) { // ask Steam for the friends' presence, list them
        next_request = (now + 10000) | 1;
        std::string list;
        for_each_candidate([&list](uint64_t id) {
            if (g_f.RequestFriendRichPresence)
                g_f.RequestFriendRichPresence(g_f.friends, id);
            const char *rp = g_f.GetFriendRichPresence(g_f.friends, id, "ddda_coop");
            char item[48];
            _snprintf(item, sizeof item, " ...%04u (%s)", short_id(id), rp && *rp ? rp : "no mod");
            list += item;
        });
        if (list != seen) {
            seen = list;
            logf("steam: friends in Dragon's Dogma:%s", list.empty() ? " none" : list.c_str());
        }
    }
    reset_quiet_sessions();
    if (g_role == NET_HOST) {
        for_each_candidate([](uint64_t id) {
            g_f.AcceptP2PSessionWithUser(g_f.net, id);
            // every friend in the game, not only those whose rich presence already says guest: Steam
            // may be slow to pass it on, and the guest's packets only come in once the host has sent
            // to it (a real test on 2026-10-27 22:49: the guest sent for 30 s and got nothing back)
            const uint8_t hello[9] = {0x44, 0x44, 0x43, 0x4F, 0, 0, 0, 0, 0}; // net.cpp Header: 'DDCO', type 0
            g_f.SendP2PPacket(g_f.net, id, hello, sizeof hello, kSendUnreliable, kChannel);
        });
        return;
    }
    if (g_peer) { // keep the session with the host accepted, and tell when nothing comes back
        g_f.AcceptP2PSessionWithUser(g_f.net, g_peer);
        return;
    }
    for_each_candidate([](uint64_t id) {
        if (g_peer)
            return;
        const char *rp = g_f.GetFriendRichPresence(g_f.friends, id, "ddda_coop");
        if (g_fixed_peer || (rp && strcmp(rp, "host") == 0)) {
            g_peer = id;
            g_f.AcceptP2PSessionWithUser(g_f.net, id);
            logf("steam: host found, friend ...%04u", short_id(id));
        }
    });
}

unsigned link_steam_short(uint64_t id) { return short_id(id); }

uint64_t link_steam_self() { return g_ready ? g_self : 0; }

// Net thread: a player's Steam name and medium avatar (64 x 64 RGBA) for the launcher's salon. The name
// comes as soon as Steam knows it; the avatar may take a few calls (-1: Steam is still downloading it).
bool link_steam_profile(uint64_t id, std::string &name, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h)
{
    if (!g_ready || !id)
        return false;
    const char *n = id == g_self ? (g_f.GetPersonaName ? g_f.GetPersonaName(g_f.friends) : nullptr)
                                 : (g_f.GetFriendPersonaName ? g_f.GetFriendPersonaName(g_f.friends, id) : nullptr);
    name = n ? n : "";
    rgba.clear();
    w = h = 0;
    if (!g_f.GetMediumFriendAvatar || !g_f.GetImageSize || !g_f.GetImageRGBA)
        return !name.empty();
    int img = g_f.GetMediumFriendAvatar(g_f.friends, id);
    if (img > 0 && g_f.GetImageSize(g_f.utils, img, &w, &h) && w > 0 && h > 0 && w <= 256 && h <= 256) {
        rgba.resize(size_t(w) * h * 4);
        if (!g_f.GetImageRGBA(g_f.utils, img, rgba.data(), int(rgba.size())))
            rgba.clear();
    }
    return !name.empty() || !rgba.empty();
}

// Guest: to its host. Host: to one guest.
bool link_steam_send_to(uint64_t to, const void *p, int n)
{
    return g_ready && to && g_f.SendP2PPacket(g_f.net, to, p, uint32_t(n), kSendUnreliable, kChannel);
}
bool link_steam_send(const void *p, int n) { return link_steam_send_to(g_peer, p, n); }

// One packet: guest, from its host only; host, from any friend whose session it accepted (*from
// tells which). Returns its size, or -1 when nothing is waiting.
int link_steam_recv(void *buf, int cap, uint64_t *from_out)
{
    *from_out = 0;
    if (!g_ready)
        return -1;
    for (;;) {
        uint32_t size = 0;
        if (!g_f.IsP2PPacketAvailable(g_f.net, &size, kChannel))
            return -1;
        uint64_t from = 0;
        uint32_t got = 0;
        if (!g_f.ReadP2PPacket(g_f.net, buf, uint32_t(cap), &got, &from, kChannel))
            return -1;
        if (g_role == NET_HOST || from == g_peer) {
            heard_from(from);
            *from_out = from;
            return int(got);
        }
        // a guest hears only its host (anyone else: a stale session): dropped
    }
}
