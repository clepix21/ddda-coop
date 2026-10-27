// Test bridge: drive a running game from outside without restarting it (tests only).
// Enabled with bridge=1 in coop.ini. Every 50 ms:
//   - reads new lines from bridge_in.txt (append-only) into a queue; each command starts when
//     the previous one has finished, so a whole route can be written at once:
//       pad <slot> <buttons hex> <lx> <ly> <rx> <ry> <lt> <rt> <ms>   hold this state for ms
//       goto <x> <z> <radius> <timeout ms>                           steer the Arisen (slot 0)
//       wait <ms>                                                     do nothing for ms
//       stop                                                          clear the queue, release slot 0
//   - rewrites bridge_out.txt: time, Arisen and main pawn position, camera forward (x z),
//     current command.
// goto turns the world direction to the target into a camera-relative stick each tick, using
// the camera that the stick code itself uses ([[0x018D0B6C]+0x34], eye +0x40, target +0x60).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game.h"
#include "mod.h"

namespace {

struct Goto {
    bool active;
    float x, z, radius;
    DWORD until;
    // obstacle handling ("bug" steering): heading offset tried while stuck
    float offset_deg;
    int attempt;
    DWORD check_at, hold_until;
    float last_x, last_z;
};
const float kTryOffsets[] = {60, -60, 120, -120, 170, 30, -30, 90, -90};
Goto g_goto = {};
long g_lines_done = 0;
char g_queue[256][128];
int g_qhead = 0, g_qtail = 0;
DWORD g_busy_until = 0; // end of the current pad/wait command
char g_current[128] = "idle";

bool camera_forward(float &fx, float &fz)
{
    uintptr_t holder = game::at<uintptr_t>(0, 0x018D0B6C);
    if (!holder)
        return false;
    uintptr_t cam = game::at<uintptr_t>(holder, 0x34);
    if (!cam)
        return false;
    fx = game::at<float>(cam, 0x60) - game::at<float>(cam, 0x40);
    fz = game::at<float>(cam, 0x68) - game::at<float>(cam, 0x48);
    float n = std::sqrt(fx * fx + fz * fz);
    if (n < 1e-3f)
        return false;
    fx /= n;
    fz /= n;
    return true;
}

bool position(uintptr_t ch, float &x, float &z)
{
    if (!ch)
        return false;
    x = game::at<float>(ch, 0x40);
    z = game::at<float>(ch, 0x48);
    return true;
}

void run_line(const char *line)
{
    unsigned slot, buttons, ms;
    int lx, ly, rx, ry, lt, rt;
    float x, z, r;
    unsigned timeout;
    if (sscanf(line, "pad %u %x %d %d %d %d %d %d %u", &slot, &buttons, &lx, &ly, &rx, &ry, &lt, &rt, &ms) == 9 &&
        slot < 4) {
        XINPUT_GAMEPAD g = {};
        g.wButtons = WORD(buttons);
        g.sThumbLX = SHORT(lx);
        g.sThumbLY = SHORT(ly);
        g.sThumbRX = SHORT(rx);
        g.sThumbRY = SHORT(ry);
        g.bLeftTrigger = BYTE(lt);
        g.bRightTrigger = BYTE(rt);
        pads_override(slot, g, GetTickCount() + ms);
        g_busy_until = GetTickCount() + ms + 100;
        _snprintf(g_current, sizeof g_current, "pad %u %04X for %u ms", slot, buttons, ms);
    } else if (sscanf(line, "goto %f %f %f %u", &x, &z, &r, &timeout) == 4) {
        g_goto = {true, x, z, r, GetTickCount() + timeout, 0, 0, GetTickCount() + 600, 0, 0, 0};
        _snprintf(g_current, sizeof g_current, "goto %.0f %.0f", x, z);
    } else if (sscanf(line, "egoto #%u %f %u", &slot, &r, &timeout) == 3) {
        // egoto #slot <r> <ms> [class]: walk to a live enemy (only of that class: slots get reused)
        char cls[24] = {};
        sscanf(line, "egoto #%*u %*f %*u %23s", cls);
        EnemySlot list[128];
        int n = enemies_snapshot(list, 128);
        for (int k = 0; k < n; ++k)
            if (unsigned(list[k].index) == slot && (!cls[0] || strcmp(list[k].name, cls) == 0)) {
                x = game::at<float>(list[k].ptr, 0x40);
                z = game::at<float>(list[k].ptr, 0x48);
                g_goto = {true, x, z, r, GetTickCount() + timeout, 0, 0, GetTickCount() + 600, 0, 0, 0};
                logf("bridge: egoto #%u %s at (%.0f, %.0f)", slot, list[k].name, x, z);
            }
        _snprintf(g_current, sizeof g_current, "egoto #%u", slot);
    } else if (sscanf(line, "enear %f %u", &r, &timeout) == 2) {
        // enear <r> <ms> [max]: walk to the nearest living enemy within max u (default 1000; a far
        // one led the Arisen off a ledge, 2026-10-27 11:46) (tests that need kills)
        float max_d = 1000;
        sscanf(line, "enear %*f %*u %f", &max_d);
        EnemySlot list[128];
        int n = enemies_snapshot(list, 128), best = -1;
        uintptr_t me = possess_arisen();
        float bd = max_d * max_d;
        for (int k = 0; me && k < n; ++k) {
            float *hp = list[k].died ? nullptr : enemies_hp(list[k].ptr);
            if (!hp || hp[0] <= 0)
                continue;
            float dx = game::at<float>(list[k].ptr, 0x40) - game::at<float>(me, 0x40);
            float dz = game::at<float>(list[k].ptr, 0x48) - game::at<float>(me, 0x48);
            if (dx * dx + dz * dz < bd) {
                bd = dx * dx + dz * dz;
                best = k;
            }
        }
        if (best >= 0) {
            x = game::at<float>(list[best].ptr, 0x40);
            z = game::at<float>(list[best].ptr, 0x48);
            g_goto = {true, x, z, r, GetTickCount() + timeout, 0, 0, GetTickCount() + 600, 0, 0, 0};
            logf("bridge: enear #%d %s at (%.0f, %.0f)", list[best].index, list[best].name, x, z);
        }
        _snprintf(g_current, sizeof g_current, "enear");
    } else if (strncmp(line, "watch ", 6) == 0) { // watch <arisen|pawn> <offset hex> <count>
        char who[16] = {};
        unsigned off = 0, count = 20;
        if (sscanf(line + 6, "%15s %x %u", who, &off, &count) >= 2) {
            uintptr_t holder = game::at<uintptr_t>(0, 0x018D0B6C);
            uintptr_t ch = strcmp(who, "pawn") == 0     ? possess_main_pawn()
                           : strcmp(who, "camera") == 0 ? (holder ? game::at<uintptr_t>(holder, 0x34) : 0)
                                                         : possess_arisen();
            if (ch)
                watch_start(ch + off, long(count), strcmp(who, "camera") == 0 ? 0 : ch);
        }
        _snprintf(g_current, sizeof g_current, "watch %s +%X", who, off);
    } else if (strncmp(line, "watchp ", 7) == 0) { // watchp <arisen|pawn> <pointer offset> <field offset> <count>: writes that lower the value
        char who[16] = {};
        unsigned poff = 0, off = 0, count = 20;
        if (sscanf(line + 7, "%15s %x %x %u", who, &poff, &off, &count) >= 3) {
            uintptr_t ch = strcmp(who, "pawn") == 0 ? possess_main_pawn() : possess_arisen();
            uintptr_t p = ch ? game::at<uintptr_t>(ch, poff) : 0;
            logf("bridge: watchp %s [+%X] = %p, field +%X = %.2f", who, poff, (void *)p, off,
                 p ? game::at<float>(p, off) : 0.f);
            if (p)
                watch_start(p + off, long(count), 0, true); // HP: damage only
        }
        _snprintf(g_current, sizeof g_current, "watchp %s +%X +%X", who, poff, off);
    } else if (strncmp(line, "uscan ", 6) == 0) {
        // uscan <arisen|pawn> <u32>: where is this integer (the exp), in the object or up to two
        // pointers away? Logs each address and its path.
        char who[16] = {};
        unsigned v = 0;
        uintptr_t obj = 0;
        if (sscanf(line + 6, "%15s %u", who, &v) == 2)
            obj = strcmp(who, "pawn") == 0 ? possess_main_pawn() : possess_arisen();
        int hits = 0;
        auto ok = [](uintptr_t p) { return p >= 0x10000 && !(p & 3) && mem_readable((void *)p, 0x1000); };
        for (uintptr_t o = 0; obj && o < 0x7000 && hits < 40 && mem_readable((void *)(obj + o), 4); o += 4) {
            if (game::at<uint32_t>(obj, o) == v && ++hits)
                logf("bridge: uscan %s +0x%X (%p)", who, unsigned(o), (void *)(obj + o));
            uintptr_t p = game::at<uintptr_t>(obj, o);
            if (!ok(p))
                continue;
            for (uintptr_t q = 0; q < 0x1000 && hits < 40; q += 4) {
                if (game::at<uint32_t>(p, q) == v && ++hits)
                    logf("bridge: uscan %s [+0x%X]+0x%X (%p)", who, unsigned(o), unsigned(q), (void *)(p + q));
                uintptr_t r = game::at<uintptr_t>(p, q);
                if (r == obj || !ok(r))
                    continue;
                for (uintptr_t w = 0; w < 0x1000 && hits < 40; w += 4)
                    if (game::at<uint32_t>(r, w) == v && ++hits)
                        logf("bridge: uscan %s [[+0x%X]+0x%X]+0x%X (%p)", who, unsigned(o), unsigned(q), unsigned(w),
                             (void *)(r + w));
            }
        }
        _snprintf(g_current, sizeof g_current, "uscan %s (%d hits)", who, hits);
    } else if (strncmp(line, "iscan ", 6) == 0) {
        // iscan <num1> <no1> <num2> <no2>: item entries in memory, (s16 num, s16 itemNo) followed by
        // the next entry 8..64 bytes further; every committed read-write region of the process
        int n1 = 0, i1 = 0, n2 = 0, i2 = 0, hits = 0;
        if (sscanf(line + 6, "%d %d %d %d", &n1, &i1, &n2, &i2) == 4) {
            uint32_t a = uint16_t(n1) | uint32_t(uint16_t(i1)) << 16, b = uint16_t(n2) | uint32_t(uint16_t(i2)) << 16;
            MEMORY_BASIC_INFORMATION mbi;
            for (uintptr_t p = 0x10000; p < 0x7FFF0000 && hits < 20 && VirtualQuery((void *)p, &mbi, sizeof mbi);
                 p = uintptr_t(mbi.BaseAddress) + mbi.RegionSize) {
                if (mbi.State != MEM_COMMIT || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD))
                    continue;
                const uint8_t *base = (const uint8_t *)mbi.BaseAddress;
                for (size_t o = 0; o + 68 <= mbi.RegionSize && hits < 20; o += 2) {
                    if (*(const uint32_t *)(base + o) != a)
                        continue;
                    for (int s = 8; s <= 64; s += 2)
                        if (*(const uint32_t *)(base + o + s) == b) {
                            logf("bridge: iscan at %p stride %d", (void *)(base + o), s);
                            ++hits;
                        }
                }
            }
        }
        _snprintf(g_current, sizeof g_current, "iscan (%d hits)", hits);
    } else if (strncmp(line, "watchpa ", 8) == 0) { // watchpa <arisen|pawn> <pointer offset> <field offset> <count>: every change
        char who[16] = {};
        unsigned poff = 0, off = 0, count = 20;
        if (sscanf(line + 8, "%15s %x %x %u", who, &poff, &off, &count) >= 3) {
            uintptr_t ch = strcmp(who, "pawn") == 0 ? possess_main_pawn() : possess_arisen();
            uintptr_t p = ch ? game::at<uintptr_t>(ch, poff) : 0;
            logf("bridge: watchpa %s [+%X] = %p, field +%X = %u", who, poff, (void *)p, off, p ? game::at<uint32_t>(p, off) : 0);
            if (p)
                watch_start(p + off, long(count), 0);
        }
        _snprintf(g_current, sizeof g_current, "watchpa %s +%X +%X", who, poff, off);
    } else if (strncmp(line, "watcha ", 7) == 0) { // watcha <address hex> <count>: every change there
        unsigned addr = 0, count = 20;
        if (sscanf(line + 7, "%x %u", &addr, &count) >= 1 && mem_readable((void *)uintptr_t(addr), 4)) {
            logf("bridge: watcha %08X = %u", addr, game::at<uint32_t>(addr, 0));
            watch_start(addr, long(count), 0);
        }
        _snprintf(g_current, sizeof g_current, "watcha %08X", addr);
    } else if (strncmp(line, "camscan", 7) == 0) { // where does the camera point at the Arisen?
        uintptr_t holder = game::at<uintptr_t>(0, 0x018D0B6C), ar = possess_arisen();
        uintptr_t cam = holder ? game::at<uintptr_t>(holder, 0x34) : 0;
        logf("bridge: camscan holder %p camera %p arisen %p vt %08X", (void *)holder, (void *)cam, (void *)ar,
             cam ? unsigned(game::at<uintptr_t>(cam, 0)) : 0u);
        // FUN_007D7210: the camera copies eye/target from its active mode objects (+0x180, +0x188)
        uintptr_t m0 = cam ? game::at<uintptr_t>(cam, 0x180) : 0, m1 = cam ? game::at<uintptr_t>(cam, 0x184) : 0,
                  m2 = cam ? game::at<uintptr_t>(cam, 0x188) : 0;
        logf("bridge: camscan modes %p %p %p (vt %08X %08X %08X)", (void *)m0, (void *)m1, (void *)m2,
             m0 ? unsigned(game::at<uintptr_t>(m0, 0)) : 0u, m1 ? unsigned(game::at<uintptr_t>(m1, 0)) : 0u,
             m2 ? unsigned(game::at<uintptr_t>(m2, 0)) : 0u);
        const struct { uintptr_t base; size_t len; const char *name; } areas[] = {
            {holder, 0x400, "holder"}, {cam, 0x2400, "camera"}, {m0, 0x1000, "mode+180"}, {m1, 0x1000, "mode+184"},
            {m2, 0x1000, "mode+188"}};
        for (const auto &a : areas)
            if (a.base && ar)
                for (size_t o = 0; o + 4 <= a.len; o += 4)
                {
                    uintptr_t v = game::at<uintptr_t>(a.base, o);
                    if (v >= ar && v < ar + 0x5A10)
                        logf("bridge: camscan %s +0x%X points into the Arisen at +0x%X", a.name, unsigned(o),
                             unsigned(v - ar));
                }
        strcpy(g_current, "camscan");
    } else if (strncmp(line, "dump ", 5) == 0) { // dump <name>: raw Arisen and main pawn objects
        char name[64] = {};
        sscanf(line + 5, "%63s", name);
        const struct { uintptr_t ch; const char *who; size_t size; } objs[] = {
            {possess_arisen(), "arisen", 0x5A10}, {possess_main_pawn(), "pawn", 0x58E0}};
        for (const auto &o : objs) {
            if (!o.ch)
                continue;
            char path[128];
            _snprintf(path, sizeof path, "dump_%s_%s.bin", name, o.who);
            if (FILE *f = fopen(path, "wb")) {
                fwrite(reinterpret_cast<const void *>(o.ch), 1, o.size, f);
                fclose(f);
            }
        }
        _snprintf(g_current, sizeof g_current, "dump %s", name);
    } else if (strncmp(line, "edump ", 6) == 0) { // edump <name>: raw bytes of every live enemy
        char name[64] = {};
        sscanf(line + 6, "%63s", name);
        enemies_dump(name);
        _snprintf(g_current, sizeof g_current, "edump %s", name);
    } else if (strncmp(line, "ederef ", 7) == 0) { // ederef <name> <offset hex>: 0x2000 bytes at *(enemy+offset)
        char name[64] = {};
        unsigned off = 0;
        EnemySlot list[128];
        int n = enemies_snapshot(list, 128);
        if (sscanf(line + 7, "%63s %x", name, &off) == 2)
            for (int k = 0; k < n; ++k) {
                uintptr_t p = game::at<uintptr_t>(list[k].ptr, off);
                if (!mem_readable((void *)p, 0x2000))
                    continue;
                char path[128];
                _snprintf(path, sizeof path, "deref_%s_e%d.bin", name, list[k].index);
                if (FILE *f = fopen(path, "wb")) {
                    fwrite(reinterpret_cast<const void *>(p), 1, 0x2000, f);
                    fclose(f);
                }
            }
        _snprintf(g_current, sizeof g_current, "ederef %s +%X", name, off);
    } else if (sscanf(line, "ewatchhp #%u %u", &slot, &ms) == 2) { // HP drops of one enemy, with the stack
        EnemySlot list[128];
        int n = enemies_snapshot(list, 128);
        for (int k = 0; k < n; ++k)
            if (unsigned(list[k].index) == slot)
                if (float *hp = enemies_hp(list[k].ptr)) {
                    logf("bridge: ewatchhp #%u %s HP %.1f / %.1f at %p", slot, list[k].name, hp[0], hp[1], (void *)hp);
                    watch_start(reinterpret_cast<uintptr_t>(hp), long(ms), 0, true);
                }
        _snprintf(g_current, sizeof g_current, "ewatchhp #%u", slot);
    } else if (strncmp(line, "ehp #", 5) == 0) {
        // ehp #slot <hp> <dead 0|1> <invulnerable 0|1>: HP entry of one enemy (FUN_00776F50: +0x30 bit 0
        // skips damage, +0x110 bit 0 is set when HP reaches 0)
        unsigned dead = 0, inv = 0;
        float v = 0.f;
        if (sscanf(line + 5, "%u %f %u %u", &slot, &v, &dead, &inv) == 4) {
            EnemySlot list[128];
            int n = enemies_snapshot(list, 128);
            for (int k = 0; k < n; ++k)
                if (unsigned(list[k].index) == slot)
                    if (float *hp = enemies_hp(list[k].ptr)) {
                        uintptr_t entry = reinterpret_cast<uintptr_t>(hp) - 8;
                        logf("bridge: ehp #%u %s HP %.1f -> %.1f, +0x30 %02X, +0x110 %02X", slot, list[k].name, hp[0], v,
                             game::at<uint8_t>(entry, 0x30), game::at<uint8_t>(entry, 0x110));
                        hp[0] = v;
                        if (dead)
                            game::at<uint8_t>(entry, 0x110) |= 1;
                        if (inv)
                            game::at<uint8_t>(entry, 0x30) |= 1;
                    }
        }
        _snprintf(g_current, sizeof g_current, "ehp #%u", slot);
    } else if (strncmp(line, "eset ", 5) == 0 || strncmp(line, "ewatch ", 7) == 0) {
        // eset <class|*|#slot> <offset hex> <float>: write a float into every live enemy of that class
        // ewatch <class|*> <offset hex> <count>: write watchpoint on the first live enemy of that class
        bool set = line[1] == 's';
        char cls[24] = {};
        unsigned off = 0;
        float v = 0.f;
        EnemySlot list[128];
        int n = enemies_snapshot(list, 128), hit = 0;
        if (sscanf(line + (set ? 5 : 7), "%23s %x %f", cls, &off, &v) == 3)
            for (int k = 0; k < n; ++k) {
                bool match = strcmp(cls, "*") == 0 || strcmp(cls, list[k].name) == 0 ||
                             (cls[0] == '#' && atoi(cls + 1) == list[k].index);
                if (!match)
                    continue;
                ++hit;
                if (set) {
                    logf("bridge: eset #%d %s +%X %.1f -> %.1f", list[k].index, list[k].name, off,
                         game::at<float>(list[k].ptr, off), v);
                    *reinterpret_cast<float *>(list[k].ptr + off) = v;
                } else {
                    logf("bridge: ewatch #%d %s +%X", list[k].index, list[k].name, off);
                    watch_start(list[k].ptr + off, long(v), list[k].ptr);
                    break;
                }
            }
        _snprintf(g_current, sizeof g_current, "%s %s +%X (%d enemies)", set ? "eset" : "ewatch", cls, off, hit);
    } else if (strncmp(line, "ptrscan ", 8) == 0) {
        // ptrscan <arisen|pawn|#slot> <float>: where is this value, in the object or one pointer away?
        char who[16] = {};
        float v = 0.f;
        uintptr_t obj = 0;
        if (sscanf(line + 8, "%15s %f", who, &v) == 2) {
            if (strcmp(who, "pawn") == 0)
                obj = possess_main_pawn();
            else if (strcmp(who, "arisen") == 0)
                obj = possess_arisen();
            else if (who[0] == '#') {
                EnemySlot list[128];
                int n = enemies_snapshot(list, 128);
                for (int k = 0; k < n; ++k)
                    if (list[k].index == atoi(who + 1))
                        obj = list[k].ptr;
            }
        }
        int hits = 0;
        auto close_to = [v](float f) { return f >= v - 1.f && f <= v + 1.f; }; // the HUD rounds
        for (uintptr_t o = 0; obj && o < 0x7000 && hits < 40 && mem_readable((void *)(obj + o), 4); o += 4) {
            if (close_to(game::at<float>(obj, o)) && ++hits)
                logf("bridge: ptrscan %s +0x%X = %.2f", who, unsigned(o), game::at<float>(obj, o));
            uintptr_t p = game::at<uintptr_t>(obj, o);
            if (p < 0x10000 || (p & 3) || !mem_readable((void *)p, 0x1000))
                continue;
            for (uintptr_t q = 0; q < 0x1000 && hits < 40; q += 4)
                if (close_to(game::at<float>(p, q)) && ++hits)
                    logf("bridge: ptrscan %s [+0x%X] %p (vt %08X) +0x%X = %.2f", who, unsigned(o), (void *)p,
                         unsigned(game::at<uintptr_t>(p, 0)), unsigned(q), game::at<float>(p, q));
        }
        _snprintf(g_current, sizeof g_current, "ptrscan %s (%d hits)", who, hits);
    } else if (strncmp(line, "vtref ", 6) == 0) {
        // vtref <vtable hex>: objects with that vtable the Arisen points at (directly or one pointer
        // away), each with its first 0x200 bytes as pointers, labelled with their class when known
        unsigned vt = 0;
        uintptr_t a = possess_arisen();
        int hits = 0;
        if (sscanf(line + 6, "%x", &vt) == 1 && a) {
            auto show = [&](uintptr_t obj, const char *path) {
                ++hits;
                logf("bridge: vtref %08X at %p via %s", vt, (void *)obj, path);
                for (uintptr_t f = 4; f < 0x200; f += 4) {
                    uintptr_t v = game::at<uintptr_t>(obj, f);
                    if (v < 0x10000 || (v & 3) || !mem_readable((void *)v, 8))
                        continue;
                    uintptr_t fvt = game::at<uintptr_t>(v, 0);
                    if (fvt >= 0x00401000 && fvt < 0x01700000)
                        logf("bridge: vtref   +0x%X -> %p %s%s", unsigned(f), (void *)v, enemies_class_of(v),
                             v == a ? " (the Arisen)" : "");
                }
            };
            char path[32];
            for (uintptr_t o = 0; o < 0x8000 && hits < 8 && mem_readable((void *)(a + o), 4); o += 4) {
                uintptr_t p = game::at<uintptr_t>(a, o);
                if (p < 0x10000 || (p & 3) || !mem_readable((void *)p, 0x200))
                    continue;
                if (game::at<uintptr_t>(p, 0) == vt) {
                    _snprintf(path, sizeof path, "+0x%X", unsigned(o));
                    show(p, path);
                    continue;
                }
                uintptr_t pvt = game::at<uintptr_t>(p, 0);
                if (pvt < 0x00401000 || pvt > 0x01700000)
                    continue;
                for (uintptr_t q = 0; q < 0x200 && hits < 8; q += 4) {
                    uintptr_t r = game::at<uintptr_t>(p, q);
                    if (r >= 0x10000 && !(r & 3) && mem_readable((void *)r, 0x200) && game::at<uintptr_t>(r, 0) == vt) {
                        _snprintf(path, sizeof path, "[+0x%X]+0x%X", unsigned(o), unsigned(q));
                        show(r, path);
                    }
                }
            }
        }
        _snprintf(g_current, sizeof g_current, "vtref (%d)", hits);
    } else if (strncmp(line, "npcref", 6) == 0) {
        // npcref [tag]: for each NPC within 6 m of the Arisen, where the Arisen holds a pointer to
        // it, directly or one pointer away (the talk target: compare before and during a talk)
        char tag[16] = "-";
        sscanf(line + 6, "%15s", tag);
        uintptr_t npcs[64], a = possess_arisen();
        int n = probe_npcs(npcs, 64), hits = 0;
        float ax = 0, az = 0;
        position(a, ax, az);
        for (int k = 0; k < n && a; ++k) {
            float x, z;
            position(npcs[k], x, z);
            if ((x - ax) * (x - ax) + (z - az) * (z - az) > 600.f * 600.f)
                continue;
            logf("bridge: npcref %s npc %p %s at %.0f u", tag, (void *)npcs[k], enemies_class_of(npcs[k]),
                 std::sqrt((x - ax) * (x - ax) + (z - az) * (z - az)));
            // the other way round: the NPC holding the Arisen (only game objects: vtable inside the exe)
            for (uintptr_t o = 0; o < 0x8000 && hits < 60 && mem_readable((void *)(npcs[k] + o), 4); o += 4) {
                uintptr_t p = game::at<uintptr_t>(npcs[k], o);
                if (p == a && ++hits)
                    logf("bridge: npcref %s npc %p +0x%X -> arisen", tag, (void *)npcs[k], unsigned(o));
                if (p < 0x10000 || (p & 3) || p == a || !mem_readable((void *)p, 0x400))
                    continue;
                uintptr_t vt = game::at<uintptr_t>(p, 0);
                if (vt < 0x00401000 || vt > 0x01700000)
                    continue;
                for (uintptr_t q = 0; q < 0x400 && hits < 60; q += 4)
                    if (game::at<uintptr_t>(p, q) == a && ++hits)
                        logf("bridge: npcref %s npc %p [+0x%X] %p (vt %08X) +0x%X -> arisen", tag, (void *)npcs[k], unsigned(o),
                             (void *)p, unsigned(vt), unsigned(q));
            }
            for (uintptr_t o = 0; o < 0x8000 && hits < 60 && mem_readable((void *)(a + o), 4); o += 4) {
                uintptr_t p = game::at<uintptr_t>(a, o);
                if (p == npcs[k] && ++hits)
                    logf("bridge: npcref %s arisen +0x%X -> npc %p", tag, unsigned(o), (void *)npcs[k]);
                if (p < 0x10000 || (p & 3) || p == npcs[k] || !mem_readable((void *)p, 0x400) ||
                    game::at<uintptr_t>(p, 0) < 0x00401000 || game::at<uintptr_t>(p, 0) > 0x01700000)
                    continue;
                for (uintptr_t q = 0; q < 0x400 && hits < 60; q += 4)
                    if (game::at<uintptr_t>(p, q) == npcs[k] && ++hits)
                        logf("bridge: npcref %s arisen [+0x%X] %p (vt %08X) +0x%X -> npc %p", tag, unsigned(o), (void *)p,
                             unsigned(game::at<uintptr_t>(p, 0)), unsigned(q), (void *)npcs[k]);
            }
        }
        uintptr_t gm = game::at<uintptr_t>(game::kGameMain, 0);
        logf("bridge: npcref %s done: event state %d, %d hits", tag, gm ? game::at<int32_t>(gm, game::kGsEvent) : -1, hits);
        _snprintf(g_current, sizeof g_current, "npcref %s (%d hits)", tag, hits);
    } else if (strncmp(line, "netstart", 8) == 0) { // tests with net_late=1: both copies walk first
        static bool started = false;
        if (!started && cfg_role() != NET_OFF) {
            started = true;
            net_start(cfg_role(), cfg_peer(), cfg_port());
        }
        strcpy(g_current, "netstart");
    } else if (strncmp(line, "nlist", 5) == 0) { // nlist: NPCs near the Arisen, nearest first
        uintptr_t npcs[64];
        int n = probe_npcs(npcs, 64);
        float ax = 0, az = 0;
        position(possess_arisen(), ax, az);
        for (int a = 0; a < n; ++a) // sort by distance (few entries)
            for (int b = a + 1; b < n; ++b) {
                float xa, za, xb, zb;
                position(npcs[a], xa, za);
                position(npcs[b], xb, zb);
                if ((xb - ax) * (xb - ax) + (zb - az) * (zb - az) < (xa - ax) * (xa - ax) + (za - az) * (za - az))
                    std::swap(npcs[a], npcs[b]);
            }
        for (int k = 0; k < n && k < 12; ++k) {
            float x, z;
            position(npcs[k], x, z);
            logf("bridge: nlist #%d npc %p at (%.0f, %.0f), %.0f u away", k, (void *)npcs[k], x, z,
                 std::sqrt((x - ax) * (x - ax) + (z - az) * (z - az)));
        }
        _snprintf(g_current, sizeof g_current, "nlist (%d)", n);
    } else if (sscanf(line, "ngoto #%u %f %u", &slot, &r, &timeout) == 3) { // walk to the k-th nearest NPC
        uintptr_t npcs[64];
        int n = probe_npcs(npcs, 64);
        float ax = 0, az = 0, best = 1e30f;
        position(possess_arisen(), ax, az);
        uintptr_t pick = 0;
        for (unsigned rank = 0; rank <= slot; ++rank) { // the slot-th nearest
            pick = 0;
            best = 1e30f;
            for (int k = 0; k < n; ++k) {
                float x, z;
                position(npcs[k], x, z);
                float d = (x - ax) * (x - ax) + (z - az) * (z - az);
                if (npcs[k] && d < best) {
                    best = d;
                    pick = npcs[k];
                }
            }
            if (rank < slot)
                for (int k = 0; k < n; ++k)
                    if (npcs[k] == pick)
                        npcs[k] = 0;
        }
        if (pick) {
            position(pick, x, z);
            g_goto = {true, x, z, r, GetTickCount() + timeout, 0, 0, GetTickCount() + 600, 0, 0, 0};
            logf("bridge: ngoto #%u npc %p at (%.0f, %.0f)", slot, (void *)pick, x, z);
        }
        _snprintf(g_current, sizeof g_current, "ngoto #%u", slot);
    } else if (sscanf(line, "camto %u", &slot) == 1) { // camto <pawn>: the camera follows that pawn (tests)
        int n = slot < unsigned(kPawns) ? camera_follow(possess_arisen(), possess_pawn_ptr(int(slot))) : 0;
        logf("bridge: camto %u: %d camera references moved", slot, n);
        _snprintf(g_current, sizeof g_current, "camto %u", slot);
    } else if (strncmp(line, "codectest", 9) == 0) {
        // the kept save: unpack, the Arisen's character over pawn 1, pack, unpack again, check;
        // codec_out.sav for tools/save_merge.py (tests)
        std::vector<uint8_t> file, packed;
        uint32_t version = 0, head[8], head2[8];
        std::string xml, ch, xml2, ch2;
        DWORD t0 = GetTickCount();
        bool ok = storage_own_save(file, version) && save_unpack(file, xml, head);
        DWORD t1 = GetTickCount();
        ok = ok && save_character(xml, -1, ch) && save_put_character(xml, 1, ch);
        DWORD t2 = GetTickCount();
        ok = ok && save_pack(xml, head, packed);
        DWORD t3 = GetTickCount();
        ok = ok && save_unpack(packed, xml2, head2) && xml2 == xml && save_character(xml2, 1, ch2) && ch2 == ch;
        logf("bridge: codectest %s: save v%u, xml %u bytes, character \"%s\" %u bytes, packed %u bytes; unpack %lu ms, "
             "merge %lu ms, pack %lu ms", ok ? "OK" : "FAILED", version, unsigned(xml.size()),
             save_character_name(ch).c_str(), unsigned(ch.size()), ok ? head2[2] : 0u, t1 - t0, t2 - t1, t3 - t2);
        if (ok)
            if (FILE *f = fopen("codec_out.sav", "wb")) {
                fwrite(packed.data(), 1, packed.size(), f);
                fclose(f);
            }
        strcpy(g_current, "codectest");
    } else if (strncmp(line, "where", 5) == 0) { // where: the Arisen's position
        if (uintptr_t a = possess_arisen())
            logf("bridge: where %.0f %.0f %.0f", game::at<float>(a, 0x40), game::at<float>(a, 0x44), game::at<float>(a, 0x48));
        _snprintf(g_current, sizeof g_current, "where");
    } else if (strncmp(line, "partyhp", 7) == 0) { // HP of the Arisen and the three pawns
        const float *a = enemies_hp(possess_arisen());
        logf("bridge: partyhp arisen %.1f/%.1f", a ? a[0] : -1.f, a ? a[1] : -1.f);
        for (int p = 0; p < kPawns; ++p) {
            const float *h = possess_pawn_ptr(p) ? enemies_hp(possess_pawn_ptr(p)) : nullptr;
            logf("bridge: partyhp pawn %d %.1f/%.1f", p, h ? h[0] : -1.f, h ? h[1] : -1.f);
        }
        strcpy(g_current, "partyhp");
    } else if (strncmp(line, "php ", 4) == 0) { // php <arisen|pawn> <hp>: set a party member's HP (tests)
        char who[16] = {};
        float v = 0.f;
        if (sscanf(line + 4, "%15s %f", who, &v) == 2) {
            uintptr_t ch = strcmp(who, "pawn") == 0 ? possess_main_pawn() : possess_arisen();
            if (float *hp = ch ? enemies_hp(ch) : nullptr) {
                logf("bridge: php %s HP %.1f -> %.1f (max %.1f)", who, hp[0], v, hp[1]);
                hp[0] = v;
            }
        }
        _snprintf(g_current, sizeof g_current, "php %s", who);
    } else if (strncmp(line, "pkill", 5) == 0) { // pkill: the Arisen's dead flag (tests)
        enemies_queue_arisen_kill();
        strcpy(g_current, "pkill");
    } else if (strncmp(line, "pdmg ", 5) == 0) { // pdmg <damage>: the Arisen takes damage the game's way (tests)
        float v = 0.f;
        if (sscanf(line + 5, "%f", &v) == 1)
            enemies_queue_arisen_damage(v);
        _snprintf(g_current, sizeof g_current, "pdmg %.0f", v);
    } else if (strncmp(line, "findvt ", 7) == 0) { // findvt <vtable hex>: globals in .data pointing at such an object
        unsigned vt = 0;
        int hits = 0;
        if (sscanf(line + 7, "%x", &vt) == 1)
            for (uintptr_t g = 0x01621000; g < 0x019AB1E0 && hits < 20; g += 4) {
                uintptr_t obj = game::at<uintptr_t>(g, 0);
                if (obj < 0x10000 || (obj & 3) || !mem_readable(reinterpret_cast<void *>(obj), 4))
                    continue;
                if (game::at<uintptr_t>(obj, 0) == vt && ++hits)
                    logf("bridge: findvt %08X: global %08X -> %08X", vt, unsigned(g), unsigned(obj));
            }
        _snprintf(g_current, sizeof g_current, "findvt (%d)", hits);
    } else if (strncmp(line, "dumpg ", 6) == 0) {
        // dumpg <global hex> <size hex> <name> [offset hex]: bytes of *global, or of *(*global + offset)
        unsigned g = 0, size = 0, off = 0;
        char name[64] = {};
        int nargs = sscanf(line + 6, "%x %x %63s %x", &g, &size, name, &off);
        if (nargs >= 3) {
            uintptr_t obj = game::at<uintptr_t>(g, 0);
            if (nargs == 4 && obj)
                obj = game::at<uintptr_t>(obj, off);
            while (size > 0x100 && !mem_readable(reinterpret_cast<void *>(obj), size))
                size -= 0x100;
            char path[128];
            _snprintf(path, sizeof path, "dumpg_%s.bin", name);
            if (obj && mem_readable(reinterpret_cast<void *>(obj), size))
                if (FILE *f = fopen(path, "wb")) {
                    fwrite(reinterpret_cast<const void *>(obj), 1, size, f);
                    fclose(f);
                }
            logf("bridge: dumpg %08X -> %08X, %X bytes as %s", g, unsigned(obj), size, path);
        }
        _snprintf(g_current, sizeof g_current, "dumpg %s", name);
    } else if (sscanf(line, "gsflag %u", &ms) == 1 && ms < 64) {
        // gsflag <bit>: sets a bit of sGameSys +0xB8840 (25 = game over, 26 = reload from +0x50)
        if (uintptr_t gs = game::at<uintptr_t>(game::kGameMain, 0))
            InterlockedOr(reinterpret_cast<volatile LONG *>(gs + 0xB8840 + ms / 32 * 4), LONG(1u << (ms % 32)));
        _snprintf(g_current, sizeof g_current, "gsflag %u", ms);
    } else if (strncmp(line, "savereq ", 8) == 0) {
        // savereq <mode hex> <slot>: a request to the save/load machine [0x018FA518], written the
        // way the game's menu does it (FUN_00532110), only when the machine is idle (+0x648 = 0)
        unsigned mode = 0, slot = 0;
        uintptr_t sm = game::at<uintptr_t>(0x018FA518, 0);
        if (sscanf(line + 8, "%x %u", &mode, &slot) == 2 && sm && game::at<uint8_t>(sm, 0x648) == 0) {
            game::at<int32_t>(sm, 0xA44) = -1;
            game::at<int32_t>(sm, 0xA48) = -1;
            game::at<uint8_t>(sm, 0x638) = uint8_t(slot);
            game::at<uint8_t>(sm, 0x63B) = 1;
            game::at<int32_t>(sm, 0xA4C) = 0;
            game::at<int32_t>(sm, 0xA50) = 0;
            game::at<uint8_t>(sm, 0x829) = 1;
            game::at<uint8_t>(sm, 0x64D) = 0;
            game::at<uint8_t>(sm, 0x82A) = mode == 0x12;
            game::at<uint8_t>(sm, 0x649) = 0; // state, then the mode last: the machine starts on it
            MemoryBarrier();
            game::at<uint8_t>(sm, 0x648) = uint8_t(mode);
            logf("bridge: savereq mode %X slot %u sent to %08X", mode, slot, unsigned(sm));
        } else {
            logf("bridge: savereq refused (machine %08X busy or bad args)", unsigned(sm));
        }
        _snprintf(g_current, sizeof g_current, "savereq");
    } else if (sscanf(line, "wait %u", &ms) == 1) {
        g_busy_until = GetTickCount() + ms;
        _snprintf(g_current, sizeof g_current, "wait %u ms", ms);
    } else if (strncmp(line, "stop", 4) == 0) {
        g_qhead = g_qtail = 0;
        g_busy_until = 0;
        g_goto.active = false;
        pads_override(0, XINPUT_GAMEPAD{}, 0);
        strcpy(g_current, "idle");
    } else {
        logf("bridge: unknown command: %s", line);
        return;
    }
    logf("bridge: %s", line);
}

