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
// Accepting before the session request arrives is enough with this API: no callback object is
// registered with the game's callback dispatcher.
//
// Only the net thread calls into this file.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    if (g_role == NET_HOST) {
        for_each_candidate([](uint64_t id) { g_f.AcceptP2PSessionWithUser(g_f.net, id); });
        return;
    }
    if (g_peer)
        return;
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
            *from_out = from;
            return int(got);
        }
        // a guest hears only its host (anyone else: a stale session): dropped
    }
}
