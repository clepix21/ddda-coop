// Bitterblack Isle's (and the field's) random spawns, the same in every player's game (issue #6: "Bitterblack
// Isle's random draws (spawn lottery, respawn chances, stage variants) are decided by the first player in an area").
//
// The set manager's draws seed the game's random generator (xorshift, FUN_00D006C0 seed / FUN_00D00690 step) with
// [sGameSys]+0xB8784 right before drawing: FUN_004A2900 (each of an area's 32 spawn entries is turned on when a draw
// is under its chance, into [sSetManager]+0x22DC), FUN_004A21B0 and FUN_004A2C30. That word is a counter the game
// steps once a second (FUN_0043CD10, from the scene update), kept in the save: both games start from the save's
// value, then the guest's runs 2-3 behind (it loaded later), so each game drew other spawns when entering an area
// (bbi_seed_watch run 2026-11-18: 0x440D on the host at 04:50:51.0, on the guest at 04:50:52.5).
//
// In co-op the three reads of that word are patched (a call to a stub that puts the lottery's seed in the register
// the game reads it into): the seed is made from the area and the host's counter in 5-minute steps, the guest
// adding the gap the host's MSG_LOTTERY tells it. Draws in the same area within the same 5 minutes give the same
// spawns in every game. Solo: the game's own value.
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {

constexpr uintptr_t kSeedOff = 0xB8784;                         // [sGameSys]+0xB8784, both builds
inline uintptr_t game_sys() { return game::at<uintptr_t>(0, game::va(0x018FA4BC)); }

struct Site {
    uintptr_t steam, gog; // the 6-byte MOV reg, [base + 0xB8784]
    uint8_t modrm;        // 8B <modrm> 84 87 0B 00
    const char *where;
};
const Site kSites[3] = {
    {0x004A2483, 0x004A1EB3, 0x90, "FUN_004A21B0"}, // MOV EDX, [EAX + 0xB8784]
    {0x004A2908, 0x004A2338, 0x88, "FUN_004A2900"}, // MOV ECX, [EAX + 0xB8784]
    {0x004A2CD7, 0x004A2707, 0x82, "FUN_004A2C30"}, // MOV EAX, [EDX + 0xB8784]
};

volatile LONG g_gap = 0;          // the host's counter minus this game's (guest)
volatile LONG g_gap_known = 0;
volatile LONG g_draws = 0;

uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352D;
    x ^= x >> 15;
    x *= 0x846CA68B;
    x ^= x >> 16;
    return x;
}

} // namespace

extern "C" {
uint32_t lottery_seed(uint32_t base, uint32_t site) __asm__("lottery_seed");
uint32_t lottery_seed(uint32_t base, uint32_t site)
{
    uint32_t own = game::at<uint32_t>(base, kSeedOff);
    if (net_role() == NET_OFF || !net_shared()) // one world (solo, classic co-op): the game's own value
        return own;
    uint32_t host = own;
    if (net_role() == NET_GUEST)
        host = own + uint32_t(g_gap); // 0 until the host's counter came: then the guest's own steps
    int32_t stage = net_my_stage();
    uint32_t seed = mix(host / 300u * 0x9E3779B9u ^ uint32_t(stage) * 0x85EBCA6Bu ^ site * 0xC2B2AE35u);
    if (seed == 0 || seed == 0xFFFFFFFFu)
        seed = 1;
    LONG n = InterlockedIncrement(&g_draws);
    if (n <= 200)
        logf("lottery: %s draw in stage %d: counter %u (host %u%s) -> seed %08X", kSites[site < 3 ? site : 0].where, int(stage),
             own, host, net_role() == NET_GUEST && !g_gap_known ? ", gap not known yet" : "", seed);
    return seed;
}
}

