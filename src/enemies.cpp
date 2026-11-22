// Enemies (milestone 4). Every uEnemy runs the uCharacterBase transform finalize FUN_0084BF60 once
// per frame, called from uEnemy's vtable slot 91 (FUN_00AA99E0, call at 0x00AA9A03). All 123 uEm*
// classes in the exe either keep that slot or call FUN_00AA99E0 from their override (checked with
// a vtable scan of DDDA.exe). The finalize hook in probe.cpp passes calls returning to 0x00AA9A08
// here, which gives the list of live enemies without knowing the game's unit lists.
//
// Class names come from the object itself: vtable slot 4 is getDTI (`mov eax, <MtDTI>; ret`), and
// the MtDTI holds the class name pointer at +4.
//
// Damage: FUN_00776F50 subtracts damage from an HP entry ([[ch+0x1D18]]; EAX = entry, two stack
// arguments: float damage and a flag; `ret 8`). It skips when entry+0x30 bit 0 is set, and when HP
// reaches 0 it sets entry+0x110 bit 0. entry+0x1B4 is the character. Found with a write watch on a
// small creature's HP while the party killed it (called from FUN_0076DB10 at 0x0076E278/0x0076E316).
// The hook asks net.cpp whether to drop the hit (the guest, for enemies the host says are alive).
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "game.h"
#include "mod.h"

// Called from the asm thunk below with the HP entry; true = skip the original (no damage).
// uEnemy's death handler (vtable slot 0x120): every enemy class seen calls it from its override.
using Death_t = void(__fastcall *)(uintptr_t, void *, uint32_t, uint32_t); // thiscall through fastcall
Death_t g_death_orig = nullptr;
void __fastcall hk_death(uintptr_t ch, void *edx, uint32_t a, uint32_t hit);

extern "C" bool __cdecl enemies_damage_filter(uintptr_t entry, float *dmg) asm("enemies_damage_filter");
extern "C" void *g_damage_orig asm("g_damage_orig");
void *g_damage_orig = nullptr;

// Who hits: FUN_0076DB10 (__stdcall: damage owner, hit record = owner + 0x640, flag) applies a hit and calls
// the damage function above; the hit record's +0x50 is the attacker (the game reads its +0x2AC8 and its class
// there; decompiled 2026-11-22, out/decomp_dmg.txt). Kept per thread while the hit is applied.
using HitApply_t = void(__stdcall *)(uintptr_t, uintptr_t, uint32_t);
HitApply_t g_hit_orig = nullptr;
thread_local uintptr_t t_attacker = 0;
uintptr_t enemies_current_attacker() { return t_attacker; }
void __stdcall hk_hit_apply(uintptr_t owner, uintptr_t hit, uint32_t flag)
{
    uintptr_t prev = t_attacker;
    t_attacker = hit ? game::at<uintptr_t>(hit, 0x50) : 0;
    g_hit_orig(owner, hit, flag);
    t_attacker = prev;
}
// a character as the logs name it: the Arisen, a pawn, or "other". Never action_name() here: an attacker
// can be another object (a shot, a hit box), and action_name read its +0x2DC8 and called a "vtable" there:
// the guest jumped to 0x00156000 and closed (DEP, guest hits suite 2026-11-22 04:11:43).
const char *who_is(uintptr_t ch)
{
    static const char *pawns[] = {"pawn 0", "pawn 1", "pawn 2"};
    if (!ch)
        return "nobody";
    if (ch == possess_arisen())
        return "the Arisen";
    for (int p = 0; p < kPawns && p < 3; ++p)
        if (ch == possess_pawn_ptr(p))
            return pawns[p];
    return "other";
}

extern "C" bool __cdecl enemies_damage_filter(uintptr_t entry, float *dmg)
{
    uintptr_t ch = game::at<uintptr_t>(entry, 0x1B4);
    { // the first 40 hits of a session: who hit whom (checks hit + 0x50)
        static LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 40)
            logf("enemies: hit %.1f on %s (%08X) by %s (%08X)", dmg ? *dmg : 0.f, who_is(ch), unsigned(ch),
                 who_is(t_attacker), unsigned(t_attacker));
    }
    if (ch && revive_on_damage(ch, dmg)) // shared world: down instead of dead (revive.cpp)
        return true;
    if (ch && enemy_net_puppet_hit(t_attacker, ch)) // host: that guest's own hit comes over the network
        return true;
    if (!ch || !enemy_net_block_damage(ch))
        return false;
    if (dmg && t_attacker && t_attacker == possess_arisen()) // guest: its own hit counts on the host
        enemy_net_forward_hit(ch, *dmg);
    if (dmg && enemy_net_feel_hit(ch)) { // played here, never the kill: HP entry +8 stays above 0
        float cap = game::at<float>(entry, 8) - 1.f;
        if (*dmg > cap)
            *dmg = cap > 0.f ? cap : 0.f;
        return false;
    }
    return true;
}

// EAX = HP entry, [esp+4] damage, [esp+8] flag; the callee pops 8 bytes. EAX and the stack are
// kept for the original; ECX/EDX/XMM0 are free on entry (the original sets them before use).
extern "C" __attribute__((naked)) void enemies_damage_thunk() asm("enemies_damage_thunk");
extern "C" __attribute__((naked)) void enemies_damage_thunk()
{
    asm volatile("pushl %eax\n\t"
                 "leal 8(%esp), %ecx\n\t" // the damage argument (may be cut by the filter)
                 "pushl %ecx\n\t"
                 "pushl %eax\n\t"
                 "call enemies_damage_filter\n\t"
                 "addl $8, %esp\n\t"
                 "testb %al, %al\n\t"
                 "popl %eax\n\t"
                 "jnz 1f\n\t"
                 "jmp *g_damage_orig\n"
                 "1:\n\t"
                 "ret $8\n\t");
}

