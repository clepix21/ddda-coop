// Tests only: a stand-in for the Steam calls of steam_link.cpp, so two copies on one PC (one Steam
// account) can test the Steam path. coop.ini:
//   steam_mock=1, mock_id=<1..9> (own fake account), mock_friends=<ids, comma separated>,
//   mock_dir=<folder shared by the copies, default ..\mock_steam>,
//   mock_loss=<% of packets dropped>, mock_latency=<ms added on receive>.
// What it copies from real Steam (what the link relies on):
//  - packets from a user that was neither accepted nor sent to are held, not delivered;
//  - unreliable packets over 1200 bytes are refused;
//  - "in game" and rich presence come from the friend's own process (a presence file in mock_dir,
//    rewritten on every SetRichPresence; older than 10 s = not in game).
// Packets travel over 127.0.0.1 UDP, port 27300 + mock id, as [from id u64][channel u32][data].
#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mod.h"
#include "steam_link.h"

namespace {

constexpr uint64_t kAccountBase = 76561197960265728ull; // individual accounts
constexpr uint32_t kAppId = 367500;

int g_id = 1;
int g_friends[8];
int g_nfriends = 0;
char g_dir[MAX_PATH] = "..\\mock_steam";
int g_loss = 0, g_latency = 0;
SOCKET g_sock = INVALID_SOCKET;
bool g_accepted[16] = {}; // by mock id
bool g_held_logged[16] = {};
char g_rp_key[32] = "", g_rp_value[32] = "";

struct Packet {
    DWORD due;
    int from, channel, size;
    uint8_t data[1500];
};
constexpr int kQueue = 256;
Packet g_q[kQueue];
int g_qn = 0;

int mock_index(uint64_t id)
{
    uint64_t n = id - kAccountBase;
    return n >= 1 && n < 16 ? int(n) : 0;
}

void presence_path(int id, char *out, size_t cap) { snprintf(out, cap, "%s\\%d.txt", g_dir, id); }

void pump()
{
    for (;;) {
        uint8_t buf[1600];
        int n = recv(g_sock, reinterpret_cast<char *>(buf), sizeof buf, 0);
        if (n < 12)
            break;
        if (g_qn == kQueue)
            continue;
        if (g_loss && rand() % 100 < g_loss)
            continue;
        Packet &p = g_q[g_qn++];
        uint64_t from;
        memcpy(&from, buf, 8);
        memcpy(&p.channel, buf + 8, 4);
        p.from = mock_index(from);
        p.size = n - 12;
        memcpy(p.data, buf + 12, size_t(p.size));
        p.due = GetTickCount() + DWORD(g_latency);
    }
}

// First due packet on that channel from an accepted user; held ones stay queued.
int find(int channel)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < g_qn; ++i) {
        Packet &p = g_q[i];
        if (p.channel != channel || int(now - p.due) < 0)
            continue;
        if (!g_accepted[p.from]) {
            if (!g_held_logged[p.from]) {
                g_held_logged[p.from] = true;
                logf("steam mock: session request from fake account %d held until accepted", p.from);
            }
            continue;
        }
        return i;
    }
    return -1;
}

uint64_t m_GetSteamID(void *) { return kAccountBase + uint64_t(g_id); }
bool m_BLoggedOn(void *) { return true; }

bool m_Send(void *, uint64_t to, const void *data, uint32_t size, int type, int channel)
{
    int i = mock_index(to);
    if (!i || (type < 2 && size > 1200) || size > 1400)
        return false;
    g_accepted[i] = true; // sending to someone accepts their session, as on Steam
    uint8_t buf[1600];
    uint64_t self = m_GetSteamID(nullptr);
    memcpy(buf, &self, 8);
    memcpy(buf + 8, &channel, 4);
    memcpy(buf + 12, data, size);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(u_short(27300 + i));
    sendto(g_sock, reinterpret_cast<char *>(buf), int(size + 12), 0, reinterpret_cast<sockaddr *>(&a), sizeof a);
    return true; // unreliable: delivery is not promised
}

bool m_Avail(void *, uint32_t *size, int channel)
{
    pump();
    int i = find(channel);
    if (i < 0)
        return false;
    *size = uint32_t(g_q[i].size);
    return true;
}

bool m_Read(void *, void *dest, uint32_t cap, uint32_t *size, uint64_t *from, int channel)
{
    int i = find(channel);
    if (i < 0)
        return false;
    Packet &p = g_q[i];
    uint32_t n = uint32_t(p.size) < cap ? uint32_t(p.size) : cap;
    memcpy(dest, p.data, n);
    *size = n;
    *from = kAccountBase + uint64_t(p.from);
    memmove(&g_q[i], &g_q[i + 1], sizeof(Packet) * size_t(g_qn - i - 1));
    --g_qn;
    return true;
}

bool m_Accept(void *, uint64_t user)
{
    int i = mock_index(user);
    if (i)
        g_accepted[i] = true;
    return i != 0;
}

int m_FriendCount(void *, int) { return g_nfriends; }
uint64_t m_FriendByIndex(void *, int index, int) { return index >= 0 && index < g_nfriends ? kAccountBase + uint64_t(g_friends[index]) : 0; }

