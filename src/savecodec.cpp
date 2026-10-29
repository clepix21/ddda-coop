// DDDA.sav packing and the "a player's character replaces a pawn" merge (2 to 4 players, step 2).
//
// File (524288 bytes): a 32-byte header of u32 {0x15, uncompressed size, compressed size, h3, h4,
// h5, CRC-32 of the compressed data without the final inversion, h7} (h3/h4/h5/h7 kept as read),
// then the zlib-compressed XML ("sSave::saveDataAllDA", ~20 MB), then zeros. Checked on two saves
// (tools/save_merge.py, the offline prototype of this file).
//
// The game links zlib 1.2.1 statically; this file calls its functions (cdecl, z_stream of 0x38
// bytes): deflateInit_ 0x00CF2C40, deflate 0x00CF16F0, deflateEnd 0x00CF1AF0, inflateInit_
// 0x00CF01C0, inflate 0x00CF02E0 (found from the "1.2.1" version pushes and the save writer's
// vtable 0x01559A18). No zlib of our own, nothing downloaded.
//
// XML: mPlayerDataManual > mPlCmcEditAndParam > mPl (the Arisen) and the array mCmc (pawn 0 = main,
// 1 and 2 = hired), each with mEdit (look and name) and mParam (level, vocation, skills, equipment,
// HP, gold). A player's character = the mEdit and mParam blocks of their own Arisen; it replaces
// those blocks of one pawn.
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mod.h"

namespace {

struct ZStream { // zlib 1.2.1 z_stream, 32-bit
    const uint8_t *next_in;
    uint32_t avail_in, total_in;
    uint8_t *next_out;
    uint32_t avail_out, total_out;
    const char *msg;
    void *state, *zalloc, *zfree, *opaque;
    int data_type;
    uint32_t adler, reserved;
};
static_assert(sizeof(ZStream) == 0x38, "z_stream size");

using DeflateInit_t = int(__cdecl *)(ZStream *, int, const char *, int);
using Deflate_t = int(__cdecl *)(ZStream *, int);
using InflateInit_t = int(__cdecl *)(ZStream *, const char *, int);
const auto z_deflateInit = reinterpret_cast<DeflateInit_t>(0x00CF2C40);
const auto z_deflate = reinterpret_cast<Deflate_t>(0x00CF16F0);
const auto z_deflateEnd = reinterpret_cast<int(__cdecl *)(ZStream *)>(0x00CF1AF0);
const auto z_inflateInit = reinterpret_cast<InflateInit_t>(0x00CF01C0);
const auto z_inflate = reinterpret_cast<Deflate_t>(0x00CF02E0);
constexpr int Z_FINISH = 4, Z_STREAM_END = 1;
constexpr size_t kSaveSize = 524288;

uint32_t crc32_raw(const uint8_t *p, size_t n) // CRC-32 without the final inversion
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

// [first, last) of the block (class or array) that opens at `pos`: from its '<' to after its closing tag.
size_t block_end(const std::string &x, size_t pos)
{
    int depth = 0;
    size_t i = pos;
    while (i < x.size()) {
        size_t lt = x.find('<', i);
        if (lt == std::string::npos)
            return std::string::npos;
        size_t gt = x.find('>', lt);
        if (gt == std::string::npos)
            return std::string::npos;
        bool closing = x[lt + 1] == '/', self = x[gt - 1] == '/';
        bool container = x.compare(lt + 1, 5, "class") == 0 || x.compare(lt + 1, 5, "array") == 0 ||
                         x.compare(lt + 2, 5, "class") == 0 || x.compare(lt + 2, 5, "array") == 0;
        if (container && !self)
            depth += closing ? -1 : 1;
        i = gt + 1;
        if (depth == 0)
            return i;
    }
    return std::string::npos;
}

// The direct child `<class name="NAME"` of the block opening at `pos`; npos if absent.
size_t child(const std::string &x, size_t pos, const char *name)
{
    std::string tag = std::string("<class name=\"") + name + "\"";
    size_t end = block_end(x, pos), i = x.find('>', pos) + 1;
    while (i < end) {
        size_t lt = x.find('<', i);
        if (lt == std::string::npos || lt >= end)
            return std::string::npos;
        if (x.compare(lt, tag.size(), tag) == 0)
            return lt;
        size_t gt = x.find('>', lt);
        bool container = x.compare(lt + 1, 5, "class") == 0 || x.compare(lt + 1, 5, "array") == 0;
        if (x[lt + 1] == '/')
            return std::string::npos;
        i = container && x[gt - 1] != '/' ? block_end(x, lt) : gt + 1;
    }
    return std::string::npos;
}

// The owner block (mPl, or mCmc[pawn]) inside the first mPlCmcEditAndParam from `from`
// (mPlayerDataManual's from 0).
size_t owner_block(const std::string &x, int pawn, size_t from = 0)
{
    size_t top = x.find("<class name=\"mPlCmcEditAndParam\"", from);
    if (top == std::string::npos)
        return top;
    if (pawn < 0)
        return child(x, top, "mPl");
    size_t arr = x.find("<array name=\"mCmc\"", top);
    if (arr == std::string::npos || arr > block_end(x, top))
        return std::string::npos;
    size_t i = x.find("<class type=\"cSAVE_DATA_CMC\"", arr);
    for (int n = 0; n < pawn && i != std::string::npos; ++n)
        i = x.find("<class type=\"cSAVE_DATA_CMC\"", block_end(x, i));
    return i;
}

} // namespace

