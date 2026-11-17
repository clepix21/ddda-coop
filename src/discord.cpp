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

// The presence for now: what the player does and with how many
void describe(char *details, size_t dcap, char *state, size_t scap, int &players)
{
    NetRole role = net_role();
    players = 0;
    for (int s = 0; s < 4; ++s) {
        PlayerInfo pi;
        if (net_player_info(s, pi) && pi.present)
            ++players;
    }
    if (role == NET_OFF) {
        snprintf(details, dcap, "Solo");
        snprintf(state, scap, "Not in a co-op session");
        players = 0;
        return;
    }
    if (players < 1)
        players = 1;
    snprintf(details, dcap, "%s", net_shared() ? "Co-op, shared world" : "Co-op, classic mode");
    if (role == NET_HOST)
        snprintf(state, scap, players > 1 ? "Hosting" : "Hosting, waiting for players");
    else
        snprintf(state, scap, players > 1 ? "Playing as a guest" : "Joining a host");
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
        char details[128], state[128], d2[160], s2[160];
        int players;
        describe(details, sizeof details, state, sizeof state, players);
        json_text(d2, sizeof d2, details);
        json_text(s2, sizeof s2, state);
        char party[96] = "";
        if (players > 0) // Discord shows "(2 of 4)" next to the state
            snprintf(party, sizeof party, ",\"party\":{\"id\":\"ddda-coop-%lu\",\"size\":[%d,4]}",
                     (unsigned long)GetCurrentProcessId(), players);
        char activity[1024];
        snprintf(activity, sizeof activity,
                 "{\"details\":\"%s\",\"state\":\"%s\",\"timestamps\":{\"start\":%lld},"
                 "\"assets\":{\"large_image\":\"logo\",\"large_text\":\"DDDA Coop " DDDA_COOP_VERSION "\"}%s}",
                 d2, s2, start, party);
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