// Tests: damage to the Arisen through the game's own damage function (HP 0 written directly does not
// kill: the death comes from that function). Queued by the bridge, applied on the game thread.
volatile LONG g_queued_damage = 0; // float bits, 0 = none
volatile LONG g_queued_kill = 0;

void enemies_queue_arisen_kill() { InterlockedExchange(&g_queued_kill, 1); }
volatile LONG g_queued_pawn_kill = 0; // tests: 1 + pawn index
void enemies_queue_pawn_kill(int pawn) { InterlockedExchange(&g_queued_pawn_kill, 1 + pawn); }

void enemies_queue_arisen_damage(float dmg)
{
    LONG bits;
    memcpy(&bits, &dmg, 4);
    InterlockedExchange(&g_queued_damage, bits);
}

void enemies_run_queued(uintptr_t arisen)
{
    if (InterlockedExchange(&g_queued_kill, 0) && arisen && enemies_hp(arisen)) {
        uintptr_t e = game::at<uintptr_t>(game::at<uintptr_t>(arisen, 0x1D18), 0);
        game::at<float>(e, 8) = 0.f;
        game::at<uint32_t>(e, 0x110) |= 1; // the dead flag FUN_0076E7C0 consumes
        logf("enemies: test kill of the Arisen (HP entry %08X)", unsigned(e));
    }
    if (LONG pk = InterlockedExchange(&g_queued_pawn_kill, 0)) { // tests: a pawn's HP 0 and dead flag
        uintptr_t ch = possess_pawn_ptr(int(pk) - 1);
        if (ch && enemies_hp(ch)) {
            uintptr_t e = game::at<uintptr_t>(game::at<uintptr_t>(ch, 0x1D18), 0);
            game::at<float>(e, 8) = 0.f;
            game::at<uint32_t>(e, 0x110) |= 1;
            logf("enemies: test kill of pawn %ld (HP entry %08X)", pk - 1, unsigned(e));
        }
    }
    LONG bits = InterlockedExchange(&g_queued_damage, 0);
    if (!bits || !arisen || !enemies_hp(arisen))
        return;
    uint32_t b = uint32_t(bits);
    float dmg;
    memcpy(&dmg, &b, 4);
    logf("enemies: test damage %.1f to the Arisen", dmg);
    enemies_apply_damage(arisen, dmg);
}

// The game's own damage function on a character's HP entry (game thread): its death comes with it when
// the HP reaches 0 (HP 0 written directly does not kill). Goes through the damage hook above.
void enemies_apply_damage(uintptr_t ch, float dmg)
{
    if (!ch || !enemies_hp(ch))
        return;
    uintptr_t entry = game::at<uintptr_t>(game::at<uintptr_t>(ch, 0x1D18), 0);
    uintptr_t fn = game::kApplyDamage;
    uint32_t b;
    memcpy(&b, &dmg, 4);
    // EAX = HP entry, push flag then damage; the callee pops both
    asm volatile("pushl $0\n\t"
                 "pushl %%edx\n\t"
                 "call *%%ecx\n\t"
                 : "+a"(entry), "+c"(fn), "+d"(b)
                 :
                 : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
}

bool mem_readable(const void *p, size_t n)
{
    if (reinterpret_cast<uintptr_t>(p) + n < reinterpret_cast<uintptr_t>(p))
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// The same for the live characters this file reads every frame (class, HP entry), with the regions
// VirtualQuery gave kept 250 ms per thread: it takes the process's address-space lock, and the game's
// worker threads calling it for every enemy (enemies_hp: three checks) spent 0.5-0.8 ms per call
// there (perf:, 2026-10-29). Not for scans: a region freed meanwhile still passes (a bridge scan with
// the cache in mem_readable read freed memory, shared exp suite 2026-10-29).
bool live_readable(const void *p, size_t n)
{
    struct Region {
        uintptr_t lo, hi;
        DWORD until;
    };
    static thread_local Region kept[8] = {};
    static thread_local int next = 0;
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a + n < a || a < 0x10000) // wraps (a code pointer read as FFFFFFFE passed through a kept region and crashed)
        return false;
    DWORD now = GetTickCount();
    for (const Region &r : kept)
        if (a >= r.lo && a + n <= r.hi && int(r.until - now) > 0)
            return true;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress), hi = lo + mbi.RegionSize;
    kept[next] = {lo, hi, now + 250};
    next = (next + 1) % 8;
    return a + n <= hi;
}

// The object holding the open world's cell origin: the uWaterReflectionMap (game.h kFieldOriginPos), found by
// its class name among the objects the exe's globals point to (field_origin_scan), 0 until found or once gone.
volatile uintptr_t g_wmap = 0, g_wmap_vt = 0;

uintptr_t field_origin_object()
{
    uintptr_t o = g_wmap, vt = g_wmap_vt;
    if (!o || !vt || !live_readable(reinterpret_cast<void *>(o), game::kFieldOriginPos + 12) || game::at<uintptr_t>(o, 0) != vt)
        return 0;
    return o;
}

// Its own thread: looks for the uWaterReflectionMap once a second while there is none (a few thousand pointer
// checks: the globals pointing to it change from run to run, its class does not; GOG's globals sit in the
// same range, a little lower). A pass takes long (a VirtualQuery per pointer): on the net thread, away from
// the open world where there is none, it held the players' states back 1-3 s (0.10.1 Steam series).
void field_origin_pass();

void spawn_scan_pass();

