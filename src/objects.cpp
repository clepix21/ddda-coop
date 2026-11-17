// Objects moved by a player (issue #4: "desynced boxes and materials"; carried crates and barrels, thrown or
// pushed things): each game sends the places of the objects it moves itself, and the others move their copy
// there. An object is known on every PC by its class and its place when its area loaded (layout data, the same
// everywhere). The game that moves an object owns it while it moves and 2 s after; meanwhile the others
// leave theirs to it.
//
// The objects are the game's units of class uOm* (sUnit's move lines, enemies.cpp units_each: 81 of them at
// Cassardis, all in one line), read with ReadProcessMemory (memory freed meanwhile fails instead of crashing).

#include <windows.h>

#include <cmath>
#include <cstring>

#include "game.h"
#include "mod.h"



namespace {

struct Track {
    uintptr_t ptr, vt;
    uint32_t cls;
    float home[3];    // where it was when first seen (its area's load), in world places
    float last[3];    // its place at the last scan, in this game's places
    float applied[3]; // the last place another player's game put it at, in this game's places
    bool has_applied;
    DWORD moved;      // last time it moved here on its own (a player here moved it): 0 = never
    DWORD seen;       // last scan it was in
};
constexpr int kTracks = 768;
Track g_tracks[kTracks];
int g_ntracks = 0;

struct Target {
    ObjState s;
    DWORD at;
};
constexpr int kTargets = 64;
Target g_targets[kTargets];

CRITICAL_SECTION g_lock;
struct LockInit {
    LockInit() { InitializeCriticalSection(&g_lock); }
} g_lock_init;

struct VtClass {
    uintptr_t vt;
    uint32_t hash; // 0: not an object
};
VtClass g_vt_cache[256];
int g_nvt = 0;

uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ uint8_t(*s)) * 16777619u;
    return h;
}

// the class hash of an object unit's vtable, 0 when it is not an object (uOm*)
uint32_t obj_class(uintptr_t unit, uintptr_t vt)
{
    for (int k = 0; k < g_nvt; ++k)
        if (g_vt_cache[k].vt == vt)
            return g_vt_cache[k].hash;
    const char *c = enemies_strict_class_of(unit);
    uint32_t h = c && strncmp(c, "uOm", 3) == 0 ? fnv(c) : 0;
    if (c && g_nvt < 256)
        g_vt_cache[g_nvt++] = {vt, h};
    return h;
}

