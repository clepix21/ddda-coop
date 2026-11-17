// Discord Rich Presence (0.11.0): "Playing DDDA Coop" with the co-op state on the player's Discord profile
// (host or guest, players in the session, since when), through the Discord client's local IPC pipe
// (\\.\pipe\discord-ipc-0..9: frames of opcode u32, length u32, JSON; handshake {"v":1,"client_id":...},
// then SET_ACTIVITY). No Discord SDK, nothing sent anywhere else: the pipe goes to the Discord app on this PC.
//
// The application is "DDDA Coop" on the Discord developer portal (its name is what Discord shows: "Playing DDDA
// Coop"; its art asset "logo"). coop.ini: discord=0 turns it off; discord_app_id=<id> another application.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "mod.h"
#include "version.h"

namespace {

constexpr const char *kAppId = "1572203907013287986"; // the DDDA Coop application (public, not a secret)
HANDLE g_pipe = INVALID_HANDLE_VALUE;
char g_app_id[32] = "";
int g_nonce = 0;

bool pipe_write(uint32_t op, const char *json)
{
    uint32_t len = uint32_t(strlen(json));
    char buf[2048];
    if (len + 8 > sizeof buf)
        return false;
    memcpy(buf, &op, 4);
    memcpy(buf + 4, &len, 4);
    memcpy(buf + 8, json, len);
    DWORD put = 0;
    return WriteFile(g_pipe, buf, len + 8, &put, nullptr) && put == len + 8;
}

// what Discord sent back (READY, replies): read and dropped, so its pipe never fills; false once closed
bool pipe_drain(char *last = nullptr, size_t cap = 0)
{
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr))
            return false;
        if (!avail)
            return true;
        char buf[4096];
        DWORD got = 0;
        if (!ReadFile(g_pipe, buf, avail < sizeof buf - 1 ? avail : sizeof buf - 1, &got, nullptr))
            return false;
        buf[got] = 0;
        if (last && cap && got > 8)
            snprintf(last, cap, "%s", buf + 8);
    }
}

void disconnect()
{
    if (g_pipe != INVALID_HANDLE_VALUE)
        CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
}

bool connect()
{
    for (int n = 0; n < 10; ++n) {
        wchar_t name[64];
        swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", n);
        HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        g_pipe = h;
        char hello[96];
        snprintf(hello, sizeof hello, "{\"v\":1,\"client_id\":\"%s\"}", g_app_id);
        if (!pipe_write(0, hello)) {
            disconnect();
            continue;
        }
        // READY comes back within a moment (or an error, e.g. an unknown application id)
        char reply[512] = "";
        for (int w = 0; w < 20 && !reply[0]; ++w) {
            Sleep(100);
            if (!pipe_drain(reply, sizeof reply))
                break;
        }
        if (strstr(reply, "\"READY\"")) {
            logf("discord: connected to the Discord app (pipe %d)", n);
            return true;
        }
        logf("discord: pipe %d did not accept the application id: %.200s", n, reply[0] ? reply : "(no reply)");
        disconnect();
        return false;
    }
    return false;
}

// JSON string content: quotes, backslashes and control characters out
void json_text(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (; *in && o + 2 < cap; ++in) {
        unsigned char c = static_cast<unsigned char>(*in);
        if (c == '"' || c == '\\')
            out[o++] = '\\', out[o++] = char(c);
        else if (c >= 0x20)
            out[o++] = char(c);
    }
    out[o] = 0;
}

// The Arisen's vocation and level, from its parameter object ([ch+0x3DEC]): the vocation at +0x10 (1 Fighter ..
// 9 Sorcerer, the save's mJob), the level at +0x700 (the save's mLevel); found with three test saves
// (Mystic Knight 200, Sorcerer 45, Strider 200), 2026-11-17. The character's name follows the level: not shown.
struct Vocation {
    const char *name, *asset; // the asset: a small image of the Discord application, by that name
};
const Vocation kVocations[] = {{"Fighter", "fighter"},   {"Strider", "strider"},   {"Mage", "mage"},
                               {"Mystic Knight", "mystic_knight"}, {"Assassin", "assassin"}, {"Magick Archer", "magick_archer"},
                               {"Warrior", "warrior"},   {"Ranger", "ranger"},     {"Sorcerer", "sorcerer"}};

bool arisen_vocation(int &job, int &level)
{
    uintptr_t a = possess_arisen(), prm = 0;
    if (!a || !mem_safe_read(a + 0x3DEC, &prm, 4) || !prm)
        return false;
    int32_t j = 0, l = 0;
    if (!mem_safe_read(prm + 0x10, &j, 4) || !mem_safe_read(prm + 0x700, &l, 4) || j < 1 || j > 9 || l < 1 || l > 200)
        return false;
    job = j;
    level = l;
    return true;
}

// Where the Arisen stands, by name (places.cpp, from the game files): "Pawn Guild, Gran Soren", "Verda
// Woodlands", "Cassardis"... empty when unknown
std::string where_name()
{
    uintptr_t a = possess_arisen();
    float p[3];
    if (!a || !mem_safe_read(a + 0x40, p, sizeof p))
        return "";
    field_to_world(p);
    std::string area, spot;
    place_names(net_my_stage(), p, area, spot);
    if (!spot.empty() && !area.empty() && area != "Gransys" && spot != area)
        return spot + ", " + area;
    return !spot.empty() ? spot : area;
}

struct Presence {
    char details[128], state[128], small_image[24], small_text[128], large_text[128];
    int players;
};

