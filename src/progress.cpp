// Guest progress (2 to 4 players). Hired pawns earn no experience in Dragon's Dogma (a fight on
// 2026-10-27 11:59: the Arisen and the main pawn +96 exp and +36 discipline, the hired pawns 0), so
// a guest's character, played as a hired pawn, gets the party's gains instead:
//  - host: at each load of a save holding guests' characters, the Arisen's level/exp/discipline and
//    the bag of each pawn a guest's character holds; at each write of such a save, what the Arisen
//    gained since (exp counted across level-ups) and the items each such pawn has more of, as one
//    record per pawn and per load ("segment"), sent back to that guest (net.cpp, MSG_PROGRESS).
//  - guest: keeps the latest record of each segment in coop_progress\pending.txt (its save is never
//    written while role=guest).
//  - later, on the guest's own PC with keep_progress=1 and not role=guest: the first read of DDDA.sav
//    is copied to coop_progress\backups\, then gets the pending gains: exp and discipline added to
//    the Arisen (the game levels it up at its next kill with its own stat growth: a level-44 Arisen
//    given 200000 exp became level 47 with higher stats, 2026-10-27 12:22), the items put in its
//    storage. Both copies of the player data in the save (mPlayerDataManual and mPlayerDataBase) get
//    them. Once the game writes its save after loading that, pending.txt becomes applied_<time>.txt.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "mod.h"

size_t save_block_end(const std::string &xml, size_t pos); // savecodec.cpp

namespace {

const char kDir[] = "coop_progress";
const char kPending[] = "coop_progress\\pending.txt";
constexpr long long kStackMax = 999;

struct Item { // a cITEM_PARAM_DATA, without its count, owner and key
    int no;
    unsigned flag, chg, d1, d2, d3;
    int mut;
    bool operator<(const Item &o) const
    {
        return std::tie(no, flag, chg, d1, d2, d3, mut) < std::tie(o.no, o.flag, o.chg, o.d1, o.d2, o.d3, o.mut);
    }
};
using Bag = std::map<Item, long long>; // item -> count

struct Point { // the Arisen's progress in a save
    bool ok = false;
    unsigned level = 0;
    long long exp = 0, next = 0, jp = 0;
};

// ---- XML (one element per line) ----

size_t find_in(const std::string &x, const std::string &s, size_t from, size_t to)
{
    size_t r = from == std::string::npos ? from : x.find(s, from);
    return r < to ? r : std::string::npos;
}

size_t next_line(const std::string &x, size_t pos)
{
    size_t nl = x.find('\n', pos);
    return nl == std::string::npos ? x.size() : nl + 1;
}

// The value="..." of the element starting at `line`.
bool value_at(const std::string &x, size_t line, long long &v)
{
    size_t a = find_in(x, "value=\"", line, next_line(x, line));
    if (a == std::string::npos)
        return false;
    v = atoll(x.c_str() + a + 7);
    return true;
}

struct Edit {
    size_t line;
    long long value;
};

void apply_edits(std::string &x, std::vector<Edit> &edits)
{
    std::sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.line > b.line; });
    for (const Edit &e : edits) { // from the end: earlier positions stay valid
        size_t a = find_in(x, "value=\"", e.line, next_line(x, e.line)) + 7;
        size_t b = x.find('"', a);
        x.replace(a, b - a, std::to_string(e.value));
    }
}

// The player data copies of a save: [begin, end) of mPlayerDataManual and mPlayerDataBase.
std::vector<std::pair<size_t, size_t>> player_copies(const std::string &x)
{
    std::vector<std::pair<size_t, size_t>> out;
    for (const char *name : {"mPlayerDataManual", "mPlayerDataBase"}) {
        size_t b = x.find(std::string("<class name=\"") + name + "\"");
        if (b != std::string::npos)
            out.push_back({b, save_block_end(x, b)});
    }
    return out;
}