DWORD WINAPI field_origin_thread(void *)
{
    for (;;) {
        Sleep(1000);
        if (!field_origin_object() && possess_arisen()) // in game only: while the game starts or loads, globals point to memory being freed
            field_origin_pass();
        static DWORD next_spawn = 0; // the spawn manager (issue #6): every 5 s until found
        if (possess_arisen() && (!next_spawn || int(GetTickCount() - next_spawn) >= 0)) {
            next_spawn = (GetTickCount() + 5000) | 1;
            spawn_scan_pass();
        }
    }
}

void field_origin_scan()
{
    static volatile LONG started = 0;
    if (InterlockedExchange(&started, 1) == 0) {
        HANDLE t = CreateThread(nullptr, 0, field_origin_thread, nullptr, 0, nullptr);
        if (t) {
            SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(t);
        }
    }
}

void field_origin_pass()
{
    static uintptr_t rejected[64] = {};
    static int n_rejected = 0;
    // seen at 0x01877DB0 .. 0x018798B8 (Steam); GOG's globals sit about 0x1F030 lower
    uintptr_t lo = game::build() == game::BUILD_GOG ? 0x01840000 : 0x01860000;
    for (uintptr_t g = lo; g < lo + 0x30000; g += 4) {
        // the exe's globals: one check a page (a VirtualQuery takes the address-space lock the game's
        // threads need too)
        if ((g & 0xFFF) == 0 && !mem_readable(reinterpret_cast<void *>(g), 0x1000)) {
            g += 0x1000 - 4;
            continue;
        }
        uintptr_t q = game::at<uintptr_t>(g, 0);
        if (q < 0x10000 || q >= 0x7FFF0000 || (q & 3) || !mem_readable(reinterpret_cast<void *>(q), 4))
            continue;
        uintptr_t vt = game::at<uintptr_t>(q, 0);
        if (vt < 0x00401000 || vt >= 0x01A00000)
            continue;
        if (g_wmap_vt) {
            if (vt == g_wmap_vt && live_readable(reinterpret_cast<void *>(q), game::kFieldOriginPos + 12)) {
                g_wmap = q;
                logf("field: cell origin in uWaterReflectionMap %08X (global %08X)", unsigned(q), unsigned(g));
                return;
            }
            continue;
        }
        bool seen = false;
        for (int k = 0; k < n_rejected && !seen; ++k)
            seen = rejected[k] == vt;
        if (seen)
            continue;
        // the class name as class_of reads it, with every pointer in the exe image (always mapped) and no
        // kept regions: class_of's live_readable passed a heap region freed meanwhile and crashed the
        // scan on fn[0] (5 games in the 0.10.1 Steam series, 2026-11-16)
        auto in_exe = [](uintptr_t a) { return a >= 0x00401000 && a < 0x01A00000; };
        uintptr_t fnp = mem_readable(reinterpret_cast<void *>(vt + 16), 4) ? game::at<uintptr_t>(vt, 16) : 0;
        if (!in_exe(fnp) || !mem_readable(reinterpret_cast<void *>(fnp), 6))
            goto reject;
        {
            auto fn = reinterpret_cast<const uint8_t *>(fnp);
            uintptr_t dti = 0, name = 0;
            if (fn[0] == 0xB8 && fn[5] == 0xC3) // mov eax, imm32; ret
                memcpy(&dti, fn + 1, 4);
            if (in_exe(dti) && mem_readable(reinterpret_cast<void *>(dti + 4), 4))
                name = game::at<uintptr_t>(dti, 4);
            if (!in_exe(name) || !mem_readable(reinterpret_cast<void *>(name), 20)
                || memcmp(reinterpret_cast<const char *>(name), "uWaterReflectionMap", 20) != 0)
                goto reject;
        }
        if (live_readable(reinterpret_cast<void *>(q), game::kFieldOriginPos + 12)) {
            g_wmap_vt = vt;
            g_wmap = q;
            logf("field: cell origin in uWaterReflectionMap %08X (global %08X, vtable %08X)", unsigned(q), unsigned(g), unsigned(vt));
            return;
        }
    reject:
        if (n_rejected < 64)
            rejected[n_rejected++] = vt;
    }
}

// The open world's cell origin (game.h kFieldOriginPos): place in the world = place here - origin, along x
// and z. False (and 0) when there is none, or when it does not look like one (whole 10000s).
bool field_origin(float &ox, float &oz)
{
    ox = oz = 0.f;
    uintptr_t fo = field_origin_object();
    if (!fo)
        return false;
    float x = game::at<float>(fo, game::kFieldOriginPos), z = game::at<float>(fo, game::kFieldOriginPos + 8);
    auto cell = [](float v) { return v == v && std::fabs(v) < 1e7f && std::fmod(v, 10000.f) == 0.f; };
    if (!cell(x) || !cell(z))
        return false;
    ox = x;
    oz = z;
    return true;
}

void field_to_world(float *p)
{
    float ox, oz;
    field_origin(ox, oz);
    p[0] -= ox;
    p[2] -= oz;
}

void field_to_local(float *p)
{
    float ox, oz;
    field_origin(ox, oz);
    p[0] += ox;
    p[2] += oz;
}

