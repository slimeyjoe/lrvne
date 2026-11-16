# SPDX-License-Identifier: GPL-3.0-or-later
"""Undo Microsoft EXEPACK on an MS-DOS MZ executable.

Many DOS games were linked with EXEPACK (LINK /EXEPACK, or the stand-alone EXEPACK.EXE):
the load image is replaced by a compressed one and a small unpacker runs first. An engine
that takes tables, fonts or texts out of a game's executable when it builds its pack needs
the plain image.

The unpacker's segment (the packed file's initial CS) starts with a header:

    +00 real IP   +02 real CS   +04 (work)   +06 unpacker size   +08 real SP   +0A real SS
    +0C unpacked image size in paragraphs   [+0E skip length (some versions)]   "RB"

The compressed data ends where the unpacker's segment begins and is read backwards. Every
record ends in a command byte preceded by a 16-bit count: B0h/B1h fill (one value byte
before the count, written count times), B2h/B3h copy (count bytes before the count). The
image is rebuilt from its end down; the odd command marks the last record. FFh bytes before
the last record are padding. The relocations follow the unpacker's error message "Packed file
is corrupt": 16 groups (segments 0000h, 1000h, ... F000h), each a count and that many offsets.

  unpack(data) -> (bytes, info)
      The plain MZ file (same image, an ordinary relocation table, the original CS:IP and
      SS:SP; minimum and maximum allocation as in the packed header) and a dict describing
      it. Raises ValueError when `data` is not an EXEPACK file or is damaged.
  is_exepack(data) -> bool
  pack(image, relocs, cs_ip, ss_sp, ...) -> bytes
      An EXEPACK file of a plain image, enough for tests (the unpacker stub is a stand-in,
      not runnable code). cs_ip and ss_sp are (segment, offset) pairs.

Command line:
    python3 -I tools/lrvne exepack IN.EXE OUT.EXE
"""
import argparse
import struct
import sys
from typing import Dict, List, Optional, Sequence, Tuple

from ._util import write_file

ERROR_MESSAGE = b'Packed file is corrupt'


def _w(d: bytes, o: int) -> int:
    return struct.unpack_from('<H', d, o)[0]


def _stub(data: bytes) -> Tuple[int, int, int]:
    """(start of the image, start of the unpacker, offset of "RB" in it), or ValueError."""
    if len(data) < 0x1C or data[:2] not in (b'MZ', b'ZM'):
        raise ValueError('not an MZ executable')
    image = _w(data, 8) * 16
    stub = image + _w(data, 0x16) * 16
    for sig in (stub + 14, stub + 16):
        if data[sig:sig + 2] == b'RB':
            return image, stub, sig
    raise ValueError('no EXEPACK header at the entry segment')


def is_exepack(data: bytes) -> bool:
    """Whether `data` is an EXEPACK-compressed MZ executable."""
    try:
        _stub(data)
    except (ValueError, struct.error):
        return False
    return True


def unpack(data: bytes) -> Tuple[bytes, Dict[str, object]]:
    """The plain MZ file of an EXEPACK file, and a description of it."""
    try:
        return _unpack(data)
    except (IndexError, struct.error):
        raise ValueError('the EXEPACK data is damaged') from None


def _unpack(data: bytes) -> Tuple[bytes, Dict[str, object]]:
    image, stub, sig = _stub(data)
    ip, cs, _work, stub_size, sp, ss, paras = struct.unpack_from('<7H', data, stub)
    skip = _w(data, stub + 14) if sig == stub + 16 else 1
    packed = data[image:stub]       # read from a copy, as the unpacker first moves it away
    out = bytearray(paras * 16)
    out[:len(packed)] = packed[:len(out)]   # what no record covers keeps the loaded bytes
    src = len(packed) - (skip - 1) * 16
    dst = len(out)
    while packed[src - 1] == 0xFF:
        src -= 1
    while True:
        cmd = packed[src - 1]
        count = _w(packed, src - 3)
        src -= 3
        if cmd & 0xFE == 0xB0:
            src -= 1
            dst -= count
            out[dst:dst + count] = bytes([packed[src]]) * count
        elif cmd & 0xFE == 0xB2:
            src -= count
            dst -= count
            out[dst:dst + count] = packed[src:src + count]
        else:
            raise ValueError('unknown EXEPACK command %02Xh' % cmd)
        if src < 0 or dst < 0:
            raise ValueError('the EXEPACK data is damaged')
        if cmd & 1:
            break

    end = stub + stub_size
    msg = data.find(ERROR_MESSAGE, stub, end)
    if msg < 0:
        raise ValueError('EXEPACK relocation table not found')
    p = msg + len(ERROR_MESSAGE)
    relocs: List[Tuple[int, int]] = []
    for group in range(16):
        n = _w(data, p)
        p += 2
        for _ in range(n):
            relocs.append((_w(data, p), group << 12))
            p += 2
    if p != end:
        raise ValueError('the relocation table ends at %Xh, the unpacker at %Xh' % (p, end))

    head = (0x1C + 4 * len(relocs) + 0x1FF) & ~0x1FF
    total = head + len(out)
    mz = bytearray(head)
    struct.pack_into('<2s13H', mz, 0, b'MZ', total & 0x1FF, (total + 0x1FF) // 0x200,
                     len(relocs), head // 16, _w(data, 0x0A), _w(data, 0x0C), ss, sp, 0, ip, cs,
                     0x1C, 0)
    for i, (off, seg) in enumerate(relocs):
        struct.pack_into('<HH', mz, 0x1C + 4 * i, off, seg)
    info = {'image': len(out), 'relocs': len(relocs), 'cs:ip': '%04X:%04X' % (cs, ip),
            'ss:sp': '%04X:%04X' % (ss, sp)}
    return bytes(mz) + bytes(out), info