void poll_input()
{
    FILE *f = fopen("bridge_in.txt", "r");
    if (!f)
        return;
    char line[256];
    long n = 0;
    while (fgets(line, sizeof line, f)) {
        if (++n <= g_lines_done)
            continue;
        line[strcspn(line, "\r\n")] = 0;
        g_lines_done = n;
        if (!line[0] || line[0] == '#')
            continue;
        if (strncmp(line, "stop", 4) == 0) { // stop jumps the queue
            run_line(line);
            continue;
        }
        if ((g_qtail + 1) % 256 != g_qhead) {
            strncpy(g_queue[g_qtail], line, 127);
            g_queue[g_qtail][127] = 0;
            g_qtail = (g_qtail + 1) % 256;
        }
    }
    fclose(f);
}

void steer()
{
    if (!g_goto.active)
        return;
    float ax, az, fx, fz;
    if (!position(possess_arisen(), ax, az) || !camera_forward(fx, fz))
        return;
    float dx = g_goto.x - ax, dz = g_goto.z - az, d = std::sqrt(dx * dx + dz * dz);
    if (d <= g_goto.radius || GetTickCount() > g_goto.until) {
        logf("bridge: goto %s at (%.0f, %.0f), %.0f u from target", d <= g_goto.radius ? "reached" : "TIMEOUT", ax, az, d);
        g_goto.active = false;
        pads_override(0, XINPUT_GAMEPAD{}, 0);
        strcpy(g_current, d <= g_goto.radius ? "goto reached" : "goto timeout");
        return;
    }
    dx /= d;
    dz /= d;
    DWORD now = GetTickCount();
    if (int(now - g_goto.check_at) >= 0) { // progress check every 600 ms
        float moved = std::sqrt((ax - g_goto.last_x) * (ax - g_goto.last_x) + (az - g_goto.last_z) * (az - g_goto.last_z));
        if (moved < 20) {
            g_goto.offset_deg = kTryOffsets[g_goto.attempt++ % (sizeof kTryOffsets / sizeof kTryOffsets[0])];
            g_goto.hold_until = now + 1500;
            logf("bridge: stuck at (%.0f, %.0f), trying %+.0f deg", ax, az, g_goto.offset_deg);
        } else if (!g_goto.hold_until || int(now - g_goto.hold_until) >= 0) {
            g_goto.offset_deg *= 0.5f; // drift back toward the direct line
            if (std::fabs(g_goto.offset_deg) < 5)
                g_goto.offset_deg = 0, g_goto.attempt = 0;
        }
        logf("bridge: at %.0f %.0f", ax, az);
        g_goto.last_x = ax;
        g_goto.last_z = az;
        g_goto.check_at = now + 600;
    }
    if (g_goto.offset_deg != 0) {
        float a = g_goto.offset_deg * 3.14159265f / 180, c = std::cos(a), s_ = std::sin(a);
        float nx = dx * c - dz * s_, nz = dx * s_ + dz * c;
        dx = nx;
        dz = nz;
    }
    // Camera basis on the ground plane: forward (fx, fz); right = (-fz, fx) or (fz, -fx).
    // The sign of "right" comes from bridge_sign (coop.ini), found by experiment.
    float s = float(possess_bridge_sign());
    float rxv = -fz * s, rzv = fx * s;
    float sy = dx * fx + dz * fz;   // stick up = forward
    float sx = dx * rxv + dz * rzv; // stick right = right
    XINPUT_GAMEPAD g = {};
    g.sThumbLX = SHORT(sx * 30000);
    g.sThumbLY = SHORT(sy * 30000);
    pads_override(0, g, GetTickCount() + 200);
}