namespace {

constexpr int kMax = 128;
EnemySlot g_slots[kMax];
CRITICAL_SECTION g_lock;
volatile LONG g_ready = 0;

bool readable(const void *p, size_t n) { return live_readable(p, n); }

// "uEm0400" etc., or "?" when the vtable does not look like an MT Framework one.
const char *class_of(uintptr_t obj)
{
    uintptr_t vt = game::at<uintptr_t>(obj, 0);
    if (!readable(reinterpret_cast<void *>(vt + 16), 4))
        return "?";
    auto fn = reinterpret_cast<const uint8_t *>(game::at<uintptr_t>(vt, 16));
    if (!readable(fn, 6) || fn[0] != 0xB8 || fn[5] != 0xC3) // mov eax, imm32; ret
        return "?";
    uintptr_t dti;
    memcpy(&dti, fn + 1, 4);
    if (!readable(reinterpret_cast<void *>(dti + 4), 4))
        return "?";
    auto name = reinterpret_cast<const char *>(game::at<uintptr_t>(dti, 4));
    return readable(name, 16) ? name : "?";
}

// Characters of uNpc and its children (uHumanEnemy, uMultiNpc; dti.csv) by vtable, kept once seen. Human
// foes (bandits, cultists) are uNpc children, not uEnemy: the enemy sync never saw them (2026-11-08).
struct KindOf {
    volatile uintptr_t vt;
    volatile int kind; // ENEMY_*, or -1: not one
};
KindOf g_kinds[32] = {};

} // namespace

int enemies_npc_kind(uintptr_t ch)
{
    static int off = -1; // coop.ini no_npc_sync=1: monsters only, as before 0.9.7
    if (off < 0)
        off = early_flag("no_npc_sync") ? 1 : 0;
    if (off)
        return 0;
    uintptr_t vt = game::at<uintptr_t>(ch, 0);
    if (vt == game::kVtNpc)
        return ENEMY_NPC;
    if (vt == game::kVtPlayer || vt == game::kVtCmc || vt == game::kVtPlayerBase)
        return 0;
    for (const KindOf &k : g_kinds) {
        if (!k.vt)
            break;
        if (k.vt == vt)
            return k.kind > 0 ? k.kind : 0;
    }
    const char *name = class_of(ch);
    int kind = strcmp(name, "uHumanEnemy") == 0 ? ENEMY_HUMAN
               : strcmp(name, "uMultiNpc") == 0 || strcmp(name, "uNpc") == 0 ? ENEMY_NPC
                                                                              : -1;
    // several game threads: a vtable written twice is harmless; the kind is written before the vtable
    for (KindOf &k : g_kinds)
        if (!k.vt || k.vt == vt) {
            k.kind = kind;
            k.vt = vt;
            if (kind > 0)
                logf("enemy: class %s (vtable %08X) synced as %s", name, unsigned(vt), kind == ENEMY_HUMAN ? "a human foe" : "an NPC");
            break;
        }
    return kind > 0 ? kind : 0;
}

void enemies_on_finalize(uintptr_t em)
{
    if (!g_ready)
        return;
    DWORD now = GetTickCount();
    // The game finalizes enemies on up to 7 worker threads at once (2026-10-29): they queue on this lock
    // (a spinning one, enemies_install). Skipping the sync when it was busy skipped 7 of 8 enemies a frame.
    int64_t tw = perf_now();
    EnterCriticalSection(&g_lock);
    perf_add(PERF_WAIT, tw);
    int free_slot = -1, i = 0;
    uintptr_t death = 0;
    // spawns and last places are kept in world places (field_to_world): the open world shifts every place
    // by 10000 u when the Arisen crosses a cell edge, and a spawn kept as it was here then was 10000 u off
    // the host's (stage 100 repro, issue #4, 2026-11-16)
    float at[3];
    memcpy(at, reinterpret_cast<void *>(em + 0x40), sizeof at);
    field_to_world(at);
    for (; i < kMax; ++i) {
        if (g_slots[i].ptr == em && g_slots[i].vt == game::at<uintptr_t>(em, 0)) {
            // the game freed it and made another of the same class at the same address: after a gap in its
            // updates it stands far from where it was. The old slot (spawn, death) went on with the new
            // enemy, and the guest's twin jumped 3400 u (stage 100 repro, issue #4, 2026-11-16)
            EnemySlot &o = g_slots[i];
            float d[3] = {at[0] - o.last_pos[0], at[1] - o.last_pos[1], at[2] - o.last_pos[2]};
            if (o.settled && now - o.last_seen > 100 && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1000.f * 1000.f) {
                logf("enemy: #%d %s %p is a new enemy at the same address (%.0f u from its last place after %lu ms)", i, o.name,
                     (void *)em, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), now - o.last_seen);
                o.ptr = 0;
                free_slot = i;
                i = kMax;
            }
            break;
        }
        if (free_slot < 0 && (!g_slots[i].ptr || now - g_slots[i].last_seen > 3000))
            free_slot = i;
    }
    if (i == kMax && free_slot >= 0) {
        i = free_slot;
        EnemySlot &s = g_slots[i];
        if (s.ptr)
            logf("enemy: gone #%d %s %p (not updated for %lu ms)", i, s.name, (void *)s.ptr, now - s.last_seen);
        s = {};
        s.ptr = em;
        s.vt = game::at<uintptr_t>(em, 0);
        snprintf(s.name, sizeof s.name, "%s", class_of(em));
        s.kind = uint8_t(enemies_npc_kind(em));
        memcpy(s.spawn, at, sizeof at);
        s.first_seen = now;
        logf("enemy: new #%d %s %p at (%.0f, %.0f, %.0f)", i, s.name, (void *)em, s.spawn[0], s.spawn[1], s.spawn[2]);
        // a slot still holding this address belongs to a freed enemy (the address was reused)
        for (int k = 0; k < kMax; ++k)
            if (k != i && g_slots[k].ptr == em) {
                logf("enemy: gone #%d %s %p (address reused)", k, g_slots[k].name, (void *)em);
                g_slots[k] = {};
            }
    }
    if (i < kMax) {
        EnemySlot &s = g_slots[i];
        s.last_seen = now;
        memcpy(s.last_pos, at, sizeof s.last_pos);
        // the spawn point is where it stands after 1 s: some classes (uEm8100) are created at
        // another place and moved there after their first frames, at a different moment on each PC
        if (!s.settled) {
            // NPCs: the first place (they walk off at once, each game its own way: an inn walker was 20 u
            // apart at its first frame and 130 u after 1 s, inn suite 2026-11-08)
            if (now - s.first_seen < 1000) {
                if (s.kind != ENEMY_NPC)
                    memcpy(s.spawn, at, sizeof at);
            } else {
                s.settled = true;
                logf("enemy: settled #%d %s at (%.0f, %.0f, %.0f)", i, s.name, s.spawn[0], s.spawn[1], s.spawn[2]);
            }
        }
        // its spawn entry (issue #6), from its settle: once a second, 5 tries (one made by another enemy or an
        // event has none)
        if (s.settled && !s.spawn_id && s.spawn_tries < 5 && now - s.first_seen >= 1000u + 1000u * s.spawn_tries) {
            ++s.spawn_tries;
            uint32_t block, gi, kind;
            if (enemy_spawn_entry(em, block, gi, kind) && gi) {
                s.spawn_id = gi;
                logf("enemy: spawn entry #%d %s: group %X index %u (kind %X)", i, s.name, gi & 0xFFFF, gi >> 16, kind);
            }
        }
        // uNpc children have another death handler than uEnemy's (hk_death): dead = HP 0
        if (s.kind && !s.died) {
            float *hp = enemies_hp(em);
            if (hp && hp[1] > 0.f && hp[0] <= 0.f) {
                s.died = now | 1;
                logf("enemy: died #%d %s at (%.0f, %.0f, %.0f) (HP 0)", i, s.name, game::at<float>(em, 0x40),
                     game::at<float>(em, 0x44), game::at<float>(em, 0x48));
            }
        }
        int64_t tn = perf_now();
        death = enemy_net_after_finalize(g_slots[i], i);
        perf_add(PERF_NETEN, tn);
    }
    LeaveCriticalSection(&g_lock);
    if (death) // a death the guest's sync asked for, run outside the table lock
        enemy_net_death(death);
}