// Lines of the Arisen's progress fields in one copy.
struct ParamLines {
    size_t level = std::string::npos, job, exp, next, job_exp, job_point, gold = std::string::npos;
};

bool arisen_lines(const std::string &x, size_t b, size_t e, ParamLines &l)
{
    size_t pl = find_in(x, "<class name=\"mPl\" type=\"cSAVE_DATA_PL\"", b, e);
    size_t pl_end = pl == std::string::npos ? pl : save_block_end(x, pl);
    size_t prm = find_in(x, "<class name=\"mParam\"", pl, pl_end);
    size_t prm_end = prm == std::string::npos ? prm : save_block_end(x, prm);
    l.level = find_in(x, "<u8 name=\"mLevel\"", prm, prm_end);
    l.job = find_in(x, "<u8 name=\"mJob\" ", prm, prm_end);
    l.exp = find_in(x, "<u32 name=\"mExp\"", prm, prm_end);
    l.next = find_in(x, "<u32 name=\"mNextExp\"", prm, prm_end);
    l.job_exp = find_in(x, "<array name=\"mJobExp\"", prm, prm_end);
    l.job_point = find_in(x, "<array name=\"mJobPoint\"", prm, prm_end);
    l.gold = find_in(x, "<s32 name=\"mGold\"", prm, prm_end);
    return l.level != std::string::npos && l.job != std::string::npos && l.exp != std::string::npos &&
           l.next != std::string::npos && l.job_exp != std::string::npos && l.job_point != std::string::npos;
}

// Line of element i of the array opening at `arr`.
size_t array_item(const std::string &x, size_t arr, int i)
{
    size_t line = next_line(x, arr);
    for (int k = 0; k < i; ++k)
        line = next_line(x, line);
    return line;
}

int array_count(const std::string &x, size_t arr)
{
    size_t a = find_in(x, "count=\"", arr, next_line(x, arr));
    return a == std::string::npos ? 0 : atoi(x.c_str() + a + 7);
}

Point arisen_point(const std::string &x)
{
    Point p;
    auto copies = player_copies(x);
    ParamLines l;
    if (copies.empty() || !arisen_lines(x, copies[0].first, copies[0].second, l))
        return p;
    long long level = 0;
    p.ok = value_at(x, l.level, level) && value_at(x, l.exp, p.exp) && value_at(x, l.next, p.next) &&
           value_at(x, array_item(x, l.job_point, 0), p.jp);
    p.level = unsigned(level);
    return p;
}

// Exp gained between two points of the same character. A level-up spends the level's requirement
// (mNextExp) and starts mExp again; levels skipped between two points count the mean requirement.
long long exp_gained(const Point &a, const Point &b)
{
    if (!a.ok || !b.ok || b.level < a.level)
        return 0;
    if (b.level == a.level)
        return b.exp > a.exp ? b.exp - a.exp : 0;
    long long g = a.next > a.exp ? a.next - a.exp : 0;
    g += (long long)(b.level - a.level - 1) * ((a.next + b.next) / 2);
    return g + b.exp;
}

// One cITEM_PARAM_DATA entry: its 10 field lines.
struct Entry {
    size_t line[10];
    long long v[10]; // num, no, flag, chg, d1, d2, d3, mut, owner, key
};

bool read_entry(const std::string &x, size_t open, Entry &e)
{
    size_t line = next_line(x, open);
    for (int i = 0; i < 10; ++i) {
        e.line[i] = line;
        if (!value_at(x, line, e.v[i]))
            return false;
        line = next_line(x, line);
    }
    return true;
}

Item item_of(const Entry &e)
{
    return {int(e.v[1]), unsigned(e.v[2]), unsigned(e.v[3]), unsigned(e.v[4]), unsigned(e.v[5]), unsigned(e.v[6]), int(e.v[7])};
}

const char kEntry[] = "<class type=\"sItemManager::cITEM_PARAM_DATA\">";