// pushfd/pushad, lottery_seed(base, site), the result into the register the game's MOV wrote. pushad's layout from
// ESP: EDI, ESI, EBP, ESP, EBX, EDX, ECX, EAX; pushfd above it.
#define LOTTERY_STUB(name, base, site, slot)                                                                              \
    extern "C" __attribute__((naked)) void name() asm(#name);                                                             \
    extern "C" __attribute__((naked)) void name()                                                                         \
    {                                                                                                                     \
        __asm__ volatile(".intel_syntax noprefix\n"                                                                       \
                         "pushfd\n"                                                                                       \
                         "pushad\n"                                                                                       \
                         "push " #site "\n"                                                                               \
                         "push " #base "\n"                                                                               \
                         "call lottery_seed\n"                                                                            \
                         "add esp, 8\n"                                                                                   \
                         "mov dword ptr [esp + " #slot "], eax\n"                                                         \
                         "popad\n"                                                                                        \
                         "popfd\n"                                                                                        \
                         "ret\n"                                                                                          \
                         ".att_syntax\n");                                                                                \
    }
LOTTERY_STUB(lottery_stub0, eax, 0, 20) // EDX <- seed
LOTTERY_STUB(lottery_stub1, eax, 1, 24) // ECX <- seed
LOTTERY_STUB(lottery_stub2, edx, 2, 28) // EAX <- seed

// Each area load (FUN_004A21B0(stage), EAX = the set manager) first draws 64 numbers from the game's global
// generator (0x018D017C; GOG 0x018B115C) without seeding it, into three masks (25 / 50 / 75 %) that pick the
// sets' variants (FUN_004A2D30): that generator serves everything (823 calls), so each game drew other variants.
// In co-op it is seeded at the start of the load with the lottery's seed for that stage.
extern "C" {
void *g_lottery_load_orig __asm__("g_lottery_load_orig") = nullptr;
void lottery_on_load(int32_t stage) __asm__("lottery_on_load");
void lottery_on_load(int32_t stage)
{
    if (net_role() == NET_OFF || !net_shared())
        return;
    uintptr_t gs = game_sys();
    if (!gs)
        return;
    uint32_t seed = lottery_seed(gs, 3);
    using Seed_t = void(__thiscall *)(void *, uint32_t);
    bool gog = game::build() == game::BUILD_GOG;
    auto seed_fn = reinterpret_cast<Seed_t>(gog ? 0x00CFEB10 : 0x00D006C0);
    void *rng = reinterpret_cast<void *>(gog ? 0x018B115C : 0x018D017C);
    seed_fn(rng, seed);
    // its next number (FUN_00D00690 on a copy): the same in every game = the same variants (tests)
    uint32_t st[4];
    memcpy(st, rng, sizeof st);
    uint32_t t = st[0] << 15 ^ st[0], next = (st[3] >> 17 ^ t) >> 4 ^ st[3] ^ t;
    logf("lottery: area %d loads: the game's generator seeded with %08X (next number %08X)", int(stage), seed, next);
}
}
// EAX (the set manager) kept; the stage is the one stack argument
__attribute__((naked)) static void lottery_load_stub()
{
    __asm__ volatile(".intel_syntax noprefix\n"
                     "pushad\n"
                     "push dword ptr [esp + 36]\n"
                     "call lottery_on_load\n"
                     "add esp, 4\n"
                     "popad\n"
                     "jmp dword ptr [g_lottery_load_orig]\n"
                     ".att_syntax\n");
}

void lottery_install()
{
    if (early_flag("test_no_lottery")) {
        logf("lottery: off (test_no_lottery)");
        return;
    }
    void (*stubs[3])() = {lottery_stub0, lottery_stub1, lottery_stub2};
    bool gog = game::build() == game::BUILD_GOG;
    for (const Site &s : kSites) {
        auto at = reinterpret_cast<const uint8_t *>(gog ? s.gog : s.steam);
        const uint8_t want[6] = {0x8B, s.modrm, 0x84, 0x87, 0x0B, 0x00};
        if (!mem_readable(const_cast<uint8_t *>(at), 6) || memcmp(at, want, 6) != 0) {
            logf("lottery: unexpected bytes at %p (%s), nothing patched", (void *)at, s.where);
            return;
        }
    }
    for (int i = 0; i < 3; ++i) {
        auto at = reinterpret_cast<uint8_t *>(gog ? kSites[i].gog : kSites[i].steam);
        DWORD old;
        VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &old);
        int32_t rel = int32_t(reinterpret_cast<uintptr_t>(stubs[i]) - (reinterpret_cast<uintptr_t>(at) + 5));
        at[0] = 0xE8;
        memcpy(at + 1, &rel, 4);
        at[5] = 0x90;
        VirtualProtect(at, 6, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    {
        void *target = reinterpret_cast<void *>(gog ? 0x004A1BE0 : 0x004A21B0);
        const uint8_t want[6] = {0x81, 0xEC, 0xC0, 0x01, 0x00, 0x00}; // SUB ESP, 0x1C0
        MH_STATUS st = MH_UNKNOWN;
        if (mem_readable(target, 6) && memcmp(target, want, 6) == 0) {
            st = MH_CreateHook(target, reinterpret_cast<void *>(&lottery_load_stub), &g_lottery_load_orig);
            if (st == MH_OK)
                st = MH_EnableHook(target);
        }
        logf("lottery: area load hook %p: %s", target, MH_StatusToString(st));
    }
    logf("lottery: 3 seed reads patched (the set manager's draws, the same in every game in co-op)");
}

// The game's own counter now (0 when no game is loaded): the host sends it (MSG_LOTTERY), net thread.
uint32_t lottery_counter()
{
    uintptr_t gs = game_sys();
    uint32_t v = 0;
    if (!gs || !mem_safe_read(gs + kSeedOff, &v, 4))
        return 0;
    return v;
}

// Guest, net thread: the host's counter came.
void lottery_host_counter(uint32_t host)
{
    uint32_t own = lottery_counter();
    if (!own || !host)
        return;
    LONG gap = LONG(host - own);
    LONG was = InterlockedExchange(&g_gap, gap);
    if (!InterlockedExchange(&g_gap_known, 1) || (was != gap && (was - gap > 2 || gap - was > 2)))
        logf("lottery: the host's counter %u, this game's %u: gap %ld", host, own, long(gap));
}
