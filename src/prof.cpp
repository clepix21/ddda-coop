// Frame cost (debug_log=1): the time the mod's own code takes inside the game's frame, by part, every
// 10 s: the friends' evening said "ça ramait" (2026-10-28) with a healthy network in every log.
// Parts: character updates before and after the game's own (possess, the pawns showing players),
// transform finalizes (enemies, players' places), the badge, logging (the line itself and its lock),
// actions (the action changers' checks). Frames: the badge's Present hook (0 if it is off).
#include <windows.h>

#include "mod.h"

namespace {

const char *const kNames[PERF_N] = {"char before", "char after", "finalize", "badge", "log", "actions", "(of which enemies", "players' places", "waiting on the enemy locks", "enemy net sync", "enemies_hp)"};
volatile LONG64 g_ticks[PERF_N] = {};
volatile LONG g_calls[PERF_N] = {};
volatile LONG g_frames = 0;
volatile LONG g_skips = 0;
volatile LONG64 g_worst_frame = 0; // the most mod time in one frame
volatile LONG64 g_this_frame = 0;
double g_ms_per_tick = 0;

DWORD WINAPI report_thread(LPVOID)
{
    for (;;) {
        Sleep(10000);
        LONG frames = InterlockedExchange(&g_frames, 0);
        LONG64 worst = InterlockedExchange64(&g_worst_frame, 0);
        char buf[900];
        int len = _snprintf(buf, sizeof buf, "perf: 10 s, %ld frames (%.0f fps):", frames, frames / 10.0);
        double total = 0;
        for (int k = 0; k < PERF_N; ++k) {
            LONG64 t = InterlockedExchange64(&g_ticks[k], 0);
            LONG c = InterlockedExchange(&g_calls[k], 0);
            double ms = t * g_ms_per_tick;
            if (k != PERF_LOG && k != PERF_ENEMY && k != PERF_PLACE && k != PERF_WAIT && k != PERF_NETEN && k != PERF_HP) // inside the other parts
                total += ms;
            len += _snprintf(buf + len, sizeof buf - len, " %s %.2f ms/frame (%ld calls);", kNames[k], frames ? ms / frames : ms, c);
        }
        _snprintf(buf + len, sizeof buf - len, " mod total %.2f ms/frame, worst frame %.2f ms; enemy syncs skipped (lock busy) %ld",
                  frames ? total / frames : total, worst * g_ms_per_tick, InterlockedExchange(&g_skips, 0));
        logf("%s", buf);
    }
}

} // namespace

int64_t perf_now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void perf_add(int part, int64_t since)
{
    LONG64 d = perf_now() - since;
    InterlockedExchangeAdd64(&g_ticks[part], d);
    InterlockedIncrement(&g_calls[part]);
    if (part != PERF_LOG && part != PERF_ENEMY && part != PERF_PLACE && part != PERF_WAIT && part != PERF_NETEN && part != PERF_HP) // parts of parts count once
        InterlockedExchangeAdd64(&g_this_frame, d);
}

void perf_skip() { InterlockedIncrement(&g_skips); }

void perf_frame()
{
    InterlockedIncrement(&g_frames);
    LONG64 f = InterlockedExchange64(&g_this_frame, 0);
    if (f > g_worst_frame)
        InterlockedExchange64(&g_worst_frame, f);
}

void perf_start()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_ms_per_tick = 1000.0 / double(f.QuadPart);
    if (!cfg_debug_log())
        return;
    if (HANDLE t = CreateThread(nullptr, 0, report_thread, nullptr, 0, nullptr))
        CloseHandle(t);
}