def _records(image: bytes) -> List[bytes]:
    """The records of `image` in file order: runs of 8 or more equal bytes become fills."""
    recs: List[bytes] = []
    i, n, lit = 0, len(image), 0
    while i < n:
        j = i
        while j < n and image[j] == image[i] and j - i < 0xFFFF:
            j += 1
        if j - i >= 8:
            if lit < i:
                recs.append(image[lit:i] + struct.pack('<HB', i - lit, 0xB2))
            recs.append(bytes([image[i]]) + struct.pack('<HB', j - i, 0xB0))
            lit = i = j
        else:
            i += 1
            if i - lit == 0xFFFF:
                recs.append(image[lit:i] + struct.pack('<HB', i - lit, 0xB2))
                lit = i
    if lit < n:
        recs.append(image[lit:n] + struct.pack('<HB', n - lit, 0xB2))
    return recs


def pack(image: bytes, relocs: Sequence[Tuple[int, int]] = (), cs_ip: Tuple[int, int] = (0, 0),
         ss_sp: Tuple[int, int] = (0, 0x100), min_alloc: int = 0, max_alloc: int = 0xFFFF,
         skip_field: bool = False) -> bytes:
    """An EXEPACK file of `image` (padded to whole paragraphs) for tests. relocs are
    (offset, segment) pairs, segment a multiple of 1000h. The unpacker stub holds the header,
    the error message and the relocation table, but no code."""
    image = bytes(image) + bytes(-len(image) % 16)
    recs = _records(image)
    first = recs[0]
    recs[0] = first[:-1] + bytes([first[-1] | 1])          # the record processed last
    stream = b''.join(recs)
    stream += b'\xFF' * (-len(stream) % 16)
    groups: List[List[int]] = [[] for _ in range(16)]
    for off, seg in relocs:
        if seg & 0x0FFF or not 0 <= seg <= 0xF000:
            raise ValueError('relocation segment %Xh is not a multiple of 1000h' % seg)
        groups[seg >> 12].append(off)
    table = b''.join(struct.pack('<H', len(g)) + struct.pack('<%dH' % len(g), *g) for g in groups)
    header = struct.pack('<7H', cs_ip[1], cs_ip[0], 0, 0, ss_sp[1], ss_sp[0], len(image) // 16)
    if skip_field:
        header += struct.pack('<H', 1)
    body = header + b'RB' + b'\x90' * 16 + ERROR_MESSAGE + table
    body = body[:6] + struct.pack('<H', len(body)) + body[8:]
    header_paras = 2
    total = header_paras * 16 + len(stream) + len(body)
    mz = struct.pack('<2s13H', b'MZ', total & 0x1FF, (total + 0x1FF) // 0x200, 0, header_paras,
                     min_alloc, max_alloc, 0, 0x80, 0, 0x10, len(stream) // 16, 0x1C, 0)
    mz += bytes(header_paras * 16 - len(mz))
    return mz + stream + body


def main(argv: Optional[Sequence[str]] = None) -> int:
    """The command line (see the module's description)."""
    ap = argparse.ArgumentParser(prog='lrvne exepack', description=__doc__.split('\n')[0])
    ap.add_argument('input')
    ap.add_argument('output')
    a = ap.parse_args(argv)
    try:
        with open(a.input, 'rb') as f:
            data, info = unpack(f.read())
        write_file(a.output, data)
    except (ValueError, OSError) as e:
        print('exepack: %s' % e, file=sys.stderr)
        return 1
    print('%s: %s' % (a.output, ', '.join('%s %s' % kv for kv in info.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