// [[ch+0x1D18]]+8: current HP, then max HP (null when the character has no HP entry). uPlayerBase
// copies the same value into cPlayerInfo+0x29C each frame (FUN_00B63F70), which is how it was found.
float *enemies_hp_raw(uintptr_t ch);
float *enemies_hp(uintptr_t ch)
{
    int64_t t = perf_now();
    float *r = enemies_hp_raw(ch);
    perf_add(PERF_HP, t);
    return r;
}
float *enemies_hp_raw(uintptr_t ch)
{
    if (!ch || !readable(reinterpret_cast<void *>(ch + 0x1D0C), 0x10) || !game::at<uintptr_t>(ch, 0x1D0C))
        return nullptr;
    uintptr_t list = game::at<uintptr_t>(ch, 0x1D18);
    if (!list || !readable(reinterpret_cast<void *>(list), 4))
        return nullptr;
    uintptr_t entry = game::at<uintptr_t>(list, 0);
    if (!entry || !readable(reinterpret_cast<void *>(entry + 8), 8))
        return nullptr;
    return reinterpret_cast<float *>(entry + 8);
}

bool enemies_install()
{
    // locks the game threads take each frame spin a little before sleeping: a sleep and wake-up cost them
    // milliseconds when the game's worker threads (up to 7 finalizing enemies) met there (perf:, 2026-10-29)
    InitializeCriticalSectionAndSpinCount(&g_lock, 20000);
    InterlockedExchange(&g_ready, 1);
    void *dmg = reinterpret_cast<void *>(game::kApplyDamage);
    MH_STATUS st = MH_CreateHook(dmg, reinterpret_cast<void *>(&enemies_damage_thunk), &g_damage_orig);
    if (st == MH_OK)
        st = MH_EnableHook(dmg);
    void *death = reinterpret_cast<void *>(game::kEnemyDeath);
    MH_STATUS st2 = MH_CreateHook(death, reinterpret_cast<void *>(&hk_death), reinterpret_cast<void **>(&g_death_orig));
    if (st2 == MH_OK)
        st2 = MH_EnableHook(death);
    void *hit = reinterpret_cast<void *>(game::kHitApply);
    MH_STATUS st3 = MH_CreateHook(hit, reinterpret_cast<void *>(&hk_hit_apply), reinterpret_cast<void **>(&g_hit_orig));
    if (st3 == MH_OK)
        st3 = MH_EnableHook(hit);
    logf("enemies: table ready (fed by the finalize hook); hook ApplyDamage %p: %s, EnemyDeath %p: %s, HitApply %p: %s", dmg,
         MH_StatusToString(st), death, MH_StatusToString(st2), hit, MH_StatusToString(st3));
    return st == MH_OK && st2 == MH_OK;
}

void __fastcall hk_death(uintptr_t ch, void *edx, uint32_t a, uint32_t hit)
{
    g_death_orig(ch, edx, a, hit);
    if (!g_ready)
        return;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < kMax; ++i)
        if (g_slots[i].ptr == ch && !g_slots[i].died && GetTickCount() - g_slots[i].last_seen < 1000) {
            g_slots[i].died = GetTickCount() | 1; // never 0
            logf("enemy: died #%d %s at (%.0f, %.0f, %.0f)%s", i, g_slots[i].name, game::at<float>(ch, 0x40),
                 game::at<float>(ch, 0x44), game::at<float>(ch, 0x48), hit ? "" : " (no hit info: forced)");
        }
    LeaveCriticalSection(&g_lock);
}