// The first `count` entries of the item array opening at `arr` (they are kept together at the start).
std::vector<Entry> entries(const std::string &x, size_t arr, int count)
{
    std::vector<Entry> out;
    size_t end = save_block_end(x, arr), at = arr;
    for (int i = 0; i <= count; ++i) { // one more: the first free entry
        at = find_in(x, kEntry, at + 1, end);
        Entry e;
        if (at == std::string::npos || !read_entry(x, at, e))
            break;
        out.push_back(e);
    }
    return out;
}

// The bag of party member `who` (0 = Arisen, 1 + mCmcId = pawns) in the first player data copy.
bool bag_of(const std::string &x, int who, Bag &bag)
{
    auto copies = player_copies(x);
    if (copies.empty())
        return false;
    size_t arr = find_in(x, "<array name=\"mItem\" type=\"class\" count=\"4\">", copies[0].first, copies[0].second);
    size_t at = arr;
    for (int i = 0; i <= who && at != std::string::npos; ++i)
        at = find_in(x, "<class type=\"cSAVE_DATA_ITEM\">", at + 1, copies[0].second);
    if (at == std::string::npos)
        return false;
    long long count = 0;
    size_t cline = next_line(x, at), list = next_line(x, cline);
    if (!value_at(x, cline, count))
        return false;
    bag.clear();
    for (const Entry &e : entries(x, list, int(count)))
        if (e.v[1] >= 0 && e.v[0] > 0)
            bag[item_of(e)] += e.v[0];
    return true;
}

// Adds exp, discipline and items to the Arisen of every player data copy.
bool add_progress(std::string &x, long long exp, long long jp, const Bag &items, int *placed, long long gold = 0)
{
    std::vector<Edit> edits;
    *placed = 0;
    for (auto [b, e] : player_copies(x)) {
        ParamLines l;
        long long v = 0, job = 0;
        if (!arisen_lines(x, b, e, l) || !value_at(x, l.exp, v) || !value_at(x, l.job, job))
            return false;
        edits.push_back({l.exp, std::min<long long>(v + exp, 0xFFFFFFFFll)});
        if (gold && l.gold != std::string::npos && value_at(x, l.gold, v)) // never below 0
            edits.push_back({l.gold, std::max<long long>(0, std::min<long long>(v + gold, 0x7FFFFFFFll))});
        if (jp > 0) {
            if (job >= 0 && job < array_count(x, l.job_exp)) {
                size_t line = array_item(x, l.job_exp, int(job));
                if (value_at(x, line, v))
                    edits.push_back({line, std::min<long long>(v + jp, 0xFFFFFFFFll)});
            }
            for (int i = 0, n = array_count(x, l.job_point); i < n; ++i) {
                size_t line = array_item(x, l.job_point, i);
                if (value_at(x, line, v))
                    edits.push_back({line, std::min<long long>(v + jp, 0x7FFFFFFFll)});
            }
        }
        // items: the storage, stacked onto an entry of the same item, else in the first free entry
        size_t cline = find_in(x, "<u32 name=\"mStorageItemCount\"", b, e);
        size_t arr = find_in(x, "<array name=\"mStorageItem\"", b, e);
        long long count = 0;
        if (items.empty())
            continue;
        if (cline == std::string::npos || arr == std::string::npos || !value_at(x, cline, count))
            return false;
        std::vector<Entry> list = entries(x, arr, int(count));
        int capacity = array_count(x, arr), used = int(count);
        int put = 0;
        for (auto &[it, n] : items) {
            if (n <= 0)
                continue; // given away as much as it gained (a gift to another player)
            Entry *same = nullptr;
            for (int i = 0; i < used && i < int(list.size()) && !same; ++i)
                if (!(item_of(list[i]) < it) && !(it < item_of(list[i])))
                    same = &list[i];
            if (same) {
                same->v[0] = std::min(same->v[0] + n, kStackMax);
                edits.push_back({same->line[0], same->v[0]});
            } else if (used < capacity && used < int(list.size())) {
                Entry &f = list[used++];
                long long vals[10] = {std::min(n, kStackMax), it.no, it.flag, it.chg, it.d1, it.d2, it.d3, it.mut, 4, 0};
                for (int i = 0; i < 10; ++i)
                    edits.push_back({f.line[i], vals[i]});
                if (used < capacity) { // the next free entry, for another new item
                    size_t at = find_in(x, kEntry, f.line[9], save_block_end(x, arr));
                    Entry g;
                    if (at != std::string::npos && read_entry(x, at, g))
                        list.push_back(g);
                }
            } else {
                continue; // storage full
            }
            ++put;
        }
        edits.push_back({cline, used});
        *placed = put;
    }
    apply_edits(x, edits);
    return true;
}

