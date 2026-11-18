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
#include <cstdlib>
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
    // A real carry (RT on a jar or a barrel; carry test 2026-11-18): the Arisen's +0x38F0 is the object (kind
    // +0x3938 = 2), whose +0x40 reads (0, 0, 0) while carried (relative to the holder), so the scan above never
    // sees it move. It is sent above this Arisen's head instead, and the others' copies follow. Thrown, a jar
    // broke: its memory took another vtable (01608BB8 -> 01558134) at once; then its copies are sent 30000 u
    // under where it was (out of sight in the others' games: no way known to break theirs). A thrown object
    // that stays whole moves on its own afterwards, and the scan sends it as any moved object.
    static struct {
        bool active;
        uintptr_t ptr, vt;
        uint32_t cls;
        float home[3], at[3];
        DWORD released;
    } carry = {};
    uintptr_t a = possess_arisen();
    uintptr_t held = 0;
    int32_t kind = 0;
    if (a && mem_safe_read(a + 0x38F0, &held, 4) && mem_safe_read(a + 0x3938, &kind, 4) && held && kind == 2) {
        Track *t = nullptr;
        for (int k = 0; k < g_ntracks && !t; ++k)
            if (g_tracks[k].ptr == held)
                t = &g_tracks[k];
        if (t) {
            if (!carry.active || carry.ptr != held)
                logf("objects: this Arisen carries %08X (class %08X home (%.0f, %.0f, %.0f))", unsigned(held), t->cls, t->home[0],
                     t->home[1], t->home[2]);
            carry.active = true;
            carry.ptr = held;
            carry.vt = t->vt;
            carry.cls = t->cls;
            memcpy(carry.home, t->home, 12);
            float at[3] = {me[0], me[1] + 150.f, me[2]};
            field_to_world(at);
            memcpy(carry.at, at, 12);
            t->seen = now; // kept while carried
            if (n < cap) {
                ObjState &s = out[n++];
                s.cls = carry.cls;
                memcpy(s.home, carry.home, 12);
                memcpy(s.pos, at, 12);
                float rot[4] = {0.f, 0.f, 0.f, 1.f};
                mem_safe_read(held + 0x50, rot, 16);
                memcpy(s.rot, rot, 16);
            }
        }
    } else if (carry.active) {
        carry.active = false;
        carry.released = now | 1;
    }
    if (carry.released && now - carry.released < 1500) {
        uintptr_t vt = 0;
        if ((!mem_safe_read(carry.ptr, &vt, 4) || vt != carry.vt) && n < cap) { // broken (or freed)
            static DWORD told = 0;
            if (told != carry.released) {
                told = carry.released;
                logf("objects: the object this Arisen carried (class %08X) is gone: its copies are put out of sight", carry.cls);
            }
            ObjState &s = out[n++];
            s.cls = carry.cls;
            memcpy(s.home, carry.home, 12);
            float under[3] = {carry.at[0], carry.at[1] - 30000.f, carry.at[2]};
            memcpy(s.pos, under, 12);
            float rot[4] = {0.f, 0.f, 0.f, 1.f};
            memcpy(s.rot, rot, 16);
        }
    } else if (carry.released) {
        carry.released = 0;
    }
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
    // "lift": what the Arisen holds (+0x38F0, kind +0x3938): its vtable, place, and whether objects.cpp tracks it
    // (a real carry, tests: the lifted object left the tracked list, 2026-11-18 carry run)
    static uintptr_t s_lifted = 0;
    if (strcmp(what, "lift") == 0) {
        uintptr_t h = game::at<uintptr_t>(a, 0x38F0);
        if (h)
            s_lifted = h;
        uintptr_t o = h ? h : s_lifted, vt = 0;
        float p[3] = {};
        bool ok = o && mem_safe_read(o, &vt, 4) && mem_safe_read(o + 0x40, p, 12);
        const char *tracked = "not tracked";
        for (int k = 0; k < g_ntracks; ++k) // g_lock is held here (taken above)
            if (g_tracks[k].ptr == o)
                tracked = "tracked";
        LeaveCriticalSection(&g_lock);
        logf("objects: lift: holds %08X (kind %d), last held %08X: vt %08X%s at (%.1f, %.1f, %.1f), %s; Arisen at (%.1f, %.1f, %.1f)",
             unsigned(h), game::at<int32_t>(a, 0x3938), unsigned(s_lifted), unsigned(vt), ok ? "" : " (unreadable)", p[0], p[1], p[2],
             tracked, me[0], me[1], me[2]);
        return;
    }
    uint32_t want_cls = 0; // "go:<class hex>": the nearest object of that class
    if (strncmp(what, "go:", 3) == 0) {
        want_cls = uint32_t(strtoul(what + 3, nullptr, 16));
        what = "go";
    }
    for (int k = 0; k < g_ntracks; ++k) {
        float d = dist2(g_tracks[k].last, me);
        if (want_cls && g_tracks[k].cls != want_cls)
            d = 1e30f;
        if (strcmp(what, "near") == 0 && d < 1500.f * 1500.f)
            logf("objects: near %08X class %08X home (%.0f, %.0f, %.0f) at (%.1f, %.1f, %.1f)", unsigned(g_tracks[k].ptr),
                 g_tracks[k].cls, g_tracks[k].home[0], g_tracks[k].home[1], g_tracks[k].home[2], g_tracks[k].last[0],
                 g_tracks[k].last[1], g_tracks[k].last[2]);
        if (d < bd) {
            bd = d;
            best = &g_tracks[k];
        }
    }
    // "go": the Arisen put 60 u from the nearest object, on its side, facing it (a real lift with RT, tests). The
    // facing: yaw at +0x2578 (forward = (sin, 0, cos)) and the rotation at +0x50 (quaternion about y).
    if (strcmp(what, "go") == 0 && best) {
        float ox = best->last[0] - me[0], oz = best->last[2] - me[2];
        float d = std::sqrt(ox * ox + oz * oz);
        if (d > 1.f) {
            float yaw = std::atan2(ox, oz) + dx; // dx: a turn added (tests the convention)
            float to[3] = {best->last[0] - ox / d * 60.f, me[1], best->last[2] - oz / d * 60.f};
            for (uintptr_t off : {uintptr_t(0x40), uintptr_t(0xE70), uintptr_t(0xE80), uintptr_t(0x1990)})
                memcpy(reinterpret_cast<void *>(a + off), to, 12);
            float q[4] = {0.f, std::sin(yaw / 2.f), 0.f, std::cos(yaw / 2.f)};
            memcpy(reinterpret_cast<void *>(a + 0x50), q, 16);
            game::at<float>(a, 0x2578) = yaw;
            logf("objects: go to %08X class %08X at (%.1f, %.1f, %.1f): Arisen at (%.1f, %.1f), yaw %.2f", unsigned(best->ptr),
                 best->cls, best->last[0], best->last[1], best->last[2], to[0], to[2], yaw);
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
