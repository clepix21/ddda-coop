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

// The owner block (mPl, or mCmc[pawn]) inside mPlayerDataManual's mPlCmcEditAndParam.
size_t owner_block(const std::string &x, int pawn)
{
    size_t top = x.find("<class name=\"mPlCmcEditAndParam\"");
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

// The mEdit and mParam blocks of the Arisen (pawn -1) or of a pawn, as one string (mEdit then mParam).
bool save_character(const std::string &xml, int pawn, std::string &out)
{
    size_t o = owner_block(xml, pawn);
    if (o == std::string::npos)
        return false;
    size_t e = child(xml, o, "mEdit"), p = child(xml, o, "mParam");
    if (e == std::string::npos || p == std::string::npos)
        return false;
    out = xml.substr(e, block_end(xml, e) - e) + "\n" + xml.substr(p, block_end(xml, p) - p);
    return true;
}

// Writes a character (from save_character) over pawn `pawn`'s mEdit and mParam.
bool save_put_character(std::string &xml, int pawn, const std::string &character)
{
    size_t split = character.find("<class name=\"mParam\"");
    if (split == std::string::npos || character.compare(0, 19, "<class name=\"mEdit\"") != 0)
        return false;
    std::string edit = character.substr(0, character.find_last_not_of('\n', split - 1) + 1);
    std::string param = character.substr(split);
    size_t o = owner_block(xml, pawn);
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
