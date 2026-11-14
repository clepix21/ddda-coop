// Shared world: the game's catch-up of a pawn left far behind is turned off for the pawns that show
// another player (and the unused ones kept out of sight).
//
// FUN_00B88710 (called by FUN_00B6CFA0 at 0x00B6D4E0, and by FUN_00BBAA60 at 0x00BBAC1E; no stack
// arguments, EDI = the pawn, EAX = where it is put) takes the Arisen's place (+0xE0 of
// [0x018FA4EC]+0x99C), finds a spot near it on the ground (FUN_010B4560, ray FUN_00DEA870) and puts the
// pawn there (vtable +0x210, +0x144 -> FUN_00B83240 -> FUN_004C6970, which writes +0x40, +0xE70, +0xE80).
// A pawn showing a player far from this Arisen was moved next to it on every frame, then put back at its
// player's place by net.cpp: "2400 u off its player's place" every 1.5 s in the three-copy area lead suite
// (2026-11-14; hardware watch on +0x40: 004C6981 <- 00B8329C <- 00B88A4B, then dinput8's write).
#include <windows.h>
#include <cstdint>

#include "MinHook.h"
#include "mod.h"

extern "C" {
void *g_catchup_orig __asm__("g_catchup_orig") = nullptr;
volatile LONG g_catchup_skipped __asm__("g_catchup_skipped") = 0;
bool catchup_skip(uintptr_t pawn) __asm__("catchup_skip");
bool catchup_skip(uintptr_t pawn)
{
    if (!net_puppet_ptr(pawn))
        return false;
    InterlockedIncrement(&g_catchup_skipped);
    return true;
}
}

// EAX and EDI are the function's inputs: kept for the original; a skipped call returns 0 (not moved).
__attribute__((naked)) static void catchup_stub()
{
    __asm__ volatile(".intel_syntax noprefix\n"
                     "pushad\n"
                     "push edi\n"
                     "call catchup_skip\n"
                     "add esp, 4\n"
                     "test al, al\n"
                     "popad\n"
                     "jnz 1f\n"
                     "jmp dword ptr [g_catchup_orig]\n"
                     "1:\n"
                     "xor eax, eax\n"
                     "ret\n"
                     ".att_syntax\n");
}

void catchup_install()
{
    void *target = reinterpret_cast<void *>(0x00B88710);
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&catchup_stub), &g_catchup_orig);
    if (st == MH_OK)
        st = MH_EnableHook(target);
    logf("catch-up: hook %p (no catch-up for the pawns showing players): %s", target, MH_StatusToString(st));
}

long catchup_skipped_take() { return InterlockedExchange(&g_catchup_skipped, 0); }
