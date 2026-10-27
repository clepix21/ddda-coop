// Stage loads (milestone 5, tests and logs). FUN_004FBD50 creates the next stage: EAX = request
// {int stage, int point, int flag}, ESI = the area-stack object ([sArea+0x3830] when a stage jumps,
// aGameMain at game start), no stack arguments, plain `ret`. Every stage change goes through it
// (gate jumps from FUN_0050EF80, loading a game from FUN_004FB3E0, FUN_004FB7B0), so a hook logs
// each load with its caller: that tells which kinds of transition use the aStage jump request the
// host watches (net.cpp) and which only the guest's 10 s "stages differ" rule catches.
// Stage 999 is special: the point (< 10) indexes a table at [0x018FA4BC]+0xBE378 (a saved place).
#include <windows.h>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

extern "C" void __cdecl stages_on_load(const int32_t *req, uintptr_t ret) asm("stages_on_load");
extern "C" void *g_stage_load_orig asm("g_stage_load_orig");
void *g_stage_load_orig = nullptr;

// Loads that did not come from an aStage jump request (retry after death, last save, a game loaded
// from the title): the host counts them so the guest reloads too (net.cpp).
constexpr uintptr_t kJumpLoadRet = 0x0050F28A; // FUN_0050EF80's call, the gate / jump path
volatile LONG g_other_loads = 0;
volatile LONG g_last_load_tick = 0, g_last_load_stage = -1; // any load, for the guest

extern "C" void __cdecl stages_on_load(const int32_t *req, uintptr_t ret)
{
    if (!req || req[0] == -1)
        return;
    if (ret != kJumpLoadRet)
        InterlockedIncrement(&g_other_loads);
    InterlockedExchange(&g_last_load_stage, req[0]);
    InterlockedExchange(&g_last_load_tick, LONG(GetTickCount() | 1));
    uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0);
    logf("stage: load %d point %d flag %d (from stage %d point %d; caller %08X)", req[0], req[1] & 0xFF, req[2],
         gm ? game::at<int32_t>(gm, 0x34) : -1, gm ? game::at<int32_t>(gm, 0x44) : -1, unsigned(ret));
}

// EAX = request, ESI = area-stack object; both kept for the original. [esp] = return address.
extern "C" __attribute__((naked)) void stages_load_thunk() asm("stages_load_thunk");
extern "C" __attribute__((naked)) void stages_load_thunk()
{
    asm volatile("pushal\n\t"
                 "pushl 32(%esp)\n\t" // return address (above the 8 saved registers)
                 "pushl %eax\n\t"
                 "call stages_on_load\n\t"
                 "addl $8, %esp\n\t"
                 "popal\n\t"
                 "jmp *g_stage_load_orig\n\t");
}

uint32_t stages_other_loads() { return uint32_t(g_other_loads); }
DWORD stages_last_load(int32_t *stage)
{
    *stage = g_last_load_stage;
    return DWORD(g_last_load_tick);
}

bool stages_install()
{
    void *target = reinterpret_cast<void *>(game::kStageLoad);
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&stages_load_thunk), &g_stage_load_orig);
    if (st == MH_OK)
        st = MH_EnableHook(target);
    logf("stages: hook StageLoad %p: %s", target, MH_StatusToString(st));
    return st == MH_OK;
}