// Copies the live enemy table (slots not updated for 1 s are left out). Returns the count.
int enemies_snapshot(EnemySlot *out, int cap)
{
    if (!g_ready)
        return 0;
    DWORD now = GetTickCount();
    int n = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < kMax && n < cap; ++i)
        if (g_slots[i].ptr && now - g_slots[i].last_seen < 1000) {
            out[n] = g_slots[i];
            out[n].index = i;
            ++n;
        }
    LeaveCriticalSection(&g_lock);
    return n;
}

const char *enemies_class_of(uintptr_t obj) { return class_of(obj); }

// Tests only (bridge "edump <name>"): raw bytes of every live enemy, dump_<name>_e<slot>.bin, plus
// dump_<name>_enemies.txt (slot, class, address, position).
// Bridge "setinfo": for each enemy, the pointers in its object (and one level below) to the layout data it
// was made from (cSetInfo*, cLayoutSet*, cLotData): the spawn entry, the same on every PC (issue #6).
// Spawn entries (issue #6: match enemies by the game's own spawn entry, the same on every PC, instead of
// by kind and place). sSetManager (a cSystem singleton, dti 0x1B950 bytes) keeps an array of
// sSetManager::cUnitData (0x20 bytes each): +4 the character it made, +0xC the entry's kind, +0x10 its
// block, +0x14 (index in its group << 16) | group, +0x1C its cGroupParam (underground, 2026-11-17: two uEm0503
// of group 0x19 at indexes 0 and 1, uEm8100 of group 0x1A at indexes 3 and 4).
namespace {
bool strict_in_exe(uintptr_t a) { return a >= 0x00401000 && a < 0x01A00000; }
// Reads that cannot fault: ReadProcessMemory on this process fails on memory freed meanwhile, where a
// VirtualQuery check followed by a read crashed (0.10.1 Steam series, 5 games).
bool safe_read(uintptr_t a, void *out, size_t n)
{
    SIZE_T got = 0;
    return a >= 0x10000 && a + n > a &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(a), out, n, &got) && got == n;
}
uintptr_t safe_u32(uintptr_t a, bool &ok)
{
    uintptr_t v = 0;
    ok = safe_read(a, &v, 4);
    return v;
}
// the class name (dti), or nullptr; the name itself lives in the exe image (always mapped)
const char *strict_class_of(uintptr_t q)
{
    bool ok;
    if (q < 0x10000 || q >= 0x7FFF0000 || (q & 3))
        return nullptr;
    uintptr_t vt = safe_u32(q, ok);
    if (!ok || !strict_in_exe(vt))
        return nullptr;
    uintptr_t fnp = safe_u32(vt + 16, ok);
    uint8_t fn[6];
    if (!ok || !strict_in_exe(fnp) || !safe_read(fnp, fn, 6) || fn[0] != 0xB8 || fn[5] != 0xC3)
        return nullptr;
    uintptr_t dti;
    memcpy(&dti, fn + 1, 4);
    uintptr_t nm = strict_in_exe(dti) ? safe_u32(dti + 4, ok) : 0;
    return ok && strict_in_exe(nm) ? reinterpret_cast<const char *>(nm) : nullptr;
}
volatile uintptr_t g_setman = 0, g_setman_vt = 0, g_unit_vt = 0;
volatile uintptr_t g_units = 0; // first cUnitData
volatile int g_unit_count = 0;
constexpr size_t kSetManSize = 0x1B950;

// a run of cUnitData (vtable uvt) 0x20 apart from a, at most max bytes
int unit_run(uintptr_t a, uintptr_t uvt, size_t max)
{
    bool ok;
    int n = 0;
    while ((n + 1) * 0x20 <= int(max) && safe_u32(a + n * 0x20, ok) == uvt && ok)
        ++n;
    return n;
}
} // namespace

// Background thread (field_origin_thread): looks for the sSetManager among the objects the exe's globals
// point to (.data and the zeroed data after it: a static instance pointer), then its cUnitData array, inside
// it or pointed to from it.
void spawn_scan_pass()
{
    bool ok;
    uintptr_t m = g_setman;
    if (m && safe_u32(m, ok) == g_setman_vt && ok && g_units && safe_u32(g_units, ok) == g_unit_vt && ok)
        return;
    g_unit_count = 0;
    g_setman = 0;
    static uint8_t page[0x1000];
    for (uintptr_t g = 0x01600000; g < 0x019C0000; g += 0x1000) {
        if (!safe_read(g, page, sizeof page))
            continue;
        for (int o = 0; o < 0x1000; o += 4) {
            uintptr_t q;
            memcpy(&q, page + o, 4);
            if (q < 0x10000 || q >= 0x7FFF0000 || (q & 3) || strict_in_exe(q))
                continue;
            const char *c = strict_class_of(q);
            if (!c || strcmp(c, "sSetManager") != 0)
                continue;
            uintptr_t first = 0, uvt = 0;
            size_t room = 0;
            for (uintptr_t off = 4; off + 0x20 <= kSetManSize && !first; off += 4) {
                uintptr_t v = safe_u32(q + off, ok);
                if (!ok)
                    break;
                if (strict_in_exe(v)) { // inline
                    const char *uc = strict_class_of(q + off);
                    if (uc && strcmp(uc, "sSetManager::cUnitData") == 0) {
                        first = q + off;
                        uvt = v;
                        room = kSetManSize - off;
                    }
                } else if (v >= 0x10000 && v < 0x7FFF0000 && !(v & 3)) { // pointed to
                    const char *uc = strict_class_of(v);
                    if (uc && strcmp(uc, "sSetManager::cUnitData") == 0) {
                        first = v;
                        uvt = safe_u32(v, ok);
                        room = 0x100000;
                    }
                }
            }
            if (!first)
                continue;
            int n = unit_run(first, uvt, room);
            g_setman_vt = safe_u32(q, ok);
            g_unit_vt = uvt;
            g_units = first;
            g_unit_count = n;
            g_setman = q;
            logf("spawn: sSetManager %08X (global %08X), %d units at %08X", unsigned(q), unsigned(g + o), n, unsigned(first));
            return;
        }
    }
    static bool told = false;
    if (!told) {
        told = true;
        logf("spawn: no sSetManager found among the exe's globals");
    }
}

