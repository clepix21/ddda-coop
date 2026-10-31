// Reverse-engineering aid (tests only): hardware write watchpoints inside the game process.
// watch_start(addr, count) puts a 4-byte write breakpoint (DR0) on every thread of the process;
// a vectored exception handler logs the instruction address after each write (EIP points just
// past the writing instruction) and the new value, up to `count` hits, then clears DR7 on the
// thread that hit. Writes that leave the value unchanged are not counted (nor, with down_only,
// writes that raise it). Used from the bridge: "watch arisen <offset> <count>", "watchp ...".
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>

#include "mod.h"

namespace {

volatile LONG g_hits_left = 0;
uintptr_t g_addr = 0;
volatile LONG g_last = 0; // last value seen at g_addr: writes of the same value are not counted
bool g_down_only = false;  // count only writes that lower the value (as a float): damage, not regeneration
uintptr_t g_scan = 0; // character that owns g_addr: look in it for pointers to the writer's ESI object
uintptr_t g_scanned_esi[8] = {};

// Which character fields point at (or just before) the object the writing code works on?
void scan_owner(uintptr_t esi)
{
    for (uintptr_t &s : g_scanned_esi) {
        if (s == esi)
            return;
        if (!s) {
            s = esi;
            break;
        }
    }
    int found = 0;
    for (uintptr_t off = 0; off < 0x5A10 && found < 6; off += 4) {
        uintptr_t v = *reinterpret_cast<uintptr_t *>(g_scan + off);
        if (v <= esi && esi - v < 0x1000) {
            logf("watch:   ESI %08X = [character+%X] + %X", unsigned(esi), unsigned(off), unsigned(esi - v));
            ++found;
        }
    }
    if (esi >= g_scan && esi - g_scan < 0x5A10)
        logf("watch:   ESI %08X = character + %X", unsigned(esi), unsigned(esi - g_scan));
}
PVOID g_veh = nullptr;

LONG CALLBACK on_exception(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = e->ContextRecord;
    if (!(c->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    LONG now = *reinterpret_cast<volatile LONG *>(g_addr);
    LONG before = InterlockedExchange(&g_last, now);
    float fnow, fbefore;
    memcpy(&fnow, &now, 4);
    memcpy(&fbefore, &before, 4);
    if ((before == now || (g_down_only && !(fnow < fbefore))) && g_hits_left > 0)
        return EXCEPTION_CONTINUE_EXECUTION;
    if (InterlockedDecrement(&g_hits_left) >= 0) {
        float f;
        memcpy(&f, reinterpret_cast<void *>(g_addr), 4);
        logf("watch: write to %08X by code before %08X (thread %lu) new value %.4f / %08X, ESI %08X EBX %08X",
             unsigned(g_addr), unsigned(c->Eip), GetCurrentThreadId(), f, *reinterpret_cast<unsigned *>(g_addr),
             unsigned(c->Esi), unsigned(c->Ebx));
        if (g_scan)
            scan_owner(c->Esi);
        // likely return addresses on the stack (values inside DDDA.exe's code section)
        char buf[256];
        int len = 0;
        const uintptr_t *sp = reinterpret_cast<const uintptr_t *>(c->Esp);
        int words = 96;
        while (words > 0 && !mem_readable(sp, words * 4))
            words -= 8;
        for (int i = 0, found = 0; i < words && found < 10; ++i)
            if (sp[i] >= 0x00401000 && sp[i] < 0x01300000) {
                len += _snprintf(buf + len, sizeof buf - len, " %08X", unsigned(sp[i]));
                ++found;
            }
        logf("watch:   stack%s", len ? buf : " (none)");
        // and the EBP chain: the values above include stale ones (2026-10-31)
        char fr[400];
        int fl = 0;
        uintptr_t bp = c->Ebp;
        for (int k = 0; k < 24 && bp && mem_readable(reinterpret_cast<void *>(bp), 8) && fl < int(sizeof fr) - 12; ++k) {
            uintptr_t ret = reinterpret_cast<const uintptr_t *>(bp)[1], next = reinterpret_cast<const uintptr_t *>(bp)[0];
            if (ret >= 0x00401000 && ret < 0x01300000)
                fl += _snprintf(fr + fl, sizeof fr - fl, " %08X", unsigned(ret));
            if (next <= bp)
                break;
            bp = next;
        }
        logf("watch:   frames%s", fl ? fr : " (none)");
    } else {
        c->Dr7 &= ~uintptr_t(0x000F0003); // disable DR0 on this thread
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void set_dr0(HANDLE th, uintptr_t addr, bool on)
{
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &c))
        return;
    c.Dr0 = on ? addr : 0;
    c.Dr7 = on ? ((c.Dr7 & ~uintptr_t(0x000F0003)) | 0x1 | (0x1 << 16) | (0x3 << 18)) // L0, write, 4 bytes
               : (c.Dr7 & ~uintptr_t(0x000F0003));
    SetThreadContext(th, &c);
}

} // namespace

void watch_start(uintptr_t addr, long count, uintptr_t owner, bool down_only)
{
    g_down_only = down_only;
    g_scan = owner;
    memset(g_scanned_esi, 0, sizeof g_scanned_esi);
    if (!g_veh)
        g_veh = AddVectoredExceptionHandler(1, on_exception);
    g_addr = addr;
    InterlockedExchange(&g_last, *reinterpret_cast<volatile LONG *>(addr));
    InterlockedExchange(&g_hits_left, count);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof te};
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
            continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th)
            continue;
        SuspendThread(th);
        set_dr0(th, addr, true);
        ResumeThread(th);
        CloseHandle(th);
        ++n;
    }
    CloseHandle(snap);
    logf("watch: write breakpoint on %08X armed on %d threads, %ld hits", unsigned(addr), n, count);
}
