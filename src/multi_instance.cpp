// Tests only (multi_instance=1 in coop.ini): let a second copy of the game run on the same PC.
// DDDA.exe calls CreateMutexA(name) at start-up (0x00DF3AE7); if GetLastError() is
// ERROR_ALREADY_EXISTS it brings the first window to the front (FindWindowW + ShowWindow) and
// quits. We point the exe's import entry for CreateMutexA (IAT slot 0x0139D254) at a wrapper that
// appends the process id to the mutex name. Only this process is affected.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "MinHook.h"
#include "mod.h"

namespace {

constexpr uintptr_t kIatCreateMutexA = 0x0139D254;
using CreateMutexA_t = HANDLE(WINAPI *)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
CreateMutexA_t g_real = nullptr;

HANDLE WINAPI my_create_mutex(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name)
{
    if (!name)
        return g_real(sa, own, name);
    char unique[MAX_PATH];
    _snprintf(unique, sizeof unique, "%s.coop%lu", name, GetCurrentProcessId());
    unique[sizeof unique - 1] = 0;
    return g_real(sa, own, unique);
}

} // namespace

// Called from DllMain: imports are already bound, WinMain has not run yet.
void multi_instance_install()
{
    auto slot = reinterpret_cast<CreateMutexA_t *>(kIatCreateMutexA);
    CreateMutexA_t expected = reinterpret_cast<CreateMutexA_t>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateMutexA"));
    if (*slot != expected) {
        logf("multi-instance: IAT slot does not hold kernel32!CreateMutexA (%p vs %p), not patched", (void *)*slot,
             (void *)expected);
        return;
    }
    g_real = *slot;
    DWORD old;
    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
    *slot = &my_create_mutex;
    VirtualProtect(slot, sizeof *slot, old, &old);
    logf("multi-instance: CreateMutexA names made unique per process");
}

// Tests only (keep_active=1): a copy in the background keeps running and reading its pads.
// On WM_ACTIVATEAPP(FALSE) the window procedure (0x00DF1550) calls FUN_00DF0ED0(0), which clears
// the active flag ([[0x018D0F50]+0x2566]) and deactivates pad and keyboard input. We turn every
// call into FUN_00DF0ED0(1).
namespace {
// __thiscall (ECX = main system object, one byte argument, ret 4): checked at the call site 0x00DF1933.
using SetActive_t = void(__attribute__((thiscall)) *)(void *, char);
SetActive_t g_set_active = nullptr;
void __attribute__((thiscall)) my_set_active(void *self, char) { g_set_active(self, 1); }
} // namespace

void keep_active_install()
{
    void *target = reinterpret_cast<void *>(0x00DF0ED0);
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&my_set_active), reinterpret_cast<void **>(&g_set_active));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    logf("keep-active: hook %p: %s", target, MH_StatusToString(st));
}

// Tests only (private_appdata=1): the game keeps its config.ini under
// %LOCALAPPDATA%\CAPCOM\DRAGONS DOGMA DARK ARISEN and rewrites it on exit, which would touch the
// owner's settings. SHGetFolderPathW is imported (IAT slot 0x0139D288); for CSIDL_LOCAL_APPDATA
// we answer "<game dir>\coop_appdata" instead.
namespace {
constexpr uintptr_t kIatSHGetFolderPathW = 0x0139D288;
using SHGetFolderPathW_t = HRESULT(WINAPI *)(HWND, int, HANDLE, DWORD, LPWSTR);
SHGetFolderPathW_t g_real_shgfp = nullptr;
HRESULT WINAPI my_shgfp(HWND w, int csidl, HANDLE tok, DWORD flags, LPWSTR out)
{
    if ((csidl & 0xFF) == 0x1C /* CSIDL_LOCAL_APPDATA */ && out) {
        wchar_t dir[MAX_PATH];
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        if (wchar_t *slash = wcsrchr(dir, L'\\'))
            *slash = 0;
        wcscat(dir, L"\\coop_appdata");
        CreateDirectoryW(dir, nullptr);
        wcscpy(out, dir);
        return S_OK;
    }
    return g_real_shgfp(w, csidl, tok, flags, out);
}
} // namespace

void private_appdata_install()
{
    auto slot = reinterpret_cast<SHGetFolderPathW_t *>(kIatSHGetFolderPathW);
    HMODULE shell = GetModuleHandleA("shell32.dll");
    auto expected = shell ? reinterpret_cast<SHGetFolderPathW_t>(GetProcAddress(shell, "SHGetFolderPathW")) : nullptr;
    if (!expected || *slot != expected) {
        logf("private-appdata: IAT slot does not hold shell32!SHGetFolderPathW (%p vs %p), not patched", (void *)*slot,
             (void *)expected);
        return;
    }
    g_real_shgfp = *slot;
    DWORD old;
    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
    *slot = &my_shgfp;
    VirtualProtect(slot, sizeof *slot, old, &old);
    logf("private-appdata: LOCAL_APPDATA -> <game dir>\\coop_appdata");
}

// Tests only (window=x,y,w,h): make the game window borderless and put it on a given screen,
// once it exists (HWND at [[0x018D0F50]+0x12C], the handle the window procedure compares).
bool window_place(int x, int y, int w, int h)
{
    uintptr_t sys = *reinterpret_cast<uintptr_t *>(0x018D0F50);
    HWND hwnd = sys ? *reinterpret_cast<HWND *>(sys + 0x12C) : nullptr;
    if (!hwnd || !IsWindow(hwnd))
        return false;
    SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    logf("window: placed at %d,%d %dx%d", x, y, w, h);
    return true;
}

// Minimal read of coop.ini for options needed before the mod thread starts.
bool early_flag(const char *key)
{
    FILE *f = fopen("coop.ini", "r");
    if (!f)
        return false;
    char line[128];
    size_t n = strlen(key);
    bool on = false;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, n) == 0 && line[n] == '=')
            on = atoi(line + n + 1) != 0;
    fclose(f);
    return on;
}

// Minimal read of a string option from coop.ini ("key=value"; the last one wins). False if absent.
bool cfg_str(const char *key, char *out, size_t cap)
{
    FILE *f = fopen("coop.ini", "r");
    if (!f)
        return false;
    char line[256];
    size_t n = strlen(key);
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, n) != 0 || line[n] != '=')
            continue;
        char *v = line + n + 1;
        v[strcspn(v, "\r\n")] = 0;
        snprintf(out, cap, "%s", v);
        found = true;
    }
    fclose(f);
    return found;
}
