// Steam Cloud storage hooks. Hooks steam_api!SteamRemoteStorage and patches the returned
// interface's vtable (ISteamRemoteStorage v012, MSVC thiscall).
//  - Save guard (block_saves=1, and always for role=guest): every write is blocked.
//  - The host keeps a copy of its latest save, DDDA.sav (the one the game read, then each write,
//    autosaves included, blocked or not) for the guest (net.cpp sends it).
//  - Guest: once the host's save has arrived, the game's reads of DDDA.sav get the host's save
//    instead of the guest's own, so "Charger" loads the host's world (characters, pawns, quests,
//    items, time). The game reads the whole file with FileExists / GetFileSize / FileRead, at boot
//    and when the load menu opens (seen 2026-10-26).
//  - 2 to 4 players: each guest's own character (from its own save, read once at its first read)
//    replaces the pawn it drives. The host merges those characters into its save (savecodec.cpp);
//    its game and every guest then load that merged save. Each host write is un-merged first (its
//    real pawns put back), so the host's save never keeps the guests' characters.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "MinHook.h"
#include "mod.h"

namespace {

// ISteamRemoteStorage v012 slot order (Steamworks SDK isteamremotestorage.h of that version).
enum Slot { FileWrite = 0, FileRead, FileForget, FileDelete, FileShare, SetSyncPlatforms,
            FileWriteStreamOpen, FileWriteStreamWriteChunk, FileWriteStreamClose,
            FileWriteStreamCancel, FileExists, FilePersisted, GetFileSize, GetFileTimestamp,
            GetSyncPlatforms, GetFileCount, GetFileNameAndSize };
void *g_orig_slot[GetFileNameAndSize + 1] = {}; // the interface's own functions, before the patch
bool g_block = false;                            // block writes (save guard)
CRITICAL_SECTION g_save_lock;
std::vector<uint8_t> g_own_save;   // host: its latest DDDA.sav
uint32_t g_own_version = 0;
bool g_own_written = false;        // g_own_save came from a write (blocked or not)
std::vector<uint8_t> g_peer_save;  // guest: the host's DDDA.sav, once complete
uint32_t g_peer_version = 0;
uint32_t g_peer_sig = 0, g_peer_read_sig = 0; // guest: sig of the host's save it has / its game read
bool g_peer_read_any = false;
uint32_t g_merged_sig = 0, g_loaded_sig = 0;  // host: sig of the merged save / of the save the game read
uint32_t g_merged_crc[kPawns] = {}, g_loaded_crc[kPawns] = {}; // the same, per pawn (0 = the host's own)
std::vector<uint8_t> g_guest_own;  // guest: its own DDDA.sav (its character), read once
std::vector<uint8_t> g_roam_save;  // guest exploring on its own (roam.cpp): served instead of the host's
std::vector<uint8_t> g_shared_save; // shared world: the host's save as this guest's own world
std::vector<uint8_t> g_override_save; // shared world, quest_sync.cpp: served to the next load, any role
uint32_t g_shared_from = 0;         // 1 + the host save version it was made from (0 = none)

// Host, 2 to 4 players: the guests' characters by pawn, and the merged save built from them.
std::string g_party_char[kPawns];  // empty = that pawn stays the host's
bool g_party_dirty = false;
std::vector<uint8_t> g_merged;     // empty = no guest character merged
std::string g_orig_char[kPawns];   // the host's own pawn blocks the merge replaced (for un-merging)
// the same, for the save the game last read: what its memory (and so its writes) holds. Kept after the
// guests leave and the merged save is gone, until the game reads a save again.
std::string g_loaded_orig[kPawns];
uint32_t g_merged_from = 0;        // g_own_version the merged save was built from
uint32_t g_serve_version = 0;      // +1 whenever what the guests should load changes

bool is_save(const char *name) { return name && _stricmp(name, "DDDA.sav") == 0; }

// Return addresses inside DDDA.exe on the current stack (who reads / writes the save), for logs.
void log_callers(const char *what, void *ret, void *frame)
{
    if (!cfg_debug_log())
        return;
    // every stack value that points into executable code of a loaded module, with the module name
    char buf[600];
    int len = 0, found = 0;
    len += _snprintf(buf, sizeof buf, " ret %08X, frame %p:", unsigned(uintptr_t(ret)), frame);
    const uintptr_t *sp = reinterpret_cast<const uintptr_t *>(frame);
    for (int i = 0; i < 2048 && found < 16 && mem_readable(sp + i, 4); ++i) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<void *>(sp[i]), &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE ||
            !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE)))
            continue;
        char mod[MAX_PATH] = "?";
        GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), mod, sizeof mod);
        const char *base = strrchr(mod, '\\');
        len += _snprintf(buf + len, sizeof buf - len, " %s+%X", base ? base + 1 : mod,
                         unsigned(sp[i] - uintptr_t(mbi.AllocationBase)));
        if (len >= int(sizeof buf)) { len = sizeof buf - 1; break; }
        ++found;
    }
    logf("storage: %s called from%s", what, len ? buf : " (none)");
}