uint32_t save_crc(const void *p, size_t n) { return crc32_raw(static_cast<const uint8_t *>(p), n); }
size_t save_block_end(const std::string &xml, size_t pos) { return block_end(xml, pos); }

bool save_unpack(const std::vector<uint8_t> &file, std::string &xml, uint32_t head[8])
{
    if (file.size() < 32)
        return false;
    memcpy(head, file.data(), 32);
    if (head[0] != 0x15 || head[2] == 0 || 32 + size_t(head[2]) > file.size() || head[1] > (64u << 20))
        return false;
    if (crc32_raw(file.data() + 32, head[2]) != head[6]) {
        logf("savecodec: checksum mismatch");
        return false;
    }
    xml.assign(head[1], '\0');
    ZStream z = {};
    if (z_inflateInit(&z, "1.2.1", int(sizeof z)) != 0)
        return false;
    z.next_in = file.data() + 32;
    z.avail_in = head[2];
    z.next_out = reinterpret_cast<uint8_t *>(&xml[0]);
    z.avail_out = head[1];
    int r = z_inflate(&z, Z_FINISH);
    // inflateEnd is not located yet: the state (about 40 KB) is left allocated, once per unpack
    return r == Z_STREAM_END && z.total_out == head[1];
}

bool save_pack(const std::string &xml, const uint32_t base_head[8], std::vector<uint8_t> &file)
{
    std::vector<uint8_t> comp(kSaveSize - 32);
    ZStream z = {};
    if (z_deflateInit(&z, 9, "1.2.1", int(sizeof z)) != 0)
        return false;
    z.next_in = reinterpret_cast<const uint8_t *>(xml.data());
    z.avail_in = uint32_t(xml.size());
    z.next_out = comp.data();
    z.avail_out = uint32_t(comp.size());
    int r = z_deflate(&z, Z_FINISH);
    uint32_t n = z.total_out;
    z_deflateEnd(&z);
    if (r != Z_STREAM_END) {
        logf("savecodec: deflate %d (does not fit in %u bytes?)", r, unsigned(comp.size()));
        return false;
    }
    uint32_t head[8];
    memcpy(head, base_head, sizeof head);
    head[1] = uint32_t(xml.size());
    head[2] = n;
    head[6] = crc32_raw(comp.data(), n);
    file.assign(kSaveSize, 0);
    memcpy(file.data(), head, 32);
    memcpy(file.data() + 32, comp.data(), n);
    return true;
}

std::vector<size_t> player_data_starts(const std::string &x);

