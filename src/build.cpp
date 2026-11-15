// Which DDDA.exe runs (Steam build 2364871 or GOG's), and the mod's addresses for it (game.h, addr_gog.h).
#include <windows.h>

#include <cstdint>

#include "game.h"
#include "mod.h"

namespace game {

Build build()
{
    static Build b = [] {
        auto base = reinterpret_cast<const BYTE *>(GetModuleHandleA(nullptr));
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
        DWORD stamp = nt->FileHeader.TimeDateStamp;
        return stamp == kStampSteam ? BUILD_STEAM : stamp == kStampGog ? BUILD_GOG : BUILD_UNKNOWN;
    }();
    return b;
}

const char *build_name()
{
    switch (build()) {
    case BUILD_STEAM: return "Steam";
    case BUILD_GOG: return "GOG";
    default: return "unknown";
    }
}

uintptr_t va(uintptr_t steam)
{
    if (build() != BUILD_GOG)
        return steam;
    size_t lo = 0, hi = sizeof kGogMap / sizeof kGogMap[0];
    while (lo < hi) { // kGogMap is sorted by the Steam address
        size_t mid = (lo + hi) / 2;
        if (kGogMap[mid].steam < steam)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < sizeof kGogMap / sizeof kGogMap[0] && kGogMap[lo].steam == steam)
        return kGogMap[lo].gog;
    static volatile LONG told = 0; // a missing entry is a bug of the table: said once (the log may not be open yet)
    if (!InterlockedExchange(&told, 1))
        OutputDebugStringA("ddda-coop: an address without a GOG entry\n");
    va_missing(steam);
    return steam;
}

} // namespace game

// Addresses asked for that the GOG table lacks (logged by mod.cpp once the log is open).
static volatile LONG g_missing[16];
static volatile LONG g_missing_n = 0;
void va_missing(uintptr_t steam)
{
    LONG n = InterlockedIncrement(&g_missing_n) - 1;
    if (n < 16)
        g_missing[n] = LONG(steam);
}

int va_missing_take(uintptr_t *out, int max)
{
    int n = int(g_missing_n < 16 ? g_missing_n : 16);
    for (int i = 0; i < n && i < max; ++i)
        out[i] = uintptr_t(g_missing[i]);
    return n;
}
