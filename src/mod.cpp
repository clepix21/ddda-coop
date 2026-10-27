// Mod core: log file, exe identity check, MinHook init, pad-2 polling thread.
// Game hooks are added once the pawn controller is mapped (see NOTES.md, Phase 3).
#include <windows.h>
#include <xinput.h>

#include <cstdarg>
#include <cstdio>

#include "MinHook.h"
#include "mod.h"

static FILE *g_log = nullptr;
static CRITICAL_SECTION g_log_lock;
static volatile LONG g_running = 0;

void logf(const char *fmt, ...)
{
    if (!g_log)
        return;
    EnterCriticalSection(&g_log_lock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_lock);
}

// Only build 2364871's exe is supported: its offsets are what we reverse engineered.
static bool exe_is_supported()
{
    auto base = reinterpret_cast<const BYTE *>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    DWORD stamp = nt->FileHeader.TimeDateStamp;
    logf("exe base %p TimeDateStamp 0x%08lX SizeOfImage 0x%08lX", base, stamp, nt->OptionalHeader.SizeOfImage);
    return stamp == 0x5A314C31; // 2017-12-13 15:50:09 UTC
}

static DWORD WINAPI pad_thread(LPVOID)
{
    possess_load_config();
    if (early_flag("keep_active"))
        keep_active_install();
    input_redirect_install();
    save_guard_install(cfg_block_saves()); // storage hooks always; writes blocked only when asked
    if (cfg_role() == NET_GUEST && !early_flag("test_no_owner_patch")) // tests: show what the patch avoids
        storage_guest_patch();
    if (!cfg_block_saves())
        logf("save-guard: off (saves work normally)");
    probe_install();
    exp_sync_install();
    loot_install();
    enemies_install();
    stages_install();
    talk_install();
    pads_install(possess_p2_pad());
    if (cfg_role() == NET_HOST) // guests 1-3 drive pawns from pad slots 1-3 (the game only uses pads it saw from the start)
        for (unsigned slot = 1; slot < 4; ++slot)
            pads_plug(slot);
    if (cfg_role() != NET_OFF && !early_flag("net_late")) // net_late=1 (tests): bridge "netstart"
        net_start(cfg_role(), cfg_peer(), cfg_port());
    if (cfg_bridge())
        bridge_start();
    hang_watch_start();
    DWORD t0 = GetTickCount();
    bool combo_was_down = false;
    bool auto_done = false; // auto possession fires once; the combo stays in charge afterwards
    int wx, wy, ww, wh;
    bool window_pending = cfg_window(wx, wy, ww, wh);
    DWORD last_packet = 0, last_err = 0xFFFFFFFF;
    while (g_running) {
        save_guard_poll();
        if (window_pending && window_place(wx, wy, ww, wh))
            window_pending = false;
        {
            XINPUT_STATE s = {};
            DWORD err = pads_read_raw(DWORD(possess_p2_pad()), &s);
            if (err != last_err) {
                logf("pad2 %s", err == ERROR_SUCCESS ? "connected" : "not connected");
                last_err = err;
            }
            if (err == ERROR_SUCCESS && s.dwPacketNumber != last_packet) {
                last_packet = s.dwPacketNumber;
                const XINPUT_GAMEPAD &g = s.Gamepad;
                const WORD combo = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
                bool down = (g.wButtons & combo) == combo;
                if (down && !combo_was_down)
                    possess_set(!possess_on());
                combo_was_down = down;
                if (cfg_debug_log())
                    logf("pad2 buttons 0x%04X LT %3u RT %3u L(%6d,%6d) R(%6d,%6d)", g.wButtons, g.bLeftTrigger,
                     g.bRightTrigger, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY);
            }
        }
        if (!auto_done && possess_auto_ms() && GetTickCount() - t0 >= possess_auto_ms()) {
            auto_done = true;
            logf("possess: auto after %lu ms", possess_auto_ms());
            possess_set(true);
        }
        Sleep(16);
    }
    return 0;
}

void mod_start(HINSTANCE)
{
    InitializeCriticalSection(&g_log_lock);
    g_log = fopen("coop.log", "w"); // cwd is the game folder
    logf("ddda-pawn-coop loaded");
    if (!exe_is_supported()) {
        logf("unsupported DDDA.exe, mod disabled");
        return;
    }
    if (early_flag("multi_instance"))
        multi_instance_install();
    if (early_flag("private_appdata"))
        private_appdata_install();
    MH_STATUS st = MH_Initialize();
    logf("MinHook init: %s", MH_StatusToString(st));
    InterlockedExchange(&g_running, 1);
    if (HANDLE t = CreateThread(nullptr, 0, pad_thread, nullptr, 0, nullptr))
        CloseHandle(t);
}

void mod_stop()
{
    InterlockedExchange(&g_running, 0);
    logf("unloading");
    // No thread join or MH_Uninitialize under the loader lock; the process is exiting.
}