// ---- host ----

CRITICAL_SECTION g_lock;
volatile LONG g_lock_ready = 0;

void lock()
{
    if (InterlockedCompareExchange(&g_lock_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        InterlockedExchange(&g_lock_ready, 2);
    }
    while (g_lock_ready != 2)
        Sleep(0);
    EnterCriticalSection(&g_lock);
}
void unlock() { LeaveCriticalSection(&g_lock); }

struct Segment {
    char id[32] = "";
    std::vector<uint8_t> load;      // the save the game loaded, parsed at the first write
    uint32_t crc[kPawns] = {};      // the guests' characters it holds (0 = the host's own pawn)
    bool parsed = false;
    Point last;                     // the Arisen at the load, then at each write
    long long exp = 0, jp = 0;      // gained since the load
    Bag base[kPawns];               // each guest pawn's bag at the load
} g_seg;
unsigned g_loads = 0;
std::string g_record[kPawns]; // the latest record for each pawn's guest (empty = none)

std::string bag_text(const Bag &bag)
{
    std::string s;
    char line[128];
    for (auto &[it, n] : bag) {
        _snprintf(line, sizeof line, "item %d %lld %u %u %u %u %u %d\n", it.no, n, it.flag, it.chg, it.d1, it.d2, it.d3, it.mut);
        s += line;
    }
    return s;
}

// ---- guest's pending file ----

struct Pending {
    std::string name;
    struct Seg {
        long long exp = 0, jp = 0;
        long long gold = 0; // shared world, guest: its gold's change (purchases, sales, gold found)
        Bag items;
    };
    std::map<std::string, Seg> segs;
};

bool parse_item(const char *s, Item &it, long long &n)
{
    return sscanf(s, "item %d %lld %u %u %u %u %u %d", &it.no, &n, &it.flag, &it.chg, &it.d1, &it.d2, &it.d3, &it.mut) == 8;
}

bool read_pending(Pending &p)
{
    FILE *f = fopen(kPending, "r");
    if (!f)
        return false;
    char line[512];
    Pending::Seg *cur = nullptr;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char id[64];
        long long a = 0, b = 0;
        Item it;
        if (strncmp(line, "name ", 5) == 0)
            p.name = line + 5;
        else if (sscanf(line, "segment %63s %lld %lld", id, &a, &b) == 3)
            cur = &p.segs[id], cur->exp = a, cur->jp = b;
        else if (cur && sscanf(line, "gold %lld", &a) == 1)
            cur->gold = a;
        else if (cur && parse_item(line, it, a))
            cur->items[it] += a;
    }
    fclose(f);
    return true;
}

bool write_pending(const Pending &p)
{
    CreateDirectoryA(kDir, nullptr);
    std::string s = "ddda-coop pending progress (DDDA-Coop): added to this PC's save at its next load with keep_progress=1\n";
    s += "name " + p.name + "\n";
    for (auto &[id, seg] : p.segs)
        s += "segment " + id + " " + std::to_string(seg.exp) + " " + std::to_string(seg.jp) + "\n" +
             (seg.gold ? "gold " + std::to_string(seg.gold) + "\n" : std::string()) + bag_text(seg.items);
    std::string tmp = std::string(kPending) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    ok = fclose(f) == 0 && ok;
    return ok && MoveFileExA(tmp.c_str(), kPending, MOVEFILE_REPLACE_EXISTING);
}

