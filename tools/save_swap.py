"""Test saves only: swaps the Arisen with pawn N (mEdit and mParam) in both player data copies, and
puts the new Arisen's mEdit in mSystemData > mEditPl, the way the mod does (savecodec.cpp).

Usage: python -I save_swap.py <in.sav> <pawn 0|1|2> <out.sav>
Example: guild.sav 2 -> the Arisen is Kaliste (sorcerer), for the spell tests.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # run with -I
from save_merge import block, child, pack, unpack


def owners(lines, start, pawn):
    """(arisen block start, pawn block start) in the copy whose mPlCmcEditAndParam is at `start`"""
    arisen = child(lines, start, 'mPl')[0]
    arr = next(i for i in range(start, len(lines)) if lines[i].strip().startswith('<array name="mCmc"'))
    owner = arr + 1
    for _ in range(pawn):
        owner = block(lines, owner) + 1
    assert lines[owner].strip().startswith('<class type="cSAVE_DATA_CMC"')
    return arisen, owner


def parts(lines, owner):
    return child(lines, owner, 'mEdit'), child(lines, owner, 'mParam')


if __name__ == '__main__':
    src, pawn, dst = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    head, xml = unpack(src)
    lines = xml.split('\n')
    tops = [i for i, l in enumerate(lines) if l.strip().startswith('<class name="mPlCmcEditAndParam"')][:2]
    # take both characters from the first copy, then write them over both copies (bottom copy first:
    # line numbers above stay valid)
    a0, p0 = owners(lines, tops[0], pawn)
    (ae, ap), (pe, pp) = parts(lines, a0), parts(lines, p0)
    arisen = (lines[ae[0]:ae[1] + 1], lines[ap[0]:ap[1] + 1])
    other = (lines[pe[0]:pe[1] + 1], lines[pp[0]:pp[1] + 1])
    for top in sorted(tops, reverse=True):
        a, p = owners(lines, top, pawn)
        for owner, (edit, param) in sorted(((a, other), (p, arisen)), key=lambda t: -t[0]):
            (e0, e1), (q0, q1) = parts(lines, owner)
            lines[q0:q1 + 1] = param  # mParam after mEdit: replaced first
            lines[e0:e1 + 1] = edit
    sysd = next(i for i, l in enumerate(lines) if l.strip().startswith('<class name="mSystemData"'))
    s0, s1 = child(lines, sysd, 'mEditPl')
    pl = list(other[0])
    pl[0] = pl[0].replace('name="mEdit"', 'name="mEditPl"', 1)
    lines[s0:s1 + 1] = pl
    open(dst, 'wb').write(pack(head, '\n'.join(lines)))
    print(f'{src}: Arisen and pawn {pawn} swapped -> {dst}')
