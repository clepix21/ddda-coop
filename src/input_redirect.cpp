// Per-thread input redirection for the command builder.
//
// Characters update on several threads at once, so the shared input singletons must never be
// edited for one character. Instead, every load of those globals inside the command builder
// (FUN_00B51EE0, FUN_00B52880, FUN_00B52A00) and the action helpers it calls (0x0040D160 ..
// 0x0040D980) is patched to `call stub` (+ nop). Each stub loads a per-thread pointer from a
// TLS slot and falls back to the real global when it is null, so threads we do not touch
// behave exactly as before. All reads at these sites are plain field/bit tests (plus one
// byte getter, FUN_00E050C0), so a zero-filled buffer reads as "nothing pressed".
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "game.h"
#include "mod.h"

namespace {

enum { EAX = 0, ECX = 1, EDX = 2, EBX = 3, EBP = 5, ESI = 6, EDI = 7 };
enum Global { G_INPUT = 0, G_KEYBOARD, G_MOUSE, G_CAMERA, NGLOBALS };
// G_CAMERA: holder of the camera the stick builder turns sticks with ([[0x018D0B6C]+0x34]).
const uint32_t kGlobalAddr[NGLOBALS] = {game::kInput, game::kKeyboard, game::kMouse, 0x018D0B6C};

// address, global, register (x86 encoding), length (6 = 8B modrm, 5 = A1 for eax)
struct Site {
    uintptr_t addr;
    uint8_t global;
    uint8_t reg;
    uint8_t len;
};
const Site kSites[] = {
    // sQlocInput loads in the command builder
    {0xB51FFD, G_INPUT, EBX, 6}, {0xB52199, G_INPUT, EBX, 6}, {0xB5223A, G_INPUT, EBX, 6}, {0xB52544, G_INPUT, ESI, 6},
    {0xB52A52, G_INPUT, EBP, 6}, {0xB52AB4, G_INPUT, EBP, 6}, {0xB52ADB, G_INPUT, EBP, 6}, {0xB52B3D, G_INPUT, EDX, 6},
    {0xB52B50, G_INPUT, EAX, 5}, {0xB52BB0, G_INPUT, EAX, 5}, {0xB52C26, G_INPUT, EAX, 5}, {0xB52C50, G_INPUT, EDX, 6},
    {0xB52C75, G_INPUT, EAX, 5}, {0xB52C9E, G_INPUT, EAX, 5}, {0xB52CC3, G_INPUT, EAX, 5}, {0xB52D06, G_INPUT, EAX, 5},
    {0xB52D60, G_INPUT, ECX, 6}, {0xB52DB2, G_INPUT, EDX, 6}, {0xB52DF0, G_INPUT, EAX, 5}, {0xB52E30, G_INPUT, ECX, 6},
    {0xB52E5A, G_INPUT, EDX, 6}, {0xB52E82, G_INPUT, EAX, 5}, {0xB52EA9, G_INPUT, ECX, 6}, {0xB52ED1, G_INPUT, EDX, 6},
    {0xB52EF9, G_INPUT, EAX, 5}, {0xB52F20, G_INPUT, ECX, 6}, {0xB52F48, G_INPUT, EDX, 6}, {0xB52F70, G_INPUT, EAX, 5},
    {0xB52F97, G_INPUT, ECX, 6}, {0xB52FBF, G_INPUT, EDX, 6}, {0xB52FE7, G_INPUT, EAX, 5},
    // keyboard / mouse loads in the command builder
    {0xB52117, G_KEYBOARD, EDX, 6}, {0xB52182, G_MOUSE, EBX, 6}, {0xB521D2, G_KEYBOARD, EDI, 6}, {0xB52204, G_MOUSE, EDX, 6},
    {0xB522A7, G_KEYBOARD, EDI, 6}, {0xB522D9, G_MOUSE, EDX, 6}, {0xB5233D, G_KEYBOARD, EDX, 6}, {0xB5236B, G_MOUSE, ECX, 6},
    {0xB528B5, G_MOUSE, ECX, 6},
    // camera holder load in the stick builder (FUN_00B51EE0)
    {0xB51EF9, G_CAMERA, ECX, 6},
    // keyboard / mouse loads in the action helpers (FUN_0040D200, FUN_0040D820 and neighbours)
    {0x40D22A, G_KEYBOARD, EDX, 6}, {0x40D230, G_MOUSE, EAX, 5}, {0x40D568, G_KEYBOARD, EDX, 6}, {0x40D598, G_MOUSE, ECX, 6},
    {0x40D6BD, G_KEYBOARD, EDX, 6}, {0x40D6EB, G_MOUSE, ECX, 6}, {0x40D7BD, G_KEYBOARD, EDX, 6}, {0x40D7EB, G_MOUSE, ECX, 6},
    {0x40D898, G_KEYBOARD, EDX, 6}, {0x40D89E, G_MOUSE, EBX, 6},
};

DWORD g_tls[NGLOBALS] = {TLS_OUT_OF_INDEXES, TLS_OUT_OF_INDEXES, TLS_OUT_OF_INDEXES, TLS_OUT_OF_INDEXES};
uint8_t *g_stubs = nullptr;
uintptr_t g_stub_for[NGLOBALS][8] = {};
bool g_active = false;
alignas(16) uint8_t g_zero[0x4000] = {}; // "nothing pressed" device for keyboard/mouse

bool site_matches(const Site &s)
{
    auto p = reinterpret_cast<const uint8_t *>(s.addr);
    const uint32_t global = kGlobalAddr[s.global];
    if (s.len == 5)
        return s.reg == EAX && p[0] == 0xA1 && memcmp(p + 1, &global, 4) == 0;
    return p[0] == 0x8B && p[1] == uint8_t(0x05 | (s.reg << 3)) && memcmp(p + 2, &global, 4) == 0;
}

// pushfd; mov r, fs:[teb_slot]; test r, r; jnz +6; mov r, [global]; popfd; ret
uintptr_t emit_stub(uint8_t *&p, uint8_t r, uint32_t teb_off, uint32_t global)
{
    uintptr_t start = reinterpret_cast<uintptr_t>(p);
    *p++ = 0x9C;
    *p++ = 0x64;
    *p++ = 0x8B;
    *p++ = uint8_t(0x05 | (r << 3));
    memcpy(p, &teb_off, 4);
    p += 4;
    *p++ = 0x85;
    *p++ = uint8_t(0xC0 | (r << 3) | r);
    *p++ = 0x75;
    *p++ = 0x06;
    *p++ = 0x8B;
    *p++ = uint8_t(0x05 | (r << 3));
    memcpy(p, &global, 4);
    p += 4;
    *p++ = 0x9D;
    *p++ = 0xC3;
    return start;
}

} // namespace