bool spawn_units(uintptr_t &units, int &count)
{
    units = g_units;
    count = g_unit_count;
    return g_setman && units && count > 0;
}

// The spawn entry an enemy was made from: block (+0x10) and (index << 16 | group) (+0x14), and the entry's
// kind (+0xC). False when it has none (made by another enemy, an event...) or the manager is not found.
bool enemy_spawn_entry(uintptr_t em, uint32_t &block, uint32_t &group_index, uint32_t &kind)
{
    uintptr_t units;
    int count;
    if (!em || !spawn_units(units, count))
        return false;
    uint32_t buf[64 * 8]; // 64 units a read (the array may be freed with its stage: reads that cannot fault)
    for (int k = 0; k < count; k += 64) {
        int m = count - k < 64 ? count - k : 64;
        if (!safe_read(units + k * 0x20, buf, m * 0x20))
            return false;
        for (int j = 0; j < m; ++j) {
            const uint32_t *u = buf + j * 8;
            if (u[0] == g_unit_vt && u[1] == em) {
                kind = u[3];
                block = u[4];
                group_index = u[5];
                return true;
            }
        }
    }
    return false;
}

// Bridge "units": the game's sUnit (every live unit, in move lines) found among the exe's globals, its
// fields, and for each pointer to a sUnit::MoveLine the classes of what it holds (objects: uOm*; issue #4,
// carried crates and barrels).
// The game's sUnit (sUnitExt; the static pointer at 018D09E0 on Steam): from +0x20, sUnit::MoveLine entries
// 0x18 bytes apart (+0xC the first unit of the line, +0x10 the last); each unit's +0xC is the next one of its
// line (units scan, Pawn Guild, 2026-11-17: objects uOmObj8530/8505/7010 in the line at +0x1B8).
namespace {
volatile uintptr_t g_sunit = 0, g_sunit_vt = 0, g_line_vt = 0;
}
bool sunit_find()
{
    bool ok;
    if (g_sunit && safe_u32(g_sunit, ok) == g_sunit_vt && ok)
        return true;
    static uint8_t page[0x1000];
    for (uintptr_t g = 0x01600000; g < 0x019C0000; g += 0x1000) {
        if (!safe_read(g, page, sizeof page))
            continue;
        for (int o = 0; o < 0x1000; o += 4) {
            uintptr_t q;
            memcpy(&q, page + o, 4);
            if (q < 0x10000 || q >= 0x7FFF0000 || (q & 3) || strict_in_exe(q))
                continue;
            const char *c = strict_class_of(q);
            if (!c || (strcmp(c, "sUnit") != 0 && strcmp(c, "sUnitExt") != 0))
                continue;
            const char *lc = strict_class_of(q + 0x20);
            if (!lc || strcmp(lc, "sUnit::MoveLine") != 0)
                continue;
            g_line_vt = safe_u32(q + 0x20, ok);
            g_sunit_vt = safe_u32(q, ok);
            g_sunit = q;
            logf("units: sUnit %08X (global %08X)", unsigned(q), unsigned(g + o));
            return true;
        }
    }
    return false;
}

// Calls f(unit, line index) for every unit of every move line; stops early when f returns false.
template <class F> void units_each(F f)
{
    if (!sunit_find())
        return;
    bool ok;
    for (int k = 0; k < 64; ++k) {
        uintptr_t line = g_sunit + 0x20 + k * 0x18;
        if (safe_u32(line, ok) != g_line_vt || !ok)
            break;
        uintptr_t u = safe_u32(line + 0xC, ok);
        for (int n = 0; ok && u && n < 4000; ++n) {
            if (!f(u, k))
                return;
            u = safe_u32(u + 0xC, ok);
        }
    }
}

// Bridge "units": every move line's units by class, and the objects (uOm*) with their places (issue #4,
// carried crates and barrels).
void units_scan()
{
    struct Count {
        const char *c;
        int line, n;
    };
    static Count counts[256];
    int nc = 0, objs = 0;
    units_each([&](uintptr_t u, int line) {
        const char *c = strict_class_of(u);
        if (!c)
            return true;
        int j = 0;
        while (j < nc && !(counts[j].c == c && counts[j].line == line))
            ++j;
        if (j == nc && nc < 256)
            counts[nc++] = {c, line, 0};
        if (j < nc)
            ++counts[j].n;
        if (strncmp(c, "uOm", 3) == 0 && objs < 120) {
            ++objs;
            float p[3] = {};
            safe_read(u + 0x40, p, sizeof p);
            logf("units: line %d %s %08X at (%.0f, %.0f, %.0f)", line, c, unsigned(u), p[0], p[1], p[2]);
        }
        return true;
    });
    for (int j = 0; j < nc; ++j)
        logf("units: line %d: %d %s", counts[j].line, counts[j].n, counts[j].c);
}

