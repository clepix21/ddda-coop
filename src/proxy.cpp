// dinput8.dll proxy: forwards DirectInput8Create to the system DLL and starts the mod.
#include <windows.h>

#include "mod.h"

using DirectInput8Create_t = HRESULT(WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
static DirectInput8Create_t g_real = nullptr;

static void load_real()
{
    if (g_real)
        return;
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH); // SysWOW64 for a 32-bit process
    lstrcatA(path, "\\dinput8.dll");
    HMODULE h = LoadLibraryA(path);
    if (h)
        g_real = reinterpret_cast<DirectInput8Create_t>(GetProcAddress(h, "DirectInput8Create"));
}

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
    load_real();
    if (!g_real)
        return E_FAIL;
    return g_real(inst, ver, riid, out, outer);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        mod_start(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        mod_stop();
    }
    return TRUE;
}
