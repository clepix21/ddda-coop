"""Offline prototype of the "a player's character replaces a pawn" save merge (tests only).

Usage: python -I save_merge.py <base.sav> <character.sav> <pawn 0|1|2> <out.sav> [--from-pawn N]
Takes the Arisen (mPl: mEdit appearance and name, mParam level, vocation, skills, equipment) of
<character.sav> (or its pawn N with --from-pawn) and writes it over pawn <pawn> (mCmc[pawn]) of
<base.sav>, then repacks the save the way the game writes it: a 32-byte header (u32 version 0x15,
uncompressed size, compressed size, three values kept from the base, CRC-32 of the compressed data
without the final inversion) and the zlib-compressed XML, padded with zeros to 524288 bytes.
"""
import re
import struct
import sys
import zlib

SAVE_SIZE = 524288


def unpack(path):
    d = open(path, 'rb').read()
    head = list(struct.unpack_from('<8I', d, 0))
    z = zlib.decompressobj()
    xml = z.decompress(d[32:])
    used = len(d) - 32 - len(z.unused_data)
    assert head[1] == len(xml) and head[2] == used, 'unexpected header'
    assert head[6] == (zlib.crc32(d[32:32 + used]) ^ 0xFFFFFFFF), 'unexpected checksum'
    return head, xml.decode('utf-8')


def pack(head, xml):
    raw = xml.encode('utf-8')
    comp = zlib.compress(raw, 9)
    head = list(head)
    head[1], head[2], head[6] = len(raw), len(comp), zlib.crc32(comp) ^ 0xFFFFFFFF
    out = struct.pack('<8I', *head) + comp
    assert len(out) <= SAVE_SIZE, f'{len(out)} bytes do not fit in {SAVE_SIZE}'
    return out + bytes(SAVE_SIZE - len(out))


def block(lines, start):
    """index of the line closing the class/array opened at `start`"""
    depth = 0
    for i in range(start, len(lines)):
        s = lines[i].strip()
        if (s.startswith('<class') or s.startswith('<array')) and not s.endswith('/>'):
            depth += 1
        elif s.startswith('</class') or s.startswith('</array'):
            depth -= 1
            if depth == 0:
                return i
    raise ValueError('unclosed block')


def child(lines, start, name):
    """(first, last) lines of the direct child class `name` of the block at `start`"""
    end = block(lines, start)
    i = start + 1
    while i < end:
        s = lines[i].strip()
        if s.startswith(f'<class name="{name}"'):
            return i, block(lines, i)
        if (s.startswith('<class') or s.startswith('<array')) and not s.endswith('/>'):
            i = block(lines, i)
        i += 1
    raise KeyError(name)


def character(lines, pawn=None):
    """line ranges of mEdit and mParam of the Arisen (pawn None) or of mCmc[pawn], in mPlayerDataManual"""
    top = next(i for i, l in enumerate(lines) if l.strip().startswith('<class name="mPlCmcEditAndParam"'))
    if pawn is None:
        owner = child(lines, top, 'mPl')[0]
    else:
        arr = next(i for i in range(top, len(lines)) if lines[i].strip().startswith('<array name="mCmc"'))
        owner, n = arr + 1, 0
        while n < pawn:
            owner = block(lines, owner) + 1
            n += 1
        assert lines[owner].strip().startswith('<class type="cSAVE_DATA_CMC"')
    return child(lines, owner, 'mEdit'), child(lines, owner, 'mParam')


if __name__ == '__main__':
    base_path, char_path, pawn, out_path = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    src_pawn = int(sys.argv[sys.argv.index('--from-pawn') + 1]) if '--from-pawn' in sys.argv else None
    head, base = unpack(base_path)
    _, other = unpack(char_path)
    b, o = base.split('\n'), other.split('\n')
    (oe0, oe1), (op0, op1) = character(o, src_pawn)
    (be0, be1), (bp0, bp1) = character(b, pawn)
    assert be1 < bp0, 'mEdit before mParam'
    merged = b[:be0] + o[oe0:oe1 + 1] + b[be1 + 1:bp0] + o[op0:op1 + 1] + b[bp1 + 1:]
    name = bytes(int(v) for v in re.findall(r'<u8 value="(\d+)"/>', '\n'.join(o[oe0:oe0 + 27]))).split(b'\0')[0]
    job = re.search(r'name="mJob" value="(\d+)"', '\n'.join(o[op0:op0 + 5]))
    open(out_path, 'wb').write(pack(head, '\n'.join(merged)))
    print(f'pawn {pawn} of {base_path} is now {name.decode(errors="replace")} (vocation {job[1] if job else "?"}) -> {out_path}')