bool input_redirect_install()
{
    for (const Site &s : kSites)
        if (!site_matches(s)) {
            logf("redirect: unexpected bytes at %08X, nothing patched", unsigned(s.addr));
            return false;
        }
    for (int g = 0; g < NGLOBALS; ++g) {
        g_tls[g] = TlsAlloc();
        if (g_tls[g] == TLS_OUT_OF_INDEXES || g_tls[g] >= 64) {
            logf("redirect: TLS index %lu not usable (need < 64), nothing patched", g_tls[g]);
            return false;
        }
    }
    g_stubs = static_cast<uint8_t *>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_stubs)
        return false;
    uint8_t *p = g_stubs;
    const uint8_t regs[] = {EAX, ECX, EDX, EBX, EBP, ESI, EDI};
    for (int g = 0; g < NGLOBALS; ++g)
        for (uint8_t r : regs)
            g_stub_for[g][r] = emit_stub(p, r, 0xE10 + 4 * g_tls[g], kGlobalAddr[g]); // TEB.TlsSlots, x86
    DWORD old;
    VirtualProtect(g_stubs, 0x1000, PAGE_EXECUTE_READ, &old);
    for (const Site &s : kSites) {
        auto at = reinterpret_cast<uint8_t *>(s.addr);
        VirtualProtect(at, s.len, PAGE_EXECUTE_READWRITE, &old);
        int32_t rel = int32_t(g_stub_for[s.global][s.reg] - (s.addr + 5));
        at[0] = 0xE8;
        memcpy(at + 1, &rel, 4);
        if (s.len == 6)
            at[5] = 0x90;
        VirtualProtect(at, s.len, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_active = true;
    logf("redirect: %u sites patched, TLS slots %lu/%lu/%lu/%lu, stubs at %p", unsigned(sizeof kSites / sizeof kSites[0]),
         g_tls[G_INPUT], g_tls[G_KEYBOARD], g_tls[G_MOUSE], g_tls[G_CAMERA], g_stubs);
    return true;
}

bool input_redirect_active() { return g_active; }

// Point this thread's reads at a private input object (nullptr = the real globals). With
// hide_kbm, keyboard and mouse read as idle for this thread too. camera_holder (0 = real) replaces
// the camera the stick builder uses to turn sticks into world directions.
void input_redirect_set(uintptr_t input, bool hide_kbm, uintptr_t camera_holder)
{
    TlsSetValue(g_tls[G_CAMERA], reinterpret_cast<void *>(camera_holder));
    TlsSetValue(g_tls[G_INPUT], reinterpret_cast<void *>(input));
    TlsSetValue(g_tls[G_KEYBOARD], hide_kbm ? g_zero : nullptr);
    TlsSetValue(g_tls[G_MOUSE], hide_kbm ? g_zero : nullptr);
}
