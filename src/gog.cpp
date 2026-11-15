// GOG's build only. Tests: no_galaxy=1 keeps the game off GOG Galaxy (the owner's account, its friends
// list and presence), like steam_mock for the Steam build. FUN_00CFE0B0 (called once from WinMain's
// FUN_00407960) creates the Galaxy instance and calls its Init(clientId, secret); it is skipped, so
// GalaxyFactory::GetInstance() stays null. Every Galaxy wrapper of the game ("GA::*") checks that
// pointer first and fails softly; the saves then go through save_guard.cpp's private_storage. WinMain then
// asks for a sign-in (FUN_00CFC970) and waits for its answer (FUN_00CFC8B0 spins until [0x0160A705] is set
// by Galaxy's listener): both skipped too, or the game never gets past its black window.
#include <windows.h>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

namespace {

constexpr uintptr_t kGalaxyInit = 0x00CFE0B0, kSignIn = 0x00CFC970, kSignInWait = 0x00CFC8B0; // GOG build (Ghidra 2026-11-15)
void(__cdecl *g_orig_init)() = nullptr;
void(__cdecl *g_orig_sign_in)() = nullptr;
void(__cdecl *g_orig_wait)() = nullptr;
volatile LONG g_skipped = 0;

void __cdecl hk_galaxy_init() { InterlockedExchange(&g_skipped, 1); }
void __cdecl hk_skip() {}

} // namespace

// Called from mod_start (the loader lock: the game's main thread has not run yet), after MH_Initialize.
void gog_early_install()
{
    if (game::build() != game::BUILD_GOG || !early_flag("no_galaxy"))
        return;
    auto *at = reinterpret_cast<const uint8_t *>(kGalaxyInit);
    if (at[0] != 0x56 || at[1] != 0xFF || at[2] != 0x15) { // push esi; call [CreateInstance]
        logf("gog: Galaxy init not as expected (%02X %02X %02X): Galaxy left on", at[0], at[1], at[2]);
        return;
    }
    struct {
        uintptr_t at;
        void *hook;
        void **orig;
    } hooks[] = {{kGalaxyInit, reinterpret_cast<void *>(&hk_galaxy_init), reinterpret_cast<void **>(&g_orig_init)},
                 {kSignIn, reinterpret_cast<void *>(&hk_skip), reinterpret_cast<void **>(&g_orig_sign_in)},
                 {kSignInWait, reinterpret_cast<void *>(&hk_skip), reinterpret_cast<void **>(&g_orig_wait)}};
    MH_STATUS st = MH_OK;
    for (auto &h : hooks) {
        st = MH_CreateHook(reinterpret_cast<void *>(h.at), h.hook, h.orig);
        if (st == MH_OK)
            st = MH_EnableHook(reinterpret_cast<void *>(h.at));
        if (st != MH_OK)
            break;
    }
    logf("gog: no_galaxy=1: the game's GOG Galaxy init, sign-in and wait skipped (%s)", MH_StatusToString(st));
}

bool gog_galaxy_skipped() { return g_skipped != 0; }
