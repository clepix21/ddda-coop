// Hang watch (debug_log=1): when the party has not been updated for 30 s after it was in game, log
// where the game's main thread is: its EIP and every stack value that points into the code of a
// loaded module. A 4-copy test saw the host stop in a gate's fade while its guests loaded the next
// area (2026-10-27 06:25); this is to see where, next time.
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "mod.h"

namespace {

volatile LONG g_main_tid = 0;

const char *module_of(uintptr_t a, uintptr_t &base)
{
    static char name[MAX_PATH];
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<void *>(a), &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE ||
        !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE)))
        return nullptr;
    base = uintptr_t(mbi.AllocationBase);
    strcpy(name, "?");
    GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), name, sizeof name);
    const char *slash = strrchr(name, '\\');
    return slash ? slash + 1 : name;
}

void dump_main_thread()
{
    DWORD tid = DWORD(g_main_tid);
    HANDLE t = tid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid) : nullptr;
    if (!t) {
        logf("hang: main thread not known or not opened (%lu)", tid);
        return;
    }
    if (SuspendThread(t) == DWORD(-1)) {
        CloseHandle(t);
        return;
    }
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    char buf[1200];
    int len = 0;
    if (GetThreadContext(t, &c)) {
        uintptr_t base = 0;
        const char *m = module_of(c.Eip, base);
        len += _snprintf(buf + len, sizeof buf - len, "EIP %s+%X ESP %08lX:", m ? m : "?", unsigned(c.Eip - base), c.Esp);
        const uintptr_t *sp = reinterpret_cast<const uintptr_t *>(c.Esp);
        for (int i = 0, found = 0; i < 4096 && found < 40 && mem_readable(sp + i, 4) && len < int(sizeof buf) - 40; ++i)
            if (const char *mm = module_of(sp[i], base)) {
                len += _snprintf(buf + len, sizeof buf - len, " %s+%X", mm, unsigned(sp[i] - base));
                ++found;
            }
    }
    ResumeThread(t);
    CloseHandle(t);
    logf("hang: main thread %lu: %s", tid, len ? buf : "(no context)");
}

DWORD WINAPI watch_thread(LPVOID)
{
    DWORD dumped_at = 0;
    bool was_in_game = false;
    for (;;) {
        Sleep(5000);
        DWORD last = possess_last_update();
        DWORD now = GetTickCount();
        if (last && now - last < 5000) {
            was_in_game = true;
            dumped_at = 0;
            continue;
        }
        // stalled 30 s after being in game; one dump, then another every 60 s while it lasts
        if (was_in_game && last && now - last > 30000 && (!dumped_at || now - dumped_at > 60000)) {
            logf("hang: no party update for %lu s", (now - last) / 1000);
            dump_main_thread();
            dumped_at = now | 1;
        }
    }
}

} // namespace

void hang_note_main_thread() { InterlockedExchange(&g_main_tid, LONG(GetCurrentThreadId())); }

void hang_watch_start()
{
    if (!cfg_debug_log())
        return;
    if (HANDLE t = CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr))
        CloseHandle(t);
}