// The mEdit and mParam blocks of the Arisen (pawn -1) or of a pawn, as one string (mEdit then mParam).
// The game shows a character with the mEdit (look, name) of the mPlayerDataBase copy and the mParam
// of the mPlayerDataManual copy (2026-10-27 12:22), so it is taken from there: the copies differed in
// the merged saves of the 2026-10-28 evening (only the Manual copy was merged), and the other players
// had another pawn's face, sex and name.
bool save_character(const std::string &xml, int pawn, std::string &out)
{
    std::vector<size_t> starts = player_data_starts(xml);
    size_t o = owner_block(xml, pawn);
    size_t look = starts.size() >= 2 ? owner_block(xml, pawn, starts[1]) : o;
    if (o == std::string::npos || look == std::string::npos)
        return false;
    size_t e = child(xml, look, "mEdit"), p = child(xml, o, "mParam");
    if (e == std::string::npos || p == std::string::npos)
        return false;
    out = xml.substr(e, block_end(xml, e) - e) + "\n" + xml.substr(p, block_end(xml, p) - p);
    return true;
}

// The Arisen's look and name has a third copy, mSystemData > mEditPl, which the menus show (Statut:
// a guest's own Arisen had the host's name there, menu screenshots 2026-10-29). The Arisen's mEdit is
// written there too.
bool put_system_edit(std::string &xml, const std::string &character)
{
    size_t sys = xml.find("<class name=\"mSystemData\"");
    if (sys == std::string::npos)
        return true; // no such block: nothing to keep in step
    size_t e = child(xml, sys, "mEditPl");
    size_t split = character.find("<class name=\"mParam\"");
    if (e == std::string::npos || split == std::string::npos || character.compare(0, 19, "<class name=\"mEdit\"") != 0)
        return false;
    std::string edit = character.substr(0, character.find_last_not_of('\n', split - 1) + 1);
    edit.replace(0, 19, "<class name=\"mEditPl\"");
    xml.replace(e, block_end(xml, e) - e, edit);
    return true;
}

// Writes a character (from save_character) over pawn `pawn`'s mEdit and mParam, in every player data
// copy (and the Arisen's look in mSystemData).
bool save_put_character(std::string &xml, int pawn, const std::string &character)
{
    if (pawn < 0 && !put_system_edit(xml, character))
        return false;
    int done = 0;
    for (size_t i = 0;; ++i) {
        std::vector<size_t> starts = player_data_starts(xml); // offsets move with each replace
        if (i >= starts.size())
            break;
        if (!save_put_character_at(xml, pawn, character, starts[i]))
            return false;
        ++done;
    }
    return done > 0;
}

// The same in the copy of the player data that starts at or after `from`.
bool save_put_character_at(std::string &xml, int pawn, const std::string &character, size_t from)
{
    size_t split = character.find("<class name=\"mParam\"");
    if (split == std::string::npos || character.compare(0, 19, "<class name=\"mEdit\"") != 0)
        return false;
    std::string edit = character.substr(0, character.find_last_not_of('\n', split - 1) + 1);
    std::string param = character.substr(split);
    size_t o = owner_block(xml, pawn, from);
    if (o == std::string::npos)
        return false;
    size_t p = child(xml, o, "mParam");
    if (p == std::string::npos)
        return false;
    xml.replace(p, block_end(xml, p) - p, param); // mParam first: mEdit comes before it
    size_t e = child(xml, o, "mEdit");
    if (e == std::string::npos)
        return false;
    xml.replace(e, block_end(xml, e) - e, edit);
    return true;
}

// Starts of the player data copies (mPlayerDataManual, mPlayerDataBase): the game reads the
// Arisen's look from one and its parameters from the other (2026-10-27 12:22), so a change to a
// character goes to both.
std::vector<size_t> player_data_starts(const std::string &x)
{
    std::vector<size_t> out;
    for (const char *name : {"<class name=\"mPlayerDataManual\"", "<class name=\"mPlayerDataBase\""}) {
        size_t at = x.find(name);
        if (at != std::string::npos)
            out.push_back(at);
    }
    return out;
}

// Every copy: the Arisen gets pawn `pawn`'s character and that pawn the Arisen's.
bool save_swap_arisen_pawn(std::string &xml, int pawn)
{
    std::string arisen, other;
    return save_character(xml, -1, arisen) && save_character(xml, pawn, other) && save_put_character(xml, -1, other) &&
           save_put_character(xml, pawn, arisen);
}

// Every copy: the Arisen gets `character` (its own character on this PC).
bool save_put_arisen_everywhere(std::string &xml, const std::string &character)
{
    return save_put_character(xml, -1, character);
}

