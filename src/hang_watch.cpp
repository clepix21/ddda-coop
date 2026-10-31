// Hang watch (debug_log=1): when the party has not been updated for 30 s after it was in game, log
// where the game's main thread is: its EIP and every stack value that points into the code of a
// loaded module. A 4-copy test saw the host stop in a gate's fade while its guests loaded the next
// area (2026-10-27 06:25); this is to see where, next time.
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "mod.h"

namespace {

volatile LONG g_main_tid = 0;
volatile LONG g_update_tid = 0; // the characters' updates (not the main thread: look2, 2026-10-31)

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

void dump_thread(DWORD tid, const char *what)
{
    HANDLE t = tid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid) : nullptr;
    if (!t) {
        logf("hang: %s thread not known or not opened (%lu)", what, tid);
        return;
    }
    if (SuspendThread(t) == DWORD(-1)) {
        CloseHandle(t);
        return;
    }
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    char buf[2000];
    int len = 0;
    if (GetThreadContext(t, &c)) {
        uintptr_t base = 0;
        const char *m = module_of(c.Eip, base);
        len += _snprintf(buf + len, sizeof buf - len, "EIP %s+%X ESP %08lX:", m ? m : "?", unsigned(c.Eip - base), c.Esp);
        const uintptr_t *sp = reinterpret_cast<const uintptr_t *>(c.Esp);
        for (int i = 0, found = 0; i < 8192 && found < 60 && mem_readable(sp + i, 4) && len < int(sizeof buf) - 40; ++i)
            if (const char *mm = module_of(sp[i], base)) {
                len += _snprintf(buf + len, sizeof buf - len, " %s+%X", mm, unsigned(sp[i] - base));
                ++found;
            }
        // the EBP chain too: the stack values above include stale ones (look4, 2026-10-31)
        len += _snprintf(buf + len, sizeof buf - len, " | frames:");
        uintptr_t bp = c.Ebp;
        for (int k = 0; k < 40 && bp && mem_readable(reinterpret_cast<void *>(bp), 8) && len < int(sizeof buf) - 40; ++k) {
            uintptr_t ret = reinterpret_cast<const uintptr_t *>(bp)[1], next = reinterpret_cast<const uintptr_t *>(bp)[0];
            if (const char *mm = module_of(ret, base))
                len += _snprintf(buf + len, sizeof buf - len, " %s+%X", mm, unsigned(ret - base));
            if (next <= bp)
                break;
            bp = next;
        }
    }
    ResumeThread(t);
    CloseHandle(t);
    logf("hang: %s thread %lu: %s", what, tid, len ? buf : "(no context)");
}

void dump_main_thread()
{
    dump_thread(DWORD(g_main_tid), "main");
    if (g_update_tid && g_update_tid != g_main_tid)
        dump_thread(DWORD(g_update_tid), "update");
}