float dist2(const float *a, const float *b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

// Net thread, ~10 times a second: scans the objects, and gives those this game moved in the last 2 s (their
// place in world places). Returns how many.
int objects_collect(ObjState *out, int cap)
{
    DWORD now = GetTickCount();
    int n = 0;
    static int32_t stage = -1;
    int32_t here = net_my_stage();
    // the open world (stage 100): its places count from the cell this Arisen stands in (issue #4, enemies.cpp
    // field_origin); objects are known by their home in world places, so none is tracked before the origin
    // is known (a game that found it later had homes 40000 u off the other's: objects suite 2026-11-17), and
    // all start again when it changes (every place here moved by 10000 u: not a move)
    float ox = 0.f, oz = 0.f;
    bool known = field_origin(ox, oz);
    static float last_ox = 0.f, last_oz = 0.f;
    EnterCriticalSection(&g_lock);
    if (here != stage || ox != last_ox || oz != last_oz) { // another area or cell: every object is new
        stage = here;
        last_ox = ox;
        last_oz = oz;
        g_ntracks = 0;
    }
    if (here == 100 && !known) {
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    // only what this game's players move: objects that move on their own (swinging signs, doors, water)
    // move in every game alike; an object counts as moved here when it moves within 1500 u of this Arisen
    float me[3] = {1e9f, 1e9f, 1e9f};
    if (uintptr_t a = possess_arisen())
        mem_safe_read(a + 0x40, me, sizeof me);
    units_each_unit([&](uintptr_t u) {
        uint8_t head[0x60];
        if (!mem_safe_read(u, head, sizeof head))
            return true;
        uintptr_t vt;
        memcpy(&vt, head, 4);
        uint32_t cls = obj_class(u, vt);
        if (!cls)
            return true;
        float pos[3], rot[4];
        memcpy(pos, head + 0x40, 12);
        memcpy(rot, head + 0x50, 16);
        if (pos[0] != pos[0] || std::fabs(pos[0]) > 1e6f)
            return true;
        Track *t = nullptr;
        for (int k = 0; k < g_ntracks && !t; ++k)
            if (g_tracks[k].ptr == u && g_tracks[k].vt == vt)
                t = &g_tracks[k];
        if (!t) {
            if (g_ntracks >= kTracks)
                return true;
            t = &g_tracks[g_ntracks++];
            *t = {};
            t->ptr = u;
            t->vt = vt;
            t->cls = cls;
            memcpy(t->home, pos, 12);
            field_to_world(t->home);
            memcpy(t->last, pos, 12);
        }
        t->seen = now;
        // moved here on its own: not where the last scan saw it, and not where another game put it
        if (dist2(pos, t->last) > 2.f * 2.f && (!t->has_applied || dist2(pos, t->applied) > 5.f * 5.f) &&
            dist2(pos, me) < 1500.f * 1500.f)
            t->moved = now | 1;
        memcpy(t->last, pos, 12);
        if (t->moved && now - t->moved < 2000 && n < cap) {
            ObjState &s = out[n++];
            s.cls = cls;
            memcpy(s.home, t->home, 12);
            memcpy(s.pos, pos, 12);
            field_to_world(s.pos);
            memcpy(s.rot, rot, 16);
        }
        return true;
    });
    // objects gone (broken, picked up, unloaded): out of the table
    int w = 0;
    for (int k = 0; k < g_ntracks; ++k)
        if (now - g_tracks[k].seen < 1000)
            g_tracks[w++] = g_tracks[k];
    g_ntracks = w;
    static DWORD next_log = 0;
    static int sent = 0;
    sent += n;
    if (!next_log || int(now - next_log) >= 0) {
        next_log = (now + 10000) | 1;
        logf("objects: %d tracked in stage %d, %d states sent in 10 s", g_ntracks, int(stage), sent);
        sent = 0;
    }
    LeaveCriticalSection(&g_lock);
    return n;
}

// Net thread: objects another player's game moves (world places).
void objects_receive(const ObjState *in, int count)
{
    DWORD now = GetTickCount();
    static DWORD next_log = 0;
    if (!next_log || int(now - next_log) >= 0) {
        next_log = (now + 2000) | 1;
        logf("objects: %d moved by another player received (first: class %08X home (%.0f, %.0f, %.0f))", count,
             count ? in[0].cls : 0, count ? in[0].home[0] : 0.f, count ? in[0].home[1] : 0.f, count ? in[0].home[2] : 0.f);
    }
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < count; ++i) {
        int slot = -1, oldest = 0;
        for (int k = 0; k < kTargets && slot < 0; ++k)
            if (g_targets[k].at && g_targets[k].s.cls == in[i].cls && dist2(g_targets[k].s.home, in[i].home) < 1.f)
                slot = k;
        for (int k = 0; k < kTargets && slot < 0; ++k)
            if (!g_targets[k].at || now - g_targets[k].at > 3000)
                slot = k;
        if (slot < 0) {
            for (int k = 1; k < kTargets; ++k)
                if (int(g_targets[k].at - g_targets[oldest].at) < 0)
                    oldest = k;
            slot = oldest;
        }
        g_targets[slot] = {in[i], now | 1};
    }
    LeaveCriticalSection(&g_lock);
}

// Game thread, each frame: the objects other games move, put where they are there (unless a player here
// moves the same object now: it is this game's while it moves).
void objects_game_tick()
{
    DWORD now = GetTickCount();
    static DWORD next_log = 0;
    EnterCriticalSection(&g_lock);
    for (Target &tg : g_targets) {
        if (!tg.at || now - tg.at > 500)
            continue;
        Track *t = nullptr;
        for (int k = 0; k < g_ntracks && !t; ++k)
            if (g_tracks[k].cls == tg.s.cls && dist2(g_tracks[k].home, tg.s.home) < 2.f * 2.f)
                t = &g_tracks[k];
        if (!t || (t->moved && now - t->moved < 1000))
            continue;
        uintptr_t vt = 0;
        if (!mem_safe_read(t->ptr, &vt, 4) || vt != t->vt) // freed meanwhile
            continue;
        float pos[3];
        memcpy(pos, tg.s.pos, 12);
        field_to_local(pos);
        float *p = reinterpret_cast<float *>(t->ptr + 0x40);
        if (dist2(p, pos) < 0.5f * 0.5f)
            continue;
        if (!next_log || int(now - next_log) >= 0) {
            next_log = (now + 1000) | 1;
            logf("objects: %08X moved by another player: from (%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f)", unsigned(t->ptr),
                 p[0], p[1], p[2], pos[0], pos[1], pos[2]);
        }
        memcpy(p, pos, 12);
        memcpy(reinterpret_cast<void *>(t->ptr + 0x50), tg.s.rot, 16);
        memcpy(t->applied, pos, 12);
        t->has_applied = true;
        memcpy(t->last, pos, 12);
    }
    LeaveCriticalSection(&g_lock);
}

// Bridge (tests): "objpush <dx> <dz>" moves the object nearest this Arisen (as a player would), "objnear"
// logs the objects within 1500 u (class hash, home, place): the other game's log shows whether it followed.
void objects_debug(const char *what, float dx, float dz)
{
    uintptr_t a = possess_arisen();
    if (!a)
        return;
    float me[3];
    memcpy(me, reinterpret_cast<void *>(a + 0x40), 12);
    EnterCriticalSection(&g_lock);
    Track *best = nullptr;
    float bd = 1e30f;
    for (int k = 0; k < g_ntracks; ++k) {
        float d = dist2(g_tracks[k].last, me);
        if (strcmp(what, "near") == 0 && d < 1500.f * 1500.f)
            logf("objects: near %08X class %08X home (%.0f, %.0f, %.0f) at (%.1f, %.1f, %.1f)", unsigned(g_tracks[k].ptr),
                 g_tracks[k].cls, g_tracks[k].home[0], g_tracks[k].home[1], g_tracks[k].home[2], g_tracks[k].last[0],
                 g_tracks[k].last[1], g_tracks[k].last[2]);
        if (d < bd) {
            bd = d;
            best = &g_tracks[k];
        }
    }
    if (strcmp(what, "push") == 0 && best) {
        uintptr_t vt = 0;
        if (mem_safe_read(best->ptr, &vt, 4) && vt == best->vt) {
            float *p = reinterpret_cast<float *>(best->ptr + 0x40);
            logf("objects: push %08X class %08X home (%.0f, %.0f, %.0f) from (%.1f, %.1f, %.1f) by (%.0f, %.0f)",
                 unsigned(best->ptr), best->cls, best->home[0], best->home[1], best->home[2], p[0], p[1], p[2], dx, dz);
            p[0] += dx;
            p[2] += dz;
        }
    }
    LeaveCriticalSection(&g_lock);
}
