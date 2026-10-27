// XInput layer: the game's only source of pad state goes through here.
//  - Test bot: pads scripted in bot.txt replace the real ones (and report as plugged in).
//      bot.txt, one step per line: <start ms> <pad 0-3> <buttons hex> [lx ly rx ry lt rt]
//      A step holds until the next step for the same pad; "#" starts a comment; time 0 is
//      when the DLL loads. Pads with no step yet are passed through untouched.
//  - Player 2's Back and Start are removed from what the game sees, so player 2 cannot open
//    player 1's menus or trigger the possession combo in the game. pads_read_raw() returns
//    the unfiltered state for the mod.
#include <windows.h>
#include <xinput.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MinHook.h"
#include "mod.h"

namespace {

struct Step {
    DWORD t;
    int pad;
    XINPUT_GAMEPAD g;
};
Step g_steps[4096];
int g_nsteps = 0;
bool g_scripted[4] = {};
DWORD g_t0 = 0;
DWORD g_packet[4] = {};
XINPUT_GAMEPAD g_last[4] = {};
volatile LONG g_calls[4] = {};
int g_filtered_pad = -1;

using XInputGetState_t = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
using XInputGetCapabilities_t = DWORD(WINAPI *)(DWORD, DWORD, XINPUT_CAPABILITIES *);
XInputGetState_t g_orig = nullptr;
XInputGetCapabilities_t g_orig_caps = nullptr;
CRITICAL_SECTION g_lock;
XINPUT_GAMEPAD g_override[4] = {};
bool g_plugged[4] = {}; // reported connected with neutral input when nothing else drives the slot
DWORD g_override_until[4] = {};

const Step *current(int pad, DWORD now)
{
    const Step *cur = nullptr;
    for (int i = 0; i < g_nsteps; ++i)
        if (g_steps[i].pad == pad && g_steps[i].t <= now)
            cur = &g_steps[i];
    return cur;
}

bool game_has_focus()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

DWORD read_raw(DWORD idx, XINPUT_STATE *s)
{
    if (idx < 4 && g_override_until[idx] && int(g_override_until[idx] - GetTickCount()) > 0) {
        EnterCriticalSection(&g_lock);
        if (memcmp(&g_last[idx], &g_override[idx], sizeof g_last[idx]) != 0) {
            g_last[idx] = g_override[idx];
            ++g_packet[idx];
        }
        s->dwPacketNumber = g_packet[idx];
        s->Gamepad = g_override[idx];
        LeaveCriticalSection(&g_lock);
        return ERROR_SUCCESS;
    }
    if (idx < 4 && g_nsteps) {
        if (const Step *st = current(int(idx), GetTickCount() - g_t0)) {
            EnterCriticalSection(&g_lock);
            if (memcmp(&g_last[idx], &st->g, sizeof st->g) != 0) {
                g_last[idx] = st->g;
                ++g_packet[idx];
                logf("bot: t=%lu pad%lu -> buttons %04X L(%d,%d) R(%d,%d) (game calls so far %ld, focus %s)",
                     GetTickCount() - g_t0, idx + 1, st->g.wButtons, st->g.sThumbLX, st->g.sThumbLY, st->g.sThumbRX,
                     st->g.sThumbRY, g_calls[idx], game_has_focus() ? "yes" : "NO");
            }
            s->dwPacketNumber = g_packet[idx];
            LeaveCriticalSection(&g_lock);
            s->Gamepad = st->g;
            return ERROR_SUCCESS;
        }
    }
    if (idx < 4 && g_plugged[idx]) {
        s->dwPacketNumber = g_packet[idx];
        memset(&s->Gamepad, 0, sizeof s->Gamepad);
        return ERROR_SUCCESS;
    }
    return g_orig(idx, s);
}

DWORD WINAPI hk_get_state(DWORD idx, XINPUT_STATE *s)
{
    if (idx < 4)
        InterlockedIncrement(&g_calls[idx]);
    DWORD err = read_raw(idx, s);
    if (err == ERROR_SUCCESS && int(idx) == g_filtered_pad)
        s->Gamepad.wButtons &= WORD(~(XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START));
    return err;
}

// Scripted pads must look plugged in, or the game never polls them.
DWORD WINAPI hk_get_caps(DWORD idx, DWORD flags, XINPUT_CAPABILITIES *c)
{
    if (idx < 4 && g_scripted[idx] && c) {
        memset(c, 0, sizeof *c);
        c->Type = XINPUT_DEVTYPE_GAMEPAD;
        c->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        c->Gamepad.wButtons = 0xF3FF;
        c->Gamepad.bLeftTrigger = c->Gamepad.bRightTrigger = 0xFF;
        c->Gamepad.sThumbLX = c->Gamepad.sThumbLY = c->Gamepad.sThumbRX = c->Gamepad.sThumbRY = SHORT(0xFFC0);
        c->Vibration.wLeftMotorSpeed = c->Vibration.wRightMotorSpeed = 0xFF;
        return ERROR_SUCCESS;
    }
    return g_orig_caps(idx, flags, c);
}

void load_script()
{
    FILE *f = fopen("bot.txt", "r");
    if (!f)
        return;
    char line[256];
    while (g_nsteps < 4096 && fgets(line, sizeof line, f)) {
        if (char *c = strchr(line, '#'))
            *c = 0;
        Step s = {};
        unsigned t, pad, buttons;
        int lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
        int n = sscanf(line, "%u %u %x %d %d %d %d %d %d", &t, &pad, &buttons, &lx, &ly, &rx, &ry, &lt, &rt);
        if (n < 3 || pad > 3)
            continue;
        s.t = t;
        s.pad = int(pad);
        s.g.wButtons = WORD(buttons);
        s.g.sThumbLX = SHORT(lx);
        s.g.sThumbLY = SHORT(ly);
        s.g.sThumbRX = SHORT(rx);
        s.g.sThumbRY = SHORT(ry);
        s.g.bLeftTrigger = BYTE(lt);
        s.g.bRightTrigger = BYTE(rt);
        g_steps[g_nsteps++] = s;
        g_scripted[s.pad] = true;
    }
    fclose(f);
}

} // namespace