// The presence for now: what the player does, with how many, as what and where
void describe(Presence &p)
{
    NetRole role = net_role();
    p = {};
    for (int s = 0; s < 4; ++s) {
        PlayerInfo pi;
        if (net_player_info(s, pi) && pi.present)
            ++p.players;
    }
    const char *mode = role == NET_OFF ? "Solo" : net_shared() ? "Co-op, shared world" : "Co-op, classic mode";
    int job = 0, level = 0;
    if (arisen_vocation(job, level)) {
        snprintf(p.details, sizeof p.details, "%s, level %d", kVocations[job - 1].name, level);
        snprintf(p.small_image, sizeof p.small_image, "%s", kVocations[job - 1].asset);
        snprintf(p.small_text, sizeof p.small_text, "%s", mode);
    } else {
        snprintf(p.details, sizeof p.details, "%s", mode);
    }
    std::string where = where_name();
    const char *area = where.empty() ? nullptr : where.c_str();
    snprintf(p.large_text, sizeof p.large_text, "%s", "DDDA Coop " DDDA_COOP_VERSION);
    if (role == NET_OFF) {
        snprintf(p.state, sizeof p.state, "%s%s", area ? "Solo in " : "Not in a co-op session", area ? area : "");
        p.players = 0;
        return;
    }
    if (p.players < 1)
        p.players = 1;
    const char *what = role == NET_HOST ? (p.players > 1 ? "Hosting" : "Hosting, waiting for players")
                                        : (p.players > 1 ? "Playing as a guest" : "Joining a host");
    snprintf(p.state, sizeof p.state, "%s%s%s", what, area ? " in " : "", area ? area : "");
}

DWORD WINAPI discord_thread(void *)
{
    // the session's start, for "elapsed" on the profile
    long long start = 0;
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        start = ((static_cast<long long>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000LL) / 10000000LL;
    }
    char last_sent[1024] = "";
    DWORD next_connect = 0;
    for (;;) {
        Sleep(1000);
        if (g_pipe == INVALID_HANDLE_VALUE) {
            if (next_connect && int(GetTickCount() - next_connect) < 0)
                continue;
            next_connect = (GetTickCount() + 30000) | 1; // Discord may start later: retried every 30 s
            if (!connect())
                continue;
            last_sent[0] = 0;
        }
        if (!pipe_drain()) {
            logf("discord: the Discord app closed the pipe");
            disconnect();
            continue;
        }
        Presence pr;
        describe(pr);
        char d2[160], s2[160], st2[160], lt2[160];
        json_text(d2, sizeof d2, pr.details);
        json_text(s2, sizeof s2, pr.state);
        json_text(st2, sizeof st2, pr.small_text);
        json_text(lt2, sizeof lt2, pr.large_text);
        char party[96] = "", small[256] = "";
        if (pr.players > 0) // Discord shows "(2 of 4)" next to the state
            snprintf(party, sizeof party, ",\"party\":{\"id\":\"ddda-coop-%lu\",\"size\":[%d,4]}",
                     (unsigned long)GetCurrentProcessId(), pr.players);
        if (pr.small_image[0]) // the vocation's small image over the logo's corner
            snprintf(small, sizeof small, ",\"small_image\":\"%s\",\"small_text\":\"%s\"", pr.small_image, st2);
        char activity[1024];
        snprintf(activity, sizeof activity,
                 "{\"details\":\"%s\",\"state\":\"%s\",\"timestamps\":{\"start\":%lld},"
                 "\"assets\":{\"large_image\":\"logo\",\"large_text\":\"%s\"%s}%s}",
                 d2, s2, start, lt2, small, party);
        const char *details = pr.details, *state = pr.state;
        int players = pr.players;
        if (strcmp(activity, last_sent) == 0)
            continue; // Discord limits updates (about 5 per 20 s): sent only when it changes
        char frame[1400];
        snprintf(frame, sizeof frame, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":%s},\"nonce\":\"%d\"}",
                 (unsigned long)GetCurrentProcessId(), activity, ++g_nonce);
        if (!pipe_write(1, frame)) {
            logf("discord: write failed, reconnecting later");
            disconnect();
            continue;
        }
        snprintf(last_sent, sizeof last_sent, "%s", activity);
        logf("discord: presence: %s / %s%s", details, state, players > 0 ? "" : "");
    }
}

} // namespace

void discord_start()
{
    char on[8] = "";
    bool set = cfg_str("discord", on, sizeof on);
    if (set && on[0] == '0')
        return;
    // the test copies (private_appdata=1, tests only) run on a PC where the owner's Discord may be open: off
    // there unless discord=1 says otherwise
    if (!set && early_flag("private_appdata"))
        return;
    if (!cfg_str("discord_app_id", g_app_id, sizeof g_app_id) || !g_app_id[0])
        snprintf(g_app_id, sizeof g_app_id, "%s", kAppId);
    for (const char *c = g_app_id; *c; ++c)
        if (*c < '0' || *c > '9') { // an application id is a number
            logf("discord: discord_app_id is not a number: Rich Presence off");
            return;
        }
    if (HANDLE t = CreateThread(nullptr, 0, discord_thread, nullptr, 0, nullptr)) {
        SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(t);
    }
}

// Bridge "presence" (tests): what the presence would show now, in the log only (Discord not involved).
void discord_describe_log()
{
    Presence p;
    describe(p);
    logf("discord: would show: details \"%s\", state \"%s\", small image \"%s\" (%s), players %d", p.details, p.state,
         p.small_image, p.small_text, p.players);
}
