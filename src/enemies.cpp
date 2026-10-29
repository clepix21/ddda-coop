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

extern "C" bool __cdecl enemies_damage_filter(uintptr_t entry, float *dmg)
{
    uintptr_t ch = game::at<uintptr_t>(entry, 0x1B4);
    if (ch && revive_on_damage(ch, dmg)) // shared world: down instead of dead (revive.cpp)
        return true;
    return ch && enemy_net_block_damage(ch);
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
    uintptr_t entry = game::at<uintptr_t>(game::at<uintptr_t>(arisen, 0x1D18), 0);
    uintptr_t fn = game::kApplyDamage;
    uint32_t b = uint32_t(bits);
    float dmg;
    memcpy(&dmg, &b, 4);
    logf("enemies: test damage %.1f to the Arisen (HP entry %08X)", dmg, unsigned(entry));
    // EAX = HP entry, push flag then damage; the callee pops both
    asm volatile("pushl $0\n\t"
                 "pushl %%edx\n\t"
                 "call *%%ecx\n\t"
                 : "+a"(entry), "+c"(fn), "+d"(b)
                 :
                 : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
}

// Committed, readable memory. The regions VirtualQuery gave are kept 250 ms per thread: it takes the
// process's address-space lock, and the game's worker threads calling it each frame for every enemy
// (enemies_hp: three checks) spent 0.5-0.8 ms per call there (perf:, 2026-10-29).
bool mem_readable(const void *p, size_t n)
{
    struct Region {
        uintptr_t lo, hi;
        DWORD until;
    };
    static thread_local Region kept[8] = {};
    static thread_local int next = 0;
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
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

namespace {

constexpr int kMax = 128;
EnemySlot g_slots[kMax];
CRITICAL_SECTION g_lock;
volatile LONG g_ready = 0;

bool readable(const void *p, size_t n) { return mem_readable(p, n); }

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

} // namespace

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
    for (; i < kMax; ++i) {
        if (g_slots[i].ptr == em && g_slots[i].vt == game::at<uintptr_t>(em, 0))
            break;
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
        memcpy(s.spawn, reinterpret_cast<void *>(em + 0x40), sizeof s.spawn);
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
        // the spawn point is where it stands after 1 s: some classes (uEm8100) are created at
        // another place and moved there after their first frames, at a different moment on each PC
        if (!s.settled) {
            const float *p = reinterpret_cast<const float *>(em + 0x40);
            if (now - s.first_seen < 1000) {
                memcpy(s.spawn, p, sizeof s.spawn);
            } else {
                s.settled = true;
                logf("enemy: settled #%d %s at (%.0f, %.0f, %.0f)", i, s.name, s.spawn[0], s.spawn[1], s.spawn[2]);
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
    logf("enemies: table ready (fed by the finalize hook); hook ApplyDamage %p: %s, EnemyDeath %p: %s", dmg,
         MH_StatusToString(st), death, MH_StatusToString(st2));
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
        const float *p = reinterpret_cast<const float *>(s.ptr + 0x40);
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