void totals(const Pending &p, long long &exp, long long &jp, Bag &items, long long *gold = nullptr)
{
    exp = jp = 0;
    if (gold)
        *gold = 0;
    items.clear();
    for (auto &[id, seg] : p.segs) {
        exp += seg.exp;
        jp += seg.jp;
        if (gold)
            *gold += seg.gold;
        for (auto &[it, n] : seg.items)
            items[it] += n;
    }
}

// ---- applying on the guest's own PC ----

uint32_t g_orig_crc = 0;         // the save bytes the cached patch was made from
std::vector<uint8_t> g_patched;  // what the game reads instead
bool g_patched_loaded = false;   // the game loaded the patched save (its next write keeps the gains)

bool keep_progress()
{
    char v[8] = "";
    return cfg_str("keep_progress", v, sizeof v) && v[0] == '1';
}

std::string stamp()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char s[32];
    _snprintf(s, sizeof s, "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
}

} // namespace

// Host, on a load of a save holding guests' characters (crc per pawn, 0 = the host's own).
void progress_host_loaded(const std::vector<uint8_t> &file, const uint32_t crc[])
{
    lock();
    g_seg = Segment();
    _snprintf(g_seg.id, sizeof g_seg.id, "%08lX-%u", GetCurrentProcessId() ^ GetTickCount(), ++g_loads);
    g_seg.load = file;
    memcpy(g_seg.crc, crc, sizeof g_seg.crc);
    unlock();
}

// Host, game thread, a write of a save holding guests' characters (xml before un-merging).
void progress_host_write(const std::string &xml)
{
    if (net_shared())
        return; // shared world: each guest keeps its own gains as they happen (progress_shared_gain)
    lock();
    Segment &s = g_seg;
    bool any = false;
    for (uint32_t c : s.crc)
        any |= c != 0;
    if (!any || (s.load.empty() && !s.parsed)) {
        unlock();
        return;
    }
    if (!s.parsed) { // the load, parsed once (unpacking takes ~0.3 s)
        std::string base;
        uint32_t head[8];
        if (save_unpack(s.load, base, head)) {
            s.last = arisen_point(base);
            for (int p = 0; p < kPawns; ++p)
                if (s.crc[p])
                    bag_of(base, 1 + p, s.base[p]);
        }
        s.load.clear();
        s.parsed = true;
    }
    Point now = arisen_point(xml);
    if (now.ok && s.last.ok) {
        s.exp += exp_gained(s.last, now);
        s.jp += now.jp > s.last.jp ? now.jp - s.last.jp : 0;
    }
    if (now.ok)
        s.last = now;
    for (int p = 0; p < kPawns; ++p) {
        if (!s.crc[p])
            continue;
        Bag bag, gained;
        bag_of(xml, 1 + p, bag);
        for (auto &[it, n] : bag) {
            auto b = s.base[p].find(it);
            long long more = n - (b == s.base[p].end() ? 0 : b->second);
            if (more > 0)
                gained[it] = more;
        }
        std::string ch;
        save_character(xml, p, ch);
        char head[160];
        _snprintf(head, sizeof head, "ddda-coop-progress 1\nbase %08X\nsegment %s\nexp %lld\njp %lld\n", s.crc[p], s.id, s.exp, s.jp);
        g_record[p] = std::string(head) + "name " + save_character_name(ch) + "\n" + bag_text(gained);
        logf("progress: pawn %d (%s): +%lld exp, +%lld discipline, %d new item kind(s) since the load (segment %s)", p,
             save_character_name(ch).c_str(), s.exp, s.jp, int(gained.size()), s.id);
    }
    unlock();
}