bool pads_install(int filtered_pad)
{
    InitializeCriticalSection(&g_lock);
    g_t0 = GetTickCount();
    g_filtered_pad = filtered_pad;
    load_script();
    HMODULE xi = LoadLibraryA("xinput1_3.dll");
    void *state = xi ? reinterpret_cast<void *>(GetProcAddress(xi, "XInputGetState")) : nullptr;
    void *caps = xi ? reinterpret_cast<void *>(GetProcAddress(xi, "XInputGetCapabilities")) : nullptr;
    if (!state || !caps) {
        logf("pads: xinput1_3 exports not found");
        return false;
    }
    MH_STATUS st = MH_CreateHook(state, reinterpret_cast<void *>(&hk_get_state), reinterpret_cast<void **>(&g_orig));
    if (st == MH_OK)
        st = MH_EnableHook(state);
    MH_STATUS st2 = MH_CreateHook(caps, reinterpret_cast<void *>(&hk_get_caps), reinterpret_cast<void **>(&g_orig_caps));
    if (st2 == MH_OK)
        st2 = MH_EnableHook(caps);
    logf("pads: bot steps %d, Back/Start filtered on pad slot %d, hooks %s / %s", g_nsteps, filtered_pad,
         MH_StatusToString(st), MH_StatusToString(st2));
    return st == MH_OK && st2 == MH_OK;
}

// Test bridge: force a pad state until the given tick (0 = clear). Takes priority over bot.txt.
void pads_override(unsigned slot, XINPUT_GAMEPAD g, DWORD until)
{
    if (slot >= 4)
        return;
    EnterCriticalSection(&g_lock);
    g_override[slot] = g;
    g_override_until[slot] = until;
    g_scripted[slot] = true;
    LeaveCriticalSection(&g_lock);
}

// Net host: player 2's slot must look plugged in from the start, or the game never reads it.
void pads_plug(unsigned slot)
{
    if (slot < 4)
        g_plugged[slot] = g_scripted[slot] = true;
}

// Unfiltered state (bot script or real pad), for the mod's own use.
DWORD pads_read_raw(DWORD idx, XINPUT_STATE *s) { return g_orig ? read_raw(idx, s) : ERROR_DEVICE_NOT_CONNECTED; }