// For logs: the names in each player data copy, "Manual: A, p0, p1, p2 / Base: ... / System: A".
std::string save_copy_names(const std::string &xml)
{
    std::string out;
    std::vector<size_t> starts = player_data_starts(xml);
    size_t sys = xml.find("<class name=\"mSystemData\"");
    size_t se = sys == std::string::npos ? sys : child(xml, sys, "mEditPl");
    std::string system = se == std::string::npos ? "?" : save_character_name(xml.substr(se, block_end(xml, se) - se));
    for (size_t i = 0; i < starts.size(); ++i) {
        out += i ? " / Base:" : "Manual:";
        for (int p = -1; p < kPawns; ++p) {
            size_t o = owner_block(xml, p, starts[i]), e = o == std::string::npos ? o : child(xml, o, "mEdit");
            out += std::string(p < 0 ? " " : ", ") +
                   (e == std::string::npos ? "?" : save_character_name(xml.substr(e, block_end(xml, e) - e)));
        }
    }
    return out + " / System: " + system;
}

// The bag of party member `who` (0 = Arisen, 1 + mCmcId) in copy `copy`: its cSAVE_DATA_ITEM block.
size_t bag_block(const std::string &x, int copy, int who)
{
    std::vector<size_t> starts = player_data_starts(x);
    if (copy < 0 || size_t(copy) >= starts.size())
        return std::string::npos;
    size_t arr = x.find("<array name=\"mItem\" type=\"class\" count=\"4\">", starts[copy]);
    if (arr == std::string::npos || arr > block_end(x, starts[copy]))
        return std::string::npos;
    size_t at = arr;
    for (int i = 0; i <= who && at != std::string::npos; ++i)
        at = x.find("<class type=\"cSAVE_DATA_ITEM\">", at + 1);
    return at;
}

bool save_bag(const std::string &xml, int who, std::string &out)
{
    size_t b = bag_block(xml, 0, who);
    if (b == std::string::npos)
        return false;
    out = xml.substr(b, block_end(xml, b) - b);
    return true;
}

// Every copy: party member `who`'s bag becomes `bag` (from save_bag).
bool save_put_bag_everywhere(std::string &xml, int who, const std::string &bag)
{
    int done = 0;
    for (int copy = 0; copy < 2; ++copy) {
        size_t b = bag_block(xml, copy, who);
        if (b == std::string::npos)
            break;
        xml.replace(b, block_end(xml, b) - b, bag);
        ++done;
    }
    return done > 0;
}

// The character's name (mEdit's mNameStr), for logs.
std::string save_character_name(const std::string &character)
{
    std::string name;
    size_t i = character.find("mNameStr");
    for (int n = 0; n < 25 && i != std::string::npos; ++n) {
        i = character.find("<u8 value=\"", i);
        if (i == std::string::npos)
            break;
        i += 11;
        int v = atoi(character.c_str() + i);
        if (v == 0)
            break;
        name += char(v);
    }
    return name;
}

// ---- quest state (shared world, quest_sync.cpp) ----

namespace {
// mPlGameData children that make the quest state: active quest and branch, scenario, talk history,
// quest lists, NPC states, quest scripts' saved state and scene flags (2026-10-27, guild.sav layout)
const char *const kQuestKeys[] = {"mScenarioNo", "mActiveQuest", "mActiveBranch", "mSpeakerHistory", "mTalkHistory",
                                  "mQuestData", "mQuestSubData", "mQuestNum", "mQuestFinishData", "mQuestFinishNum",
                                  "mNpcData", "mSaveFsmFix", "mSaveFsmFixNum", "mSceSaveFlag", "mSceDone"};

// [begin, end) of child `key` (a class, an array or a single value) of the block opening at `top`.
bool quest_child(const std::string &x, size_t top, const char *key, size_t &b, size_t &e)
{
    size_t end = block_end(x, top);
    std::string pat = std::string("name=\"") + key + "\"";
    for (size_t at = x.find(pat, top); at != std::string::npos && at < end; at = x.find(pat, at + 1)) {
        size_t lt = x.rfind('<', at);
        // a direct child only: the line starts at a newline
        if (lt == std::string::npos || (lt > 0 && x[lt - 1] != '\n'))
            continue;
        b = lt;
        bool container = x.compare(lt + 1, 5, "class") == 0 || x.compare(lt + 1, 5, "array") == 0;
        size_t gt = x.find('>', lt);
        e = container && x[gt - 1] != '/' ? block_end(x, lt) : gt + 1;
        return e != std::string::npos;
    }
    return false;
}

std::vector<size_t> game_data_tops(const std::string &x)
{
    std::vector<size_t> out;
    for (size_t at = x.find("<class name=\"mPlGameData\""); at != std::string::npos; at = x.find("<class name=\"mPlGameData\"", at + 1))
        out.push_back(at);
    return out;
}
} // namespace