// Host, net thread: the latest record for the guest driving `pawn`.
bool progress_host_record(int pawn, std::string &out)
{
    if (pawn < 0 || pawn >= kPawns)
        return false;
    lock();
    out = g_record[pawn];
    unlock();
    return !out.empty();
}

// Guest, net thread: a record from the host. False when it is not for this guest's character.
bool progress_guest_received(const std::string &rec)
{
    std::string own;
    unsigned base = 0;
    char id[64] = "";
    long long exp = 0, jp = 0;
    const char *b = strstr(rec.c_str(), "\nbase "), *s = strstr(rec.c_str(), "\nsegment "),
               *e = strstr(rec.c_str(), "\nexp "), *j = strstr(rec.c_str(), "\njp ");
    if (!b || !s || !e || !j || sscanf(b, "\nbase %x", &base) != 1 || sscanf(s, "\nsegment %63s", id) != 1 ||
        sscanf(e, "\nexp %lld", &exp) != 1 || sscanf(j, "\njp %lld", &jp) != 1)
        return false;
    if (!storage_guest_character(own) || (save_crc(own.data(), own.size()) | 1) != base) {
        logf("progress: a record for another character (base %08X), ignored", base);
        return false;
    }
    Pending p;
    read_pending(p);
    p.name = save_character_name(own);
    Pending::Seg &seg = p.segs[id];
    seg = Pending::Seg();
    seg.exp = exp;
    seg.jp = jp;
    for (const char *line = strstr(rec.c_str(), "\nitem "); line; line = strstr(line + 1, "\nitem ")) {
        Item it;
        long long n = 0;
        if (parse_item(line + 1, it, n) && n > 0)
            seg.items[it] += n;
    }
    bool ok = write_pending(p);
    long long te, tj;
    Bag ti;
    totals(p, te, tj, ti);
    logf("progress: from the host: segment %s +%lld exp, +%lld discipline, %d item kind(s); pending in total +%lld exp, "
         "+%lld discipline, %d item kind(s)%s",
         id, exp, jp, int(seg.items.size()), te, tj, int(ti.size()), ok ? "" : " (pending.txt NOT written)");
    return ok;
}

// Not a guest, keep_progress=1: the save bytes the game reads, with the pending gains added (the
// original copied to coop_progress\backups first). `load`: this read loads the save into the game.
bool progress_patch(std::vector<uint8_t> &bytes, bool load)
{
    if (cfg_role() == NET_GUEST || !keep_progress() || bytes.empty())
        return false;
    lock();
    uint32_t crc = save_crc(bytes.data(), bytes.size());
    if (!g_patched.empty() && crc == g_orig_crc) {
        bytes = g_patched;
        g_patched_loaded |= load;
        unlock();
        return true;
    }
    unlock();
    Pending p;
    if (!read_pending(p) || p.segs.empty())
        return false;
    long long exp, jp, gold;
    Bag items;
    totals(p, exp, jp, items, &gold);
    std::string xml, ch;
    uint32_t head[8];
    std::vector<uint8_t> out;
    if (!save_unpack(bytes, xml, head) || !save_character(xml, -1, ch)) {
        logf("progress: the save could not be read, nothing added");
        return false;
    }
    if (save_character_name(ch) != p.name) {
        logf("progress: pending gains are for %s, this save's Arisen is %s: nothing added", p.name.c_str(),
             save_character_name(ch).c_str());
        return false;
    }
    int placed = 0;
    if (!add_progress(xml, exp, jp, items, &placed, gold) || !save_pack(xml, head, out)) {
        logf("progress: the gains could not be added, nothing changed");
        return false;
    }
    CreateDirectoryA(kDir, nullptr);
    CreateDirectoryA("coop_progress\\backups", nullptr);
    std::string backup = "coop_progress\\backups\\DDDA.sav." + stamp();
    FILE *f = fopen(backup.c_str(), "wb");
    bool saved = f && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (f)
        saved = fclose(f) == 0 && saved;
    if (!saved) {
        logf("progress: the backup %s could not be written: nothing added", backup.c_str());
        return false;
    }
    logf("progress: added to %s's save: +%lld exp, +%lld discipline, %+lld gold, %d of %d item kind(s) put in storage "
         "(the save before: %s)",
         p.name.c_str(), exp, jp, gold, placed, int(items.size()), backup.c_str());
    lock();
    g_orig_crc = crc;
    g_patched = out;
    g_patched_loaded |= load;
    unlock();
    bytes.swap(out);
    return true;
}

