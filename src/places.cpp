// Where the player is, by name, for the Discord presence: read at run time from the player's own game files, so
// the mod carries no game data.
// - The place list: nativePC/rom/bbsrpg_core.arc, entry id\DDN\message\common\map_placelist_eng (a GMD message
//   file: u32 key count at +0x14, message count at +0x18, the messages last, each ending with a 0).
// - Each stage's places: rStagePlaceName (.spn, type 0x02373BA7) in nativePC/rom/stage/stage<N>00/stage<NNN>.arc:
//   "spn\0", version, u32 count at +8, then 30-byte entries from +0xC: x, y, z, radius (floats, world places),
//   u16 message number into the place list (stage 220: Urban Quarter, Pawn Guild...; 100: Verda Woodlands...).
// An .arc: "ARC\0", u16 version, u16 count, then 0x50-byte entries (name[64], type, compressed size, size in the
// low 29 bits, offset), zlib data inflated with the game's own zlib (save_inflate).

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "mod.h"

namespace {

std::vector<std::string> g_names; // the place list (English)
bool g_names_tried = false;
struct Spot {
    float x, y, z, r;
    uint16_t msg;
};
std::vector<Spot> g_spots;
int32_t g_spots_stage = -999;

bool read_all(const char *path, std::vector<uint8_t> &out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (256L << 20)) {
        fclose(f);
        return false;
    }
    out.resize(size_t(n));
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// the first entry of the .arc at path that `want` accepts (name, type), inflated
template <class F> bool arc_entry(const char *path, F want, std::string &out)
{
    std::vector<uint8_t> d;
    if (!read_all(path, d) || d.size() < 8 || memcmp(d.data(), "ARC\0", 4) != 0)
        return false;
    uint16_t n;
    memcpy(&n, d.data() + 6, 2);
    for (uint32_t i = 0; i < n && 8 + (i + 1) * 0x50 <= d.size(); ++i) {
        const uint8_t *e = d.data() + 8 + i * 0x50;
        char name[65] = {};
        memcpy(name, e, 64);
        uint32_t type, cs, us, off;
        memcpy(&type, e + 64, 4);
        memcpy(&cs, e + 68, 4);
        memcpy(&us, e + 72, 4);
        memcpy(&off, e + 76, 4);
        us &= 0x1FFFFFFF;
        if (!want(name, type) || off + uint64_t(cs) > d.size())
            continue;
        if (cs == us) {
            out.assign(reinterpret_cast<const char *>(d.data() + off), cs);
            return true;
        }
        std::vector<uint8_t> blob(4 + cs);
        memcpy(blob.data(), &us, 4);
        memcpy(blob.data() + 4, d.data() + off, cs);
        return save_inflate(blob, out);
    }
    return false;
}

void load_names()
{
    g_names_tried = true;
    std::string g;
    if (!arc_entry("nativePC\\rom\\bbsrpg_core.arc",
                   [](const char *name, uint32_t) { return strstr(name, "map_placelist_eng") != nullptr; }, g) ||
        g.size() < 0x20 || g.compare(0, 4, std::string("GMD\0", 4)) != 0) {
        logf("places: no place list in the game files");
        return;
    }
    uint32_t mc;
    memcpy(&mc, g.data() + 0x18, 4);
    // the messages are the last mc strings of the file
    std::vector<std::string> all;
    size_t end = g.size();
    while (end > 0 && g[end - 1] == '\0')
        --end;
    size_t pos = end;
    while (all.size() < mc && pos > 0) {
        size_t b = g.rfind('\0', pos - 1);
        size_t s = b == std::string::npos ? 0 : b + 1;
        all.push_back(g.substr(s, pos - s));
        if (b == std::string::npos)
            break;
        pos = b;
    }
    g_names.assign(all.rbegin(), all.rend());
    logf("places: %u place names from the game files", unsigned(g_names.size()));
}

void load_spots(int32_t stage)
{
    g_spots_stage = stage;
    g_spots.clear();
    if (stage < 100 || stage >= 999)
        return;
    char path[96];
    snprintf(path, sizeof path, "nativePC\\rom\\stage\\stage%d00\\stage%d.arc", int(stage / 100), int(stage));
    std::string s;
    if (!arc_entry(path, [](const char *, uint32_t type) { return type == 0x02373BA7; }, s) || s.size() < 12 ||
        s.compare(0, 4, std::string("spn\0", 4)) != 0)
        return;
    uint32_t n;
    memcpy(&n, s.data() + 8, 4);
    if (!n || n > 1000)
        return;
    size_t stride = (s.size() - 12) / n;
    if (stride < 18)
        return;
    for (uint32_t i = 0; i < n; ++i) {
        Spot sp;
        memcpy(&sp, s.data() + 12 + i * stride, 16);
        memcpy(&sp.msg, s.data() + 12 + i * stride + 16, 2);
        g_spots.push_back(sp);
    }
}

// places that are things to use rather than where one is
bool not_a_place(const std::string &n)
{
    return n.empty() || n == "???" || n == "Notice Board" || n == "Request Board" || n == "Resting Bench" ||
           n == "Portcrystal" || n == "Knowledge Chair" || n == "Field" || n.find("Floor") != std::string::npos;
}

} // namespace

// Discord thread: the area's name (stage) and the place within it where `world` (world places) stands; either
// may come back empty. Stage names for the stages whose places say what they are (stage 220: the Pawn Guild...).
void place_names(int32_t stage, const float world[3], std::string &area, std::string &spot)
{
    area.clear();
    spot.clear();
    if (stage == 100)
        area = "Gransys";
    else if (stage == 200)
        area = "Cassardis";
    else if (stage == 210)
        area = "The Encampment";
    else if (stage == 220 || stage == 230)
        area = "Gran Soren";
    else if (stage == 240)
        area = "The Duke's Demesne";
    else if (stage == 300)
        area = "Witchwood";
    else if (stage == 310)
        area = "The Greatwall";
    else if (stage >= 400 && stage < 500)
        area = "Bitterblack Isle";
    if (!g_names_tried)
        load_names();
    if (stage != g_spots_stage)
        load_spots(stage);
    float best = 1e30f;
    for (const Spot &sp : g_spots) {
        if (sp.msg >= g_names.size() || not_a_place(g_names[sp.msg]))
            continue;
        float dx = world[0] - sp.x, dy = world[1] - sp.y, dz = world[2] - sp.z;
        if (dx * dx + dy * dy + dz * dz <= sp.r * sp.r && sp.r < best) { // the smallest place it stands in
            best = sp.r;
            spot = g_names[sp.msg];
        }
    }
}