// Crashes (debug_log=1): the first access violations in DDDA.exe or the mod, with the faulting place,
// the address read or written and the code addresses on the stack. A guest copy closed in the lift
// suite twice with nothing in its log (2026-10-29); first-chance, so ones the game handles itself show
// too (at most 5 logged).
LONG CALLBACK on_exception(EXCEPTION_POINTERS *e)
{
    static volatile LONG logged = 0;
    if (!e || !e->ExceptionRecord || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || logged >= 5)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t base = 0;
    const char *m = module_of(e->ContextRecord->Eip, base);
    if (!m || (_stricmp(m, "DDDA.exe") != 0 && _stricmp(m, "dinput8.dll") != 0))
        return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&logged);
    char buf[1200];
    const ULONG_PTR *info = e->ExceptionRecord->ExceptionInformation;
    int len = _snprintf(buf, sizeof buf, "EIP %s+%X, %s %08lX, EAX %08lX ECX %08lX EDX %08lX ESI %08lX EDI %08lX, stack:", m,
                        unsigned(e->ContextRecord->Eip - base), info[0] ? "write" : "read", static_cast<unsigned long>(info[1]),
                        e->ContextRecord->Eax, e->ContextRecord->Ecx, e->ContextRecord->Edx, e->ContextRecord->Esi, e->ContextRecord->Edi);
    const uintptr_t *sp = reinterpret_cast<const uintptr_t *>(e->ContextRecord->Esp);
    for (int i = 0, found = 0; i < 2048 && found < 24 && mem_readable(sp + i, 4) && len < int(sizeof buf) - 40; ++i)
        if (const char *mm = module_of(sp[i], base)) {
            len += _snprintf(buf + len, sizeof buf - len, " %s+%X", mm, unsigned(sp[i] - base));
            ++found;
        }
    logf("crash? access violation in thread %lu: %s", GetCurrentThreadId(), buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Archive opens (test_log_arc=1): each .arc the game opens, with the game's frames that asked for it
// (EBP chain), to learn which archive holds a piece of gear and which code loads it (2026-10-31).
using CreateFileA_t = HANDLE(WINAPI *)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileA_t g_cfa_orig = nullptr;
HANDLE WINAPI hk_create_file_a(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    size_t n = name ? strlen(name) : 0;
    if (n > 4 && _stricmp(name + n - 4, ".arc") == 0) {
        char buf[600];
        int len = 0;
        uintptr_t base = 0, bp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        for (int k = 0; k < 16 && bp && mem_readable(reinterpret_cast<void *>(bp), 8) && len < int(sizeof buf) - 40; ++k) {
            uintptr_t ret = reinterpret_cast<const uintptr_t *>(bp)[1], next = reinterpret_cast<const uintptr_t *>(bp)[0];
            if (const char *mm = module_of(ret, base))
                len += _snprintf(buf + len, sizeof buf - len, " %s+%X", mm, unsigned(ret - base));
            if (next <= bp)
                break;
            bp = next;
        }
        logf("arc: open %s (thread %lu):%s", name, GetCurrentThreadId(), len ? buf : "");
    }
    return g_cfa_orig(name, acc, share, sa, disp, flags, tmpl);
}
using CreateFileW_t = HANDLE(WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileW_t g_cfw_orig = nullptr;
HANDLE WINAPI hk_create_file_w(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    size_t n = name ? wcslen(name) : 0;
    if (n > 4 && _wcsicmp(name + n - 4, L".arc") == 0) {
        char narrow[MAX_PATH] = "";
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof narrow, nullptr, nullptr);
        char buf[600];
        int len = 0;
        uintptr_t base = 0, bp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        for (int k = 0; k < 16 && bp && mem_readable(reinterpret_cast<void *>(bp), 8) && len < int(sizeof buf) - 40; ++k) {
            uintptr_t ret = reinterpret_cast<const uintptr_t *>(bp)[1], next = reinterpret_cast<const uintptr_t *>(bp)[0];
            if (const char *mm = module_of(ret, base))
                len += _snprintf(buf + len, sizeof buf - len, " %s+%X", mm, unsigned(ret - base));
            if (next <= bp)
                break;
            bp = next;
        }
        logf("arc: open %s (thread %lu):%s", narrow, GetCurrentThreadId(), len ? buf : "");
    }
    return g_cfw_orig(name, acc, share, sa, disp, flags, tmpl);
}

// The game's error dialogs (debug_log=1): a gear rebuild with a model this game had not loaded froze the
// guest at 0 fps; it was a MessageBoxA (FUN_00DF0C70) behind the game's window (look5, 2026-10-31). Their
// text and caller are logged, and in tests the box is answered at once (OK) instead of waiting for a click.
using MessageBoxA_t = int(WINAPI *)(HWND, LPCSTR, LPCSTR, UINT);
MessageBoxA_t g_msgbox_orig = nullptr;
int WINAPI hk_msgbox(HWND w, LPCSTR text, LPCSTR caption, UINT type)
{
    uintptr_t base = 0;
    uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const char *m = module_of(ret, base);
    logf("hang: the game shows a message box (from %s+%X, type %X): \"%s\": \"%s\"", m ? m : "?", unsigned(ret - base), type,
         caption ? caption : "", text ? text : "");
    return IDOK;
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
void hang_note_update_thread() { g_update_tid = LONG(GetCurrentThreadId()); }

void hang_watch_start()
{
    if (!cfg_debug_log())
        return;
    AddVectoredExceptionHandler(1, on_exception);
    if (void *mb = reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("user32.dll"), "MessageBoxA")))
        if (MH_CreateHook(mb, reinterpret_cast<void *>(&hk_msgbox), reinterpret_cast<void **>(&g_msgbox_orig)) == MH_OK)
            MH_EnableHook(mb);
    if (early_flag("test_log_arc"))
        if (void *cf = reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateFileA")))
            if (MH_CreateHook(cf, reinterpret_cast<void *>(&hk_create_file_a), reinterpret_cast<void **>(&g_cfa_orig)) == MH_OK)
                MH_EnableHook(cf);
    if (early_flag("test_log_arc"))
        if (void *cf = reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateFileW")))
            if (MH_CreateHook(cf, reinterpret_cast<void *>(&hk_create_file_w), reinterpret_cast<void **>(&g_cfw_orig)) == MH_OK)
                MH_EnableHook(cf);
    if (HANDLE t = CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr))
        CloseHandle(t);
}