// Reads the friend's presence file; false if absent or stale.
bool read_presence(int id, char *key_value, size_t cap)
{
    char path[MAX_PATH];
    presence_path(id, path, sizeof path);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a))
        return false;
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER now, w;
    now.LowPart = now_ft.dwLowDateTime, now.HighPart = now_ft.dwHighDateTime;
    w.LowPart = a.ftLastWriteTime.dwLowDateTime, w.HighPart = a.ftLastWriteTime.dwHighDateTime;
    if (now.QuadPart - w.QuadPart > 10ull * 10000000ull)
        return false;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    key_value[0] = 0;
    if (fgets(key_value, int(cap), f))
        key_value[strcspn(key_value, "\r\n")] = 0;
    fclose(f);
    return true;
}

bool m_GamePlayed(void *, uint64_t user, FriendGameInfo *info)
{
    char kv[80];
    int i = mock_index(user);
    if (!i || !read_presence(i, kv, sizeof kv))
        return false;
    memset(info, 0, sizeof *info);
    info->game_id = kAppId;
    return true;
}

const char *m_GetRP(void *, uint64_t user, const char *key)
{
    static char value[64];
    char kv[80];
    value[0] = 0;
    int i = mock_index(user);
    size_t n = strlen(key);
    if (i && read_presence(i, kv, sizeof kv) && strncmp(kv, key, n) == 0 && kv[n] == '=')
        snprintf(value, sizeof value, "%s", kv + n + 1);
    return value; // "" when unset, as on Steam
}

bool m_SetRP(void *, const char *key, const char *value)
{
    snprintf(g_rp_key, sizeof g_rp_key, "%s", key);
    snprintf(g_rp_value, sizeof g_rp_value, "%s", value);
    char path[MAX_PATH];
    presence_path(g_id, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fprintf(f, "%s=%s\n", g_rp_key, g_rp_value);
    fclose(f);
    return true;
}

const char *m_PersonaName(void *)
{
    static char n[32];
    snprintf(n, sizeof n, "Testeur %d", g_id);
    return n;
}
const char *m_FriendPersonaName(void *, uint64_t user)
{
    static char n[32];
    snprintf(n, sizeof n, "Testeur %d", int(user - kAccountBase));
    return n;
}
int m_Avatar(void *, uint64_t user) { return int(user - kAccountBase); } // image handle = fake account
bool m_ImageSize(void *, int, uint32_t *w, uint32_t *h)
{
    *w = *h = 64;
    return true;
}
bool m_ImageRGBA(void *, int image, uint8_t *dest, int size)
{
    if (size < 64 * 64 * 4)
        return false;
    static const uint8_t colors[10][3] = {{120, 120, 120}, {180, 50, 50},  {50, 120, 190}, {60, 150, 80}, {200, 150, 40},
                                          {130, 70, 170}, {40, 160, 160}, {200, 90, 130}, {110, 110, 40}, {90, 90, 200}};
    const uint8_t *c = colors[image % 10];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            uint8_t *p = dest + (y * 64 + x) * 4;
            bool stripe = ((x + y) / 8) % 2 == 0; // diagonal stripes: a recognisable fake picture
            p[0] = uint8_t(stripe ? c[0] : c[0] / 2);
            p[1] = uint8_t(stripe ? c[1] : c[1] / 2);
            p[2] = uint8_t(stripe ? c[2] : c[2] / 2);
            p[3] = 255;
        }
    return true;
}

} // namespace

bool steam_mock_fill(SteamFns &f)
{
    static bool done = false;
    if (!done) {
        done = true;
        char buf[128];
        if (cfg_str("mock_id", buf, sizeof buf))
            g_id = atoi(buf);
        if (g_id < 1 || g_id > 9)
            g_id = 1;
        if (cfg_str("mock_friends", buf, sizeof buf))
            for (char *t = strtok(buf, ","); t && g_nfriends < 8; t = strtok(nullptr, ","))
                if (int v = atoi(t); v >= 1 && v <= 9 && v != g_id)
                    g_friends[g_nfriends++] = v;
        cfg_str("mock_dir", g_dir, sizeof g_dir);
        if (cfg_str("mock_loss", buf, sizeof buf))
            g_loss = atoi(buf);
        if (cfg_str("mock_latency", buf, sizeof buf))
            g_latency = atoi(buf);
        CreateDirectoryA(g_dir, nullptr);
        srand(GetTickCount());
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        u_long nb = 1;
        ioctlsocket(g_sock, FIONBIO, &nb);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(u_short(27300 + g_id));
        if (bind(g_sock, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0)
            logf("steam mock: bind 127.0.0.1:%d failed (%d)", 27300 + g_id, WSAGetLastError());
        logf("steam mock: fake account %d, %d friend(s), loss %d%%, latency %d ms, presence in %s", g_id, g_nfriends,
             g_loss, g_latency, g_dir);
    }
    f.net = f.user = f.friends = &g_id; // any non-null pointer
    f.GetSteamID = m_GetSteamID;
    f.BLoggedOn = m_BLoggedOn;
    f.SendP2PPacket = m_Send;
    f.IsP2PPacketAvailable = m_Avail;
    f.ReadP2PPacket = m_Read;
    f.AcceptP2PSessionWithUser = m_Accept;
    f.GetFriendCount = m_FriendCount;
    f.GetFriendByIndex = m_FriendByIndex;
    f.GetFriendGamePlayed = m_GamePlayed;
    f.GetFriendRichPresence = m_GetRP;
    f.SetRichPresence = m_SetRP;
    f.utils = &g_id;
    f.GetPersonaName = m_PersonaName;
    f.GetFriendPersonaName = m_FriendPersonaName;
    f.GetMediumFriendAvatar = m_Avatar;
    f.GetImageSize = m_ImageSize;
    f.GetImageRGBA = m_ImageRGBA;
    return true;
}