// Bridge "setinfo": each enemy's spawn entry, then every unit of the manager (class, entry, place).
void enemies_setinfo_scan()
{
    {
        uintptr_t units;
        int count;
        static uint32_t buf[0x20 / 4];
        if (spawn_units(units, count))
            for (int k = 0; k < count; ++k) {
                if (!safe_read(units + k * 0x20, buf, 0x20) || !buf[1])
                    continue;
                const char *c = strict_class_of(buf[1]);
                float p[3] = {};
                safe_read(buf[1] + 0x40, p, sizeof p);
                logf("setinfo: unit %d %s kind %X block %X group %X index %u at (%.0f, %.0f, %.0f) +18 %08X", k, c ? c : "?",
                     buf[3], buf[4], buf[5] & 0xFFFF, buf[5] >> 16, p[0], p[1], p[2], buf[6]);
            }
    }
    EnemySlot list[kMax];
    int n = enemies_snapshot(list, kMax);
    for (int k = 0; k < n; ++k) {
        uint32_t b = 0, gi = 0, kd = 0;
        bool ok = enemy_spawn_entry(list[k].ptr, b, gi, kd);
        float p[3];
        memcpy(p, reinterpret_cast<const void *>(list[k].ptr + 0x40), sizeof p);
        field_to_world(p);
        logf("setinfo: #%d %s at (%.0f, %.0f, %.0f) spawn (%.0f, %.0f, %.0f): %s kind %X block %X group %X index %u",
             list[k].index, list[k].name, p[0], p[1], p[2], list[k].spawn[0], list[k].spawn[1], list[k].spawn[2],
             ok ? "entry" : "no entry", kd, b, gi & 0xFFFF, gi >> 16);
    }
}

void enemies_dump(const char *name)
{
    EnemySlot list[kMax];
    int n = enemies_snapshot(list, kMax);
    char path[128];
    _snprintf(path, sizeof path, "dump_%s_enemies.txt", name);
    FILE *idx = fopen(path, "w");
    for (int k = 0; k < n; ++k) {
        const EnemySlot &s = list[k];
        size_t size = 0x8000;
        while (size > 0x1000 && !readable(reinterpret_cast<void *>(s.ptr), size))
            size -= 0x1000;
        _snprintf(path, sizeof path, "dump_%s_e%d.bin", name, s.index);
        if (FILE *f = fopen(path, "wb")) {
            fwrite(reinterpret_cast<const void *>(s.ptr), 1, size, f);
            fclose(f);
        }
        float p[3]; // world places: the two copies may stand in different cells of the open world (issue #4)
        memcpy(p, reinterpret_cast<const void *>(s.ptr + 0x40), sizeof p);
        field_to_world(p);
        // HP candidate, as uPlayerBase copies it into cPlayerInfo+0x29C (FUN_00B63F70): [[ch+0x1D18]]+8
        // when ch+0x1D0C is set; the 8 floats from there are logged for comparison
        float hp[8] = {};
        uintptr_t list = game::at<uintptr_t>(s.ptr, 0x1D18), entry = 0;
        if (game::at<uintptr_t>(s.ptr, 0x1D0C) && readable(reinterpret_cast<void *>(list), 4))
            entry = game::at<uintptr_t>(list, 0);
        if (entry && readable(reinterpret_cast<void *>(entry), 0x28))
            memcpy(hp, reinterpret_cast<void *>(entry + 8), sizeof hp);
        if (idx)
            fprintf(idx, "%d %s %08X %.1f %.1f %.1f size %X hp@%08X %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f follows %d dead %d\n",
                    s.index, s.name, unsigned(s.ptr), p[0], p[1], p[2], unsigned(size), unsigned(entry), hp[0], hp[1],
                    hp[2], hp[3], hp[4], hp[5], hp[6], hp[7], enemy_net_follow(s.index), s.died ? 1 : 0);
    }
    if (idx)
        fclose(idx);
    logf("enemies: dumped %d enemies as %s", n, name);
}

// For objects.cpp: reads that cannot fault, class names, and every unit of the move lines.
bool mem_safe_read(uintptr_t a, void *out, size_t n) { return safe_read(a, out, n); }
const char *enemies_strict_class_of(uintptr_t q) { return strict_class_of(q); }
void units_each_raw(bool (*f)(uintptr_t unit, void *ctx), void *ctx)
{
    units_each([&](uintptr_t u, int) { return f(u, ctx); });
}

// Bridge "placename" (tests, Discord presence): the GUI units that show the area's name (uGUIPlaceName,
// uGUIMiniMap) and the text they hold or point to (UTF-8 / UTF-16 runs of letters).
void placename_scan()
{
    auto text_at = [](uintptr_t a, char *out, size_t cap) -> bool {
        uint8_t b[96];
        if (!safe_read(a, b, sizeof b))
            return false;
        size_t n = 0; // UTF-8 / ASCII letters
        while (n < sizeof b && n + 1 < cap && ((b[n] >= 0x20 && b[n] < 0x7F) || b[n] >= 0xC0))
            out[n] = char(b[n]), ++n;
        out[n] = 0;
        if (n >= 5)
            return true;
        n = 0; // UTF-16
        while (2 * n + 1 < sizeof b && n + 1 < cap && b[2 * n] >= 0x20 && b[2 * n] < 0x7F && b[2 * n + 1] == 0)
            out[n] = char(b[2 * n]), ++n;
        out[n] = 0;
        return n >= 5;
    };
    units_each([&](uintptr_t u, int line) {
        const char *c = strict_class_of(u);
        if (!c || (!strstr(c, "PlaceName") && !strstr(c, "MiniMap") && !strstr(c, "uGUIMap")))
            return true;
        logf("placename: line %d %s %08X", line, c, unsigned(u));
        char s[100];
        for (int off = 0; off < 0x1000; off += 4) {
            if (text_at(u + off, s, sizeof s))
                logf("placename:   +%X inline \"%s\"", off, s);
            bool ok;
            uintptr_t p = safe_u32(u + off, ok);
            if (ok && p >= 0x10000 && p < 0x7FFF0000 && !strict_in_exe(p) && text_at(p, s, sizeof s))
                logf("placename:   +%X -> %08X \"%s\"", off, unsigned(p), s);
        }
        return true;
    });
}