// Not a guest: the game wrote its save. If it had loaded the patched save, the gains are in it now.
void progress_after_write()
{
    lock();
    bool done = g_patched_loaded;
    if (done) {
        g_patched.clear();
        g_patched_loaded = false;
        g_orig_crc = 0;
    }
    unlock();
    if (!done)
        return;
    std::string to = "coop_progress\\applied_" + stamp() + ".txt";
    if (MoveFileExA(kPending, to.c_str(), MOVEFILE_REPLACE_EXISTING))
        logf("progress: the game saved with the gains: %s is now %s", kPending, to.c_str());
}

// ---- free roam (roam.cpp): what the guest's own character gains while exploring on its own ----

namespace {
Point g_roam_point;
Bag g_roam_bag;
bool g_roam_started = false;
} // namespace

// Guest, the save it loads to roam (its character as the Arisen): the starting point.
void progress_roam_begin(const std::string &xml)
{
    lock();
    g_roam_point = arisen_point(xml);
    g_roam_started = bag_of(xml, 0, g_roam_bag) && g_roam_point.ok;
    unlock();
}

// Guest, a save written at the end of the roam: the Arisen's gains since the start, kept like the
// host's records (a segment of their own in pending.txt).
void progress_roam_end(const std::string &xml)
{
    lock();
    bool started = g_roam_started;
    Point a = g_roam_point;
    Bag base = g_roam_bag;
    g_roam_started = false;
    unlock();
    if (!started)
        return;
    Point b = arisen_point(xml);
    Bag now;
    bag_of(xml, 0, now);
    long long exp = exp_gained(a, b), jp = b.ok && b.jp > a.jp ? b.jp - a.jp : 0;
    Pending p;
    read_pending(p);
    std::string own;
    if (storage_guest_character(own))
        p.name = save_character_name(own);
    Pending::Seg &seg = p.segs["roam-" + stamp()];
    seg.exp = exp;
    seg.jp = jp;
    for (auto &[it, n] : now) {
        auto f = base.find(it);
        long long more = n - (f == base.end() ? 0 : f->second);
        if (more > 0)
            seg.items[it] = more;
    }
    bool ok = write_pending(p);
    logf("progress: free roam: +%lld exp, +%lld discipline, %d new item kind(s)%s", exp, jp, int(seg.items.size()),
         ok ? "" : " (pending.txt NOT written)");
}

// Shared world, guest: gains of its own character in its copy of the host's world (never saved
// there) kept for its own save: exp and discipline, and items (picked up or copies of the other
// players' pickups). One "shared-<session>" segment per game session, added to on each call.
static const std::string &shared_seg_id()
{
    static std::string seg_id;
    if (seg_id.empty()) {
        char s[48];
        _snprintf(s, sizeof s, "shared-%s-%lu", stamp().c_str(), GetCurrentProcessId());
        seg_id = s;
    }
    return seg_id;
}