// Tests (writes blocked): coop.ini test_save=<file> makes the game read that file as DDDA.sav, so bot
// routes start from a known place whatever the owner's own save says. Read once, kept in memory.
std::vector<uint8_t> g_test_save;
bool g_test_save_tried = false;

bool test_save(std::vector<uint8_t> &out)
{
    if (!g_block)
        return false;
    EnterCriticalSection(&g_save_lock);
    if (!g_test_save_tried) {
        g_test_save_tried = true;
        char path[MAX_PATH] = {};
        if (cfg_str("test_save", path, sizeof path) && path[0]) {
            if (FILE *f = fopen(path, "rb")) {
                fseek(f, 0, SEEK_END);
                long n = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (n > 0 && n <= (4 << 20)) {
                    g_test_save.resize(size_t(n));
                    if (fread(g_test_save.data(), 1, size_t(n), f) != size_t(n))
                        g_test_save.clear();
                }
                fclose(f);
            }
            logf("storage: test_save %s: %d bytes", path, int(g_test_save.size()));
        }
    }
    bool have = !g_test_save.empty();
    if (have)
        out = g_test_save;
    LeaveCriticalSection(&g_save_lock);
    return have;
}

void keep_own(const void *data, int32_t size, const char *why, bool written)
{
    if (!data || size <= 0)
        return;
    EnterCriticalSection(&g_save_lock);
    g_own_written |= written;
    bool same = g_own_save.size() == size_t(size) && memcmp(g_own_save.data(), data, size_t(size)) == 0;
    if (!same) {
        g_own_save.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
        ++g_own_version;
        ++g_serve_version;
        logf("storage: kept DDDA.sav from %s (%d bytes, version %u)", why, size, g_own_version);
    }
    LeaveCriticalSection(&g_save_lock);
}

using SteamRemoteStorage_t = void *(__cdecl *)();
SteamRemoteStorage_t g_orig_get = nullptr;
volatile LONG g_patched = 0;
volatile LONG g_blocked = 0;