// Logs every change of the save/load machine's mode, state and slot (seen every 50 ms).
void watch_save_machine()
{
    static uint32_t last = 0xFFFFFFFF;
    uintptr_t sm = game::at<uintptr_t>(0x018FA518, 0);
    if (!sm || !mem_readable(reinterpret_cast<void *>(sm + 0x638), 0x20))
        return;
    uint32_t now = game::at<uint8_t>(sm, 0x648) | game::at<uint8_t>(sm, 0x649) << 8 | game::at<uint8_t>(sm, 0x638) << 16;
    if (now != last)
        logf("bridge: save machine mode %X state %X slot %u", now & 0xFF, now >> 8 & 0xFF, now >> 16);
    last = now;
}

void write_status()
{
    FILE *f = fopen("bridge_out.txt.tmp", "w");
    if (!f)
        return;
    float ax = 0, az = 0, px = 0, pz = 0, fx = 0, fz = 0;
    bool a = position(possess_arisen(), ax, az), p = position(possess_main_pawn(), px, pz), c = camera_forward(fx, fz);
    fprintf(f, "tick %lu\narisen %s %.1f %.1f\npawn %s %.1f %.1f\ncamera_fwd %s %.3f %.3f\ncommand %s\nlines_done %ld\nqueued %d\n",
            GetTickCount(), a ? "ok" : "none", ax, az, p ? "ok" : "none", px, pz, c ? "ok" : "none", fx, fz, g_current,
            g_lines_done, (g_qtail - g_qhead + 256) % 256);
    fclose(f);
    MoveFileExA("bridge_out.txt.tmp", "bridge_out.txt", MOVEFILE_REPLACE_EXISTING);
}

DWORD WINAPI bridge_thread(LPVOID)
{
    logf("bridge: listening on bridge_in.txt");
    for (;;) {
        poll_input();
        bool busy = g_busy_until && int(GetTickCount() - g_busy_until) < 0; // 0 = not busy (tick count may exceed 2^31)
        if (!g_goto.active && !busy && g_qhead != g_qtail) {
            run_line(g_queue[g_qhead]);
            g_qhead = (g_qhead + 1) % 256;
        }
        steer();
        watch_save_machine();
        write_status();
        Sleep(50);
    }
}

} // namespace

void bridge_start()
{
    DeleteFileA("bridge_in.txt");
    if (HANDLE t = CreateThread(nullptr, 0, bridge_thread, nullptr, 0, nullptr))
        CloseHandle(t);
}