void progress_shared_gain(long long exp, long long jp, int item, int count, unsigned flags, long long gold)
{
    const std::string &seg_id = shared_seg_id();
    lock();
    Pending p;
    read_pending(p);
    std::string own;
    if (storage_guest_character(own))
        p.name = save_character_name(own);
    Pending::Seg &seg = p.segs[seg_id];
    seg.exp += exp > 0 ? exp : 0;
    seg.jp += jp > 0 ? jp : 0;
    if (item >= 0 && count != 0) { // < 0: given to another player
        long long &n = seg.items[Item{item, flags, 0, 0, 0, 0, 0}];
        n += count;
        if (n <= 0) // not kept below 0 here: the item may have come from the host's bag, not from a gain
            seg.items.erase(Item{item, flags, 0, 0, 0, 0, 0});
    }
    seg.gold += gold;
    bool ok = write_pending(p);
    unlock();
    if (!ok)
        logf("progress: shared world gains NOT written to pending.txt");
}

// Shared world, guest, game thread: its Arisen's gold (character parameter [ch+0x3DEC]+0x348, the save's
// mParam mGold; found with uscan, shop test 2026-11-06 23:09: 1747991 -> 1740381 for a 7610 hat). Its
// world is the host's save with its own character in, as it was when it joined, so its purchases and
// sales were lost when it left (Nexus report, 2026-11-06) and its gold went back after each load. Each
// change is kept for its own save (pending.txt "gold"), and after a load of its world its gold is put
// back to what it had (the save's value + the changes of this session).
void progress_guest_gold_tick()
{
    static long seen_load = -1;
    static DWORD load_at = 0;
    static long long last = -1, session = 0, boot = -1;
    if (cfg_role() != NET_GUEST || !net_shared())
        return;
    uintptr_t a = possess_arisen();
    uintptr_t prm = a ? *reinterpret_cast<uintptr_t *>(a + 0x3DEC) : 0;
    if (!prm || !mem_readable(reinterpret_cast<void *>(prm + 0x348), 4))
        return;
    int32_t &g = *reinterpret_cast<int32_t *>(prm + 0x348);
    long loads = storage_guest_world_loads();
    if (loads == 0)
        return; // not in the host's world yet
    DWORD t = GetTickCount();
    if (loads != seen_load) { // a load of its world: wait for the character to settle, then put the gold back
        if (!load_at) {
            load_at = t | 1;
            return;
        }
        if (int(t - load_at) < 3000)
            return;
        load_at = 0;
        seen_load = loads;
        if (boot < 0)
            boot = g; // the first load: its own save's gold
        // only when the load brought back the gold it joined with: a reload in place from its own snapshot
        // (rejoin.cpp, a second guest joining, 2026-11-06 23:31) keeps the gold it had, changes included
        if (!session || g != boot) {
            last = g;
            return;
        }
        long long want = std::max<long long>(0, std::min<long long>(g + session, 0x7FFFFFFFll));
        logf("progress: gold after a load: back to %d, this session's changes %+lld: set to %lld", g, session, want);
        g = int32_t(want);
        // and the Arisen's own member parameter in sGameSys (+0xA76D0, as the gear sync writes it)
        uintptr_t gs = *reinterpret_cast<uintptr_t *>(0x018FA4BC);
        if (gs && gs + 0xA76D0 != prm && mem_readable(reinterpret_cast<void *>(gs + 0xA76D0 + 0x348), 4))
            *reinterpret_cast<int32_t *>(gs + 0xA76D0 + 0x348) = int32_t(want);
        last = want;
        return;
    }
    if (last < 0 || g == last)
        return;
    long long d = g - last;
    last = g;
    long sale = loot_take_sale_gold(); // a sale's gold: the items sold were the host's (loot.cpp)
    long long kept = d - (sale > 0 && d > 0 ? std::min<long long>(d, sale) : 0);
    session += d; // its world's gold, for the put-back after a load
    if (kept != d)
        logf("progress: gold %+lld (now %d), of which %ld from a sale: %+lld kept for this player's own save", d, g,
             sale, kept);
    else
        logf("progress: gold %+lld (now %d, this session %+lld): kept for this player's own save", d, g, session);
    if (kept)
        progress_shared_gain(0, 0, -1, 0, 0, kept);
}