// A host write of a merged save: the host's own pawns put back (the guests' characters are never
// kept in the host's save). Returns false when nothing was merged or the save could not be read.
bool unmerge(const void *data, int32_t size, std::vector<uint8_t> &out)
{
    std::string orig[kPawns];
    EnterCriticalSection(&g_save_lock);
    bool merged = false;
    for (int p = 0; p < kPawns; ++p) {
        orig[p] = g_loaded_orig[p];
        merged |= !orig[p].empty();
    }
    LeaveCriticalSection(&g_save_lock);
    if (!merged)
        return false;
    std::vector<uint8_t> file(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
    std::string xml;
    uint32_t head[8];
    if (!save_unpack(file, xml, head)) {
        logf("storage: a merged save written but not readable: written as is");
        return false;
    }
    progress_host_write(xml); // what the guests' characters gained (progress.cpp)
    int put = 0;
    for (int p = 0; p < kPawns; ++p)
        if (!orig[p].empty() && save_put_character(xml, p, orig[p]))
            ++put;
    if (!save_pack(xml, head, out))
        return false;
    logf("storage: write un-merged: %d pawn(s) of the host's own put back", put);
    return true;
}

bool __attribute__((thiscall)) hk_file_write(void *self, const char *name, const void *data, int32_t size)
{
    if (is_save(name))
        log_callers("FileWrite(DDDA.sav)", __builtin_return_address(0), __builtin_frame_address(0));
    char dump[8] = "";
    if (is_save(name) && g_block && cfg_str("dump_writes", dump, sizeof dump) && dump[0] == '1') {
        // tests: each write as the game made it (before un-merging), in the game folder
        static int n = 0;
        char path[32];
        _snprintf(path, sizeof path, "write_%d.sav", ++n);
        if (FILE *f = fopen(path, "wb")) {
            fwrite(data, 1, size_t(size), f);
            fclose(f);
            logf("storage: write dumped to %s", path);
        }
    }
    std::vector<uint8_t> clean;
    if (is_save(name) && cfg_role() != NET_GUEST && unmerge(data, size, clean)) {
        data = clean.data();
        size = int32_t(clean.size());
    }
    if (is_save(name))
        keep_own(data, size, "a write", true);
    if (!g_block) {
        using F = bool(__attribute__((thiscall)) *)(void *, const char *, const void *, int32_t);
        bool ok = reinterpret_cast<F>(g_orig_slot[FileWrite])(self, name, data, size);
        if (ok && is_save(name) && cfg_role() != NET_GUEST)
            progress_after_write();
        return ok;
    }
    InterlockedIncrement(&g_blocked);
    logf("save-guard: BLOCKED FileWrite(\"%s\", %d bytes)", name ? name : "?", size);
    if (is_save(name) && cfg_role() != NET_GUEST)
        progress_after_write(); // tests: the game believes it saved
    return true; // pretend success
}

constexpr uint32_t kRoamVersion = 0xFFFFFFFFu; // peer_save's version for the free roam save

// Guest with the host's save: the bytes the game gets for DDDA.sav (false = use the real file)
bool peer_save(std::vector<uint8_t> &out, uint32_t &version)
{
    if (cfg_role() != NET_GUEST)
        return false;
    EnterCriticalSection(&g_save_lock);
    std::vector<uint8_t> raw;
    uint32_t raw_version = g_peer_version;
    bool build = net_shared() && !g_peer_save.empty() && g_shared_from != g_peer_version + 1;
    if (build)
        raw = g_peer_save;
    LeaveCriticalSection(&g_save_lock);
    if (build) { // shared world: this guest's own world from the host's save (once per host save)
        std::vector<uint8_t> mine;
        if (roam_world_from_host(raw, mine)) {
            EnterCriticalSection(&g_save_lock);
            g_shared_save.swap(mine);
            g_shared_from = raw_version + 1;
            LeaveCriticalSection(&g_save_lock);
        }
    }
    EnterCriticalSection(&g_save_lock);
    if (net_shared() && g_shared_from == g_peer_version + 1 && !g_shared_save.empty()) {
        out = g_shared_save;
        version = g_peer_version;
        LeaveCriticalSection(&g_save_lock);
        return true;
    }
    bool roam = !g_roam_save.empty();
    bool have = roam || !g_peer_save.empty();
    if (have) {
        out = roam ? g_roam_save : g_peer_save;
        version = roam ? kRoamVersion : g_peer_version;
    }
    LeaveCriticalSection(&g_save_lock);
    return have;
}

bool __attribute__((thiscall)) hk_file_forget(void *, const char *name)
{
    InterlockedIncrement(&g_blocked);
    logf("save-guard: BLOCKED FileForget(\"%s\")", name ? name : "?");
    return true;
}

bool __attribute__((thiscall)) hk_file_delete(void *, const char *name)
{
    InterlockedIncrement(&g_blocked);
    logf("save-guard: BLOCKED FileDelete(\"%s\")", name ? name : "?");
    return true;
}

uint64_t __attribute__((thiscall)) hk_stream_open(void *, const char *name)
{
    InterlockedIncrement(&g_blocked);
    logf("save-guard: BLOCKED FileWriteStreamOpen(\"%s\")", name ? name : "?");
    return 0xFFFFFFFFFFFFFFFFull; // k_UGCFileStreamHandleInvalid
}

bool __attribute__((thiscall)) hk_stream_false(void *, uint64_t)
{
    logf("save-guard: BLOCKED FileWriteStream call");
    return false;
}

// The save/load machine [0x018FA518] runs a save (modes 2, 7, 0xF: FUN_004B8280 -> FUN_004B8DF0).
uint8_t machine_mode()
{
    uintptr_t sm = *reinterpret_cast<uintptr_t *>(0x018FA518);
    return sm ? *reinterpret_cast<uint8_t *>(sm + 0x648) : 0;
}
bool saving()
{
    uint8_t mode = machine_mode();
    return mode == 2 || mode == 7 || mode == 0xF;
}
// A read that loads the save into the game (modes 3, 5, B, C, 0x12: FUN_004B8280 restores the
// party from it). Other reads (modes 6, D, 0x13 at boot and at the title, the read during a stage
// jump on a guest) do not change what the world shows.
bool loading()
{
    uint8_t mode = machine_mode();
    return mode == 3 || mode == 5 || mode == 0xB || mode == 0xC || mode == 0x12;
}

// Reads, logged (milestone 6: how the game loads its save)
int32_t __attribute__((thiscall)) hk_file_read(void *self, const char *name, void *data, int32_t cap)
{
    if (is_save(name))
        log_callers("FileRead(DDDA.sav)", __builtin_return_address(0), __builtin_frame_address(0));
    std::vector<uint8_t> host;
    uint32_t version = 0;
    if (is_save(name) && !saving()) { // quest_sync.cpp: this world's snapshot with another's quest state
        EnterCriticalSection(&g_save_lock);
        int32_t n = g_override_save.empty() ? -1 : (int32_t(g_override_save.size()) < cap ? int32_t(g_override_save.size()) : cap);
        if (n >= 0)
            memcpy(data, g_override_save.data(), size_t(n));
        LeaveCriticalSection(&g_save_lock);
        if (n >= 0) {
            logf("storage: FileRead(\"%s\", cap %d) = %d: this world with another player's quest state", name, cap, n);
            return n;
        }
    }
    if (is_save(name) && cfg_role() == NET_GUEST && g_guest_own.empty()) {
        // the guest's own save, once: its character goes to the host (test_save in tests)
        std::vector<uint8_t> own(size_t(cap > 0 ? cap : 0));
        int32_t n = test_save(own) ? int32_t(own.size())
                                   : reinterpret_cast<int32_t(__attribute__((thiscall)) *)(void *, const char *, void *, int32_t)>(
                                         g_orig_slot[FileRead])(self, name, own.data(), cap);
        if (n > 0) {
            own.resize(size_t(n));
            EnterCriticalSection(&g_save_lock);
            g_guest_own.swap(own);
            LeaveCriticalSection(&g_save_lock);
            logf("storage: the guest's own save read (%d bytes): its character goes to the host", n);
        }
    }
    if (is_save(name) && cfg_role() == NET_GUEST && saving()) {
        // a guest's own save request (free roam, roam.cpp): the check read gets what it just wrote
        EnterCriticalSection(&g_save_lock);
        int32_t n = g_own_written ? (int32_t(g_own_save.size()) < cap ? int32_t(g_own_save.size()) : cap) : -1;
        if (n >= 0)
            memcpy(data, g_own_save.data(), size_t(n));
        LeaveCriticalSection(&g_save_lock);
        if (n >= 0) {
            logf("storage: FileRead(\"%s\", cap %d) = %d: the guest's last blocked write", name, cap, n);
            return n;
        }
    }
    if (is_save(name) && peer_save(host, version)) {
        int32_t n = int32_t(host.size()) < cap ? int32_t(host.size()) : cap;
        memcpy(data, host.data(), size_t(n));
        if (version == kRoamVersion) {
            logf("storage: FileRead(\"%s\", cap %d) = %d: the free roam save", name, cap, n);
            return n;
        }
        if (loading()) {
            EnterCriticalSection(&g_save_lock);
            g_peer_read_sig = g_peer_sig;
            g_peer_read_any = true;
            LeaveCriticalSection(&g_save_lock);
        }
        logf("storage: FileRead(\"%s\", cap %d) = %d: the host's save (version %u)", name, cap, n, version);
        return n;
    }
    // a save in progress reads the file before and after writing it (a check): that is not a load,
    // so it gets the host's own save as written, never the merged one
    if (is_save(name) && cfg_role() != NET_GUEST && !saving()) { // host with guests' characters: the merged save
        EnterCriticalSection(&g_save_lock);
        int32_t n = g_merged.empty() ? -1 : (int32_t(g_merged.size()) < cap ? int32_t(g_merged.size()) : cap);
        if (n >= 0)
            memcpy(data, g_merged.data(), size_t(n));
        std::vector<uint8_t> loaded;
        if (loading()) {
            g_loaded_sig = n >= 0 ? g_merged_sig : 0; // the host's own save otherwise (below)
            for (int p = 0; p < kPawns; ++p) {
                g_loaded_orig[p] = n >= 0 ? g_orig_char[p] : std::string();
                g_loaded_crc[p] = n >= 0 ? g_merged_crc[p] : 0;
            }
            if (n >= 0)
                loaded = g_merged;
        }
        uint32_t crcs[kPawns];
        memcpy(crcs, g_loaded_crc, sizeof crcs);
        LeaveCriticalSection(&g_save_lock);
        if (!loaded.empty())
            progress_host_loaded(loaded, crcs); // the guests' progress counts from here
        if (n >= 0) {
            logf("storage: FileRead(\"%s\", cap %d) = %d: the merged save (guests' characters as pawns)", name, cap, n);
            return n;
        }
    }
    // writes blocked: the game reads back what it last wrote (it reads the save right after each
    // autosave; the file on disk is still the old one, which would also become the "latest" save)
    if (is_save(name) && g_block && cfg_role() != NET_GUEST) {
        EnterCriticalSection(&g_save_lock);
        int32_t n = g_own_written ? (int32_t(g_own_save.size()) < cap ? int32_t(g_own_save.size()) : cap) : -1;
        if (n >= 0)
            memcpy(data, g_own_save.data(), size_t(n));
        LeaveCriticalSection(&g_save_lock);
        if (n >= 0) {
            logf("storage: FileRead(\"%s\", cap %d) = %d: the last blocked write", name, cap, n);
            return n;
        }
    }
    std::vector<uint8_t> test;
    if (is_save(name) && test_save(test)) {
        if (cfg_role() != NET_GUEST)
            progress_patch(test, loading()); // keep_progress=1: a guest's earlier gains (progress.cpp)
        int32_t n = int32_t(test.size()) < cap ? int32_t(test.size()) : cap;
        memcpy(data, test.data(), size_t(n));
        logf("storage: FileRead(\"%s\", cap %d) = %d: test_save", name, cap, n);
        if (cfg_role() != NET_GUEST)
            keep_own(data, n, "the game's read", false);
        return n;
    }
    using F = int32_t(__attribute__((thiscall)) *)(void *, const char *, void *, int32_t);
    int32_t n = reinterpret_cast<F>(g_orig_slot[FileRead])(self, name, data, cap);
    if (is_save(name) && n > 0 && cfg_role() != NET_GUEST) {
        std::vector<uint8_t> v(static_cast<uint8_t *>(data), static_cast<uint8_t *>(data) + n);
        if (progress_patch(v, loading()) && int32_t(v.size()) <= cap) { // keep_progress=1 (progress.cpp)
            memcpy(data, v.data(), v.size());
            n = int32_t(v.size());
        }
    }
    logf("storage: FileRead(\"%s\", cap %d) = %d%s", name ? name : "?", cap, n,
         is_save(name) && cfg_role() == NET_GUEST ? ": the guest's own save (the host's has not arrived)" : "");
    if (is_save(name) && n > 0 && cfg_role() != NET_GUEST)
        keep_own(data, n, "the game's read", false);
    return n;
}

int32_t __attribute__((thiscall)) hk_get_file_size(void *self, const char *name)
{
    std::vector<uint8_t> host;
    uint32_t version = 0;
    if (is_save(name) && peer_save(host, version)) {
        logf("storage: GetFileSize(\"%s\") = %d: the host's save", name, int(host.size()));
        return int32_t(host.size());
    }
    if (is_save(name) && cfg_role() != NET_GUEST) {
        EnterCriticalSection(&g_save_lock);
        int32_t n = g_merged.empty() ? -1 : int32_t(g_merged.size());
        LeaveCriticalSection(&g_save_lock);
        if (n >= 0)
            return n;
    }
    std::vector<uint8_t> test;
    if (is_save(name) && test_save(test))
        return int32_t(test.size());
    using F = int32_t(__attribute__((thiscall)) *)(void *, const char *);
    int32_t n = reinterpret_cast<F>(g_orig_slot[GetFileSize])(self, name);
    logf("storage: GetFileSize(\"%s\") = %d", name ? name : "?", n);
    return n;
}

bool __attribute__((thiscall)) hk_file_exists(void *self, const char *name)
{
    std::vector<uint8_t> host;
    uint32_t version = 0;
    if (is_save(name) && peer_save(host, version)) {
        logf("storage: FileExists(\"%s\") = 1: the host's save", name);
        return true;
    }
    std::vector<uint8_t> test;
    if (is_save(name) && test_save(test))
        return true;
    using F = bool(__attribute__((thiscall)) *)(void *, const char *);
    bool r = reinterpret_cast<F>(g_orig_slot[FileExists])(self, name);
    logf("storage: FileExists(\"%s\") = %d", name ? name : "?", r);
    return r;
}

int32_t __attribute__((thiscall)) hk_get_file_count(void *self)
{
    using F = int32_t(__attribute__((thiscall)) *)(void *);
    int32_t n = reinterpret_cast<F>(g_orig_slot[GetFileCount])(self);
    logf("storage: GetFileCount() = %d", n);
    return n;
}

const char *__attribute__((thiscall)) hk_get_file_name_and_size(void *self, int i, int32_t *size)
{
    using F = const char *(__attribute__((thiscall)) *)(void *, int, int32_t *);
    const char *r = reinterpret_cast<F>(g_orig_slot[GetFileNameAndSize])(self, i, size);
    logf("storage: GetFileNameAndSize(%d) = \"%s\", %d", i, r ? r : "?", size ? *size : -1);
    return r;
}

void patch_vtable(void *iface)
{
    void **vt = *reinterpret_cast<void ***>(iface);
    DWORD old;
    if (!VirtualProtect(vt, sizeof(void *) * (GetFileNameAndSize + 1), PAGE_READWRITE, &old)) {
        logf("save-guard: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    for (int i = 0; i <= GetFileNameAndSize; ++i)
        g_orig_slot[i] = vt[i];
    vt[FileRead] = reinterpret_cast<void *>(&hk_file_read);
    vt[GetFileSize] = reinterpret_cast<void *>(&hk_get_file_size);
    vt[FileExists] = reinterpret_cast<void *>(&hk_file_exists);
    vt[GetFileCount] = reinterpret_cast<void *>(&hk_get_file_count);
    vt[GetFileNameAndSize] = reinterpret_cast<void *>(&hk_get_file_name_and_size);
    vt[FileWrite] = reinterpret_cast<void *>(&hk_file_write);
    if (g_block) {
        vt[FileForget] = reinterpret_cast<void *>(&hk_file_forget);
        vt[FileDelete] = reinterpret_cast<void *>(&hk_file_delete);
        vt[FileWriteStreamOpen] = reinterpret_cast<void *>(&hk_stream_open);
        vt[FileWriteStreamClose] = reinterpret_cast<void *>(&hk_stream_false);
    }
    VirtualProtect(vt, sizeof(void *) * (GetFileNameAndSize + 1), old, &old);
    InterlockedExchange(&g_patched, 1);
    if (g_block)
        logf("save-guard: ACTIVE, ISteamRemoteStorage %p vtable %p patched", iface, vt);
    else
        logf("storage: ISteamRemoteStorage %p vtable %p patched (saves allowed)", iface, vt);
}

void *__cdecl hk_get_remote_storage()
{
    void *iface = g_orig_get();
    if (iface && !g_patched)
        patch_vtable(iface);
    return iface;
}

} // namespace

bool save_guard_install(bool block)
{
    g_block = block;
    InitializeCriticalSection(&g_save_lock);
    HMODULE steam = nullptr;
    for (int i = 0; i < 500 && !steam; ++i) { // wait up to 5 s for the static import
        steam = GetModuleHandleA("steam_api.dll");
        if (!steam)
            Sleep(10);
    }
    if (!steam) {
        logf("save-guard: steam_api.dll not loaded");
        return false;
    }
    void *target = reinterpret_cast<void *>(GetProcAddress(steam, "SteamRemoteStorage"));
    if (!target) {
        logf("save-guard: SteamRemoteStorage export missing");
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void *>(&hk_get_remote_storage),
                                 reinterpret_cast<void **>(&g_orig_get));
    if (st == MH_OK)
        st = MH_EnableHook(target);
    logf("save-guard: hook SteamRemoteStorage %p: %s", target, MH_StatusToString(st));
    return st == MH_OK;
}

// Called from the mod thread: patch as soon as Steam hands out the interface, in case the game
// fetched (and cached) it before our hook went in.
void save_guard_poll()
{
    if (g_patched || !g_orig_get)
        return;
    if (void *iface = g_orig_get())
        patch_vtable(iface);
}

bool save_guard_active() { return g_patched != 0; }
long save_guard_blocked() { return g_blocked; }

// Host: what the guests should load (the merged save when guests' characters are in, else its
// latest DDDA.sav) and a version that changes with it; false when none yet.
bool storage_own_save(std::vector<uint8_t> &out, uint32_t &version, uint32_t *sig)
{
    if (!g_patched)
        return false;
    EnterCriticalSection(&g_save_lock);
    const std::vector<uint8_t> &serve = g_merged.empty() ? g_own_save : g_merged;
    bool have = !serve.empty();
    if (have) {
        out = serve;
        version = g_serve_version;
        if (sig)
            *sig = g_merged.empty() ? 0 : g_merged_sig;
    }
    LeaveCriticalSection(&g_save_lock);
    return have;
}

// Host: guest's character for a pawn (empty = give the pawn back to the host). Merged by storage_merge_tick.
void storage_set_party_character(int pawn, const std::string &character)
{
    if (pawn < 0 || pawn >= kPawns)
        return;
    EnterCriticalSection(&g_save_lock);
    if (g_party_char[pawn] != character) {
        g_party_char[pawn] = character;
        g_party_dirty = true;
    }
    LeaveCriticalSection(&g_save_lock);
}

// Host, net thread: rebuild the merged save when the guests' characters or the host's save change
// (about 300 ms: unpack, replace, pack with the game's zlib).
void storage_merge_tick()
{
    EnterCriticalSection(&g_save_lock);
    bool need = (g_party_dirty || g_merged_from != g_own_version) && !g_own_save.empty();
    std::vector<uint8_t> base;
    std::string chars[kPawns];
    uint32_t from = g_own_version;
    bool any = false;
    if (need) {
        base = g_own_save;
        for (int p = 0; p < kPawns; ++p) {
            chars[p] = g_party_char[p];
            any |= !chars[p].empty();
        }
        g_party_dirty = false;
        g_merged_from = from;
    }
    LeaveCriticalSection(&g_save_lock);
    if (!need)
        return;
    std::vector<uint8_t> merged;
    std::string orig[kPawns];
    uint32_t sig = 0; // CRC of the characters put in, per pawn (0 = none)
    uint32_t crcs[kPawns] = {};
    if (any) {
        for (int p = 0; p < kPawns; ++p)
            if (!chars[p].empty())
                crcs[p] = save_crc(chars[p].data(), chars[p].size()) | 1;
        sig = save_crc(crcs, sizeof crcs) | 1;
        std::string xml;
        uint32_t head[8];
        if (!save_unpack(base, xml, head)) {
            logf("storage: merge: the host's save could not be read");
            return;
        }
        std::string names;
        for (int p = 0; p < kPawns; ++p) {
            if (chars[p].empty())
                continue;
            if (!save_character(xml, p, orig[p]) || !save_put_character(xml, p, chars[p])) {
                logf("storage: merge: pawn %d could not be replaced", p);
                orig[p].clear();
                continue;
            }
            names += " pawn " + std::to_string(p) + " = " + save_character_name(chars[p]) + " (was " +
                     save_character_name(orig[p]) + ");";
        }
        if (!save_pack(xml, head, merged)) {
            logf("storage: merge: the merged save does not fit");
            return;
        }
        logf("storage: merged save built from version %u:%s", from, names.c_str());
    }
    EnterCriticalSection(&g_save_lock);
    g_merged.swap(merged);
    g_merged_sig = g_merged.empty() ? 0 : sig;
    for (int p = 0; p < kPawns; ++p)
        g_merged_crc[p] = g_merged.empty() ? 0 : crcs[p];
    for (int p = 0; p < kPawns; ++p)
        g_orig_char[p] = orig[p];
    ++g_serve_version;
    LeaveCriticalSection(&g_save_lock);
}

// Guest: its own character (its Arisen's mEdit and mParam), once its own save was read.
bool storage_guest_character(std::string &out)
{
    static std::string cached;
    if (!cached.empty()) {
        out = cached;
        return true;
    }
    EnterCriticalSection(&g_save_lock);
    std::vector<uint8_t> own = g_guest_own;
    LeaveCriticalSection(&g_save_lock);
    std::string xml;
    uint32_t head[8];
    if (own.empty() || !save_unpack(own, xml, head) || !save_character(xml, -1, cached)) {
        cached.clear();
        return false;
    }
    logf("storage: the guest's character: %s (%u bytes)", save_character_name(cached).c_str(), unsigned(cached.size()));
    out = cached;
    return true;
}

// Guest: the host's DDDA.sav arrived complete (checked); used for every later read of DDDA.sav.
void storage_set_peer_save(const uint8_t *data, size_t size, uint32_t version, uint32_t sig)
{
    // tests: test_owner=<SteamID64> makes the host's save belong to another account, as between two
    // real accounts (the two test copies share one)
    char owner[32] = "";
    std::vector<uint8_t> foreign;
    if (cfg_str("test_owner", owner, sizeof owner) && owner[0]) {
        std::vector<uint8_t> in(data, data + size), out;
        std::string xml;
        uint32_t head[8];
        size_t at = std::string::npos;
        if (save_unpack(in, xml, head) && (at = xml.rfind("<u64 name=\"mSteamID\" value=\"")) != std::string::npos) {
            size_t a = at + 28, b = xml.find('"', a);
            logf("storage: test_owner: the host's save owner %s -> %s", xml.substr(a, b - a).c_str(), owner);
            xml.replace(a, b - a, owner);
            if (save_pack(xml, head, out)) {
                foreign.swap(out);
                data = foreign.data();
                size = foreign.size();
            }
        }
    }
    EnterCriticalSection(&g_save_lock);
    g_peer_save.assign(data, data + size);
    g_peer_version = version;
    g_peer_sig = sig;
    LeaveCriticalSection(&g_save_lock);
}

uint32_t storage_host_sigs(uint32_t *loaded, bool *stable, uint32_t *own_version, bool *arrivals)
{
    EnterCriticalSection(&g_save_lock);
    uint32_t want = g_merged.empty() ? 0 : g_merged_sig;
    *arrivals = false; // a guest's character the game's world does not show yet (leaving ones do not count)
    for (int p = 0; p < kPawns; ++p)
        *arrivals |= !g_merged.empty() && g_merged_crc[p] && g_merged_crc[p] != g_loaded_crc[p];
    *loaded = g_loaded_sig;
    *stable = !g_party_dirty && g_merged_from == g_own_version;
    *own_version = g_own_version;
    LeaveCriticalSection(&g_save_lock);
    return want;
}

uint32_t storage_guest_sigs(uint32_t *read, bool *read_any)
{
    EnterCriticalSection(&g_save_lock);
    uint32_t have = g_peer_save.empty() ? 0 : g_peer_sig;
    *read = g_peer_read_sig;
    *read_any = g_peer_read_any;
    LeaveCriticalSection(&g_save_lock);
    return have;
}

// Guest: the game refuses a save whose owner is another Steam account. FUN_0040F880 compares the
// save's mSteamID (u64, last element of the save XML; read into [0x018FA4F8]+0x21AF98) with
// SteamUser()->GetSteamID() and returns error 5 when they differ. A guest loads the HOST's save, so
// on a guest the jne at 0x0040FA31 (75 09, to the error path) becomes jmp 0x0040FA4A (EB 17, the
// success path). The guest never saves, so the foreign id is never written anywhere.
bool storage_guest_patch()
{
    auto *at = reinterpret_cast<uint8_t *>(0x0040FA31);
    const uint8_t want[2] = {0x75, 0x09}, patch[2] = {0xEB, 0x17};
    if (memcmp(at, patch, 2) == 0)
        return true;
    if (memcmp(at, want, 2) != 0) {
        logf("storage: save owner check not patched: bytes %02X %02X, expected 75 09", at[0], at[1]);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(at, 2, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(at, patch, 2);
    VirtualProtect(at, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 2);
    logf("storage: save owner check patched (a guest loads the host's save, owned by another account)");
    return true;
}

// Guest, free roam (roam.cpp): the save its game loads instead of the host's (empty = the host's).
void storage_set_roam_save(const std::vector<uint8_t> &file)
{
    EnterCriticalSection(&g_save_lock);
    g_roam_save = file;
    LeaveCriticalSection(&g_save_lock);
}

// Guest: its own DDDA.sav as read at boot (empty until then).
bool storage_guest_own_save(std::vector<uint8_t> &out)
{
    EnterCriticalSection(&g_save_lock);
    out = g_guest_own;
    LeaveCriticalSection(&g_save_lock);
    return !out.empty();
}

// Shared world (quest_sync.cpp): the save the next load reads, whatever the role (empty = as usual).
void storage_set_override_save(const std::vector<uint8_t> &file)
{
    EnterCriticalSection(&g_save_lock);
    g_override_save = file;
    LeaveCriticalSection(&g_save_lock);
}

// This game's own latest save (its last write, blocked or not), never the merged one.
bool storage_own_raw(std::vector<uint8_t> &out, uint32_t &version)
{
    EnterCriticalSection(&g_save_lock);
    out = g_own_save;
    version = g_own_version;
    LeaveCriticalSection(&g_save_lock);
    return !out.empty();
}