// The quest state of a save as text: for each player data copy, each key's block, separated by \x01.
bool save_quest_state(const std::string &xml, std::string &out)
{
    out.clear();
    std::vector<size_t> tops = game_data_tops(xml);
    if (tops.empty())
        return false;
    for (size_t top : tops)
        for (const char *k : kQuestKeys) {
            size_t b, e;
            if (!quest_child(xml, top, k, b, e))
                return false;
            out += xml.substr(b, e - b);
            out += '\x01';
        }
    return true;
}

// Puts a quest state (from save_quest_state) in place of this save's.
bool save_put_quest_state(std::string &xml, const std::string &state)
{
    std::vector<std::string> parts;
    for (size_t at = 0, nx; (nx = state.find('\x01', at)) != std::string::npos; at = nx + 1)
        parts.push_back(state.substr(at, nx - at));
    size_t per = sizeof kQuestKeys / sizeof kQuestKeys[0];
    std::vector<size_t> tops = game_data_tops(xml);
    if (tops.empty() || parts.size() != tops.size() * per)
        return false;
    for (size_t c = tops.size(); c-- > 0;) // from the end: earlier offsets stay valid
        for (size_t k = per; k-- > 0;) {
            size_t b, e;
            if (!quest_child(xml, game_data_tops(xml)[c], kQuestKeys[k], b, e))
                return false;
            xml.replace(b, e - b, parts[c * per + k]);
        }
    return true;
}

// zlib with the game's own functions: a 4-byte length, then the deflated bytes.
bool save_deflate(const std::string &in, std::vector<uint8_t> &out)
{
    out.assign(8 + in.size() + in.size() / 100 + 64, 0);
    uint32_t n = uint32_t(in.size());
    memcpy(out.data(), &n, 4);
    ZStream z = {};
    if (z_deflateInit(&z, 9, "1.2.1", int(sizeof z)) != 0)
        return false;
    z.next_in = reinterpret_cast<const uint8_t *>(in.data());
    z.avail_in = n;
    z.next_out = out.data() + 4;
    z.avail_out = uint32_t(out.size() - 4);
    int r = z_deflate(&z, Z_FINISH);
    uint32_t got = z.total_out;
    z_deflateEnd(&z);
    if (r != Z_STREAM_END)
        return false;
    out.resize(4 + got);
    return true;
}

bool save_inflate(const std::vector<uint8_t> &in, std::string &out)
{
    if (in.size() < 4)
        return false;
    uint32_t n = 0;
    memcpy(&n, in.data(), 4);
    if (n > (64u << 20))
        return false;
    out.assign(n, '\0');
    ZStream z = {};
    if (z_inflateInit(&z, "1.2.1", int(sizeof z)) != 0)
        return false;
    z.next_in = in.data() + 4;
    z.avail_in = uint32_t(in.size() - 4);
    z.next_out = reinterpret_cast<uint8_t *>(&out[0]);
    z.avail_out = n;
    int r = z_inflate(&z, Z_FINISH);
    return r == Z_STREAM_END && z.total_out == n;
}

// A quest state's quest list only (active quest, quests, sub data, finished quests, from the first
// player data copy): what is worth a reload elsewhere (talks and NPC moves change the rest often:
// the first real test reloaded the other worlds twice in 30 s for nothing).
uint32_t save_quest_list_crc(const std::string &state)
{
    std::vector<std::string> parts;
    for (size_t at = 0, nx; (nx = state.find('\x01', at)) != std::string::npos; at = nx + 1)
        parts.push_back(state.substr(at, nx - at));
    std::string list;
    for (size_t k : {size_t(1), size_t(5), size_t(6), size_t(7), size_t(8), size_t(9)}) // see kQuestKeys
        if (k < parts.size())
            list += parts[k];
    return save_crc(list.data(), list.size()) | 1;
}
