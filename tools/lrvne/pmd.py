# SPDX-License-Identifier: GPL-3.0-or-later
"""PMD 4.7g data for lrvne's PMD module (include/lrvne/pmd.h): songs and the SSG-effect table.

PMD (M. Kajihara's music driver for the PC-98) plays compiled songs, ".M" files. The engines
that use lrvne's PMD module build their packs with this module:

  ssg_effect_table(pmd_com)
      The driver's built-in SSG effects (drums, noises), taken from the player's own PMD.COM
      4.7g: the table at CS:2CE9h..3095h. The module needs it for music command D4h, for the
      drums of rhythm part K and for INT 60h AH=03h; the engine puts it into its pack.
      Raises ValueError for any other file than PMD.COM 4.7g (checked by MD5).

lrvne's tests use the rest: songs written here byte by byte, which exercise the commands and
parts the module implements, and a table of SSG effects of our own in PMD's format, so that
the tests need neither a game's music nor PMD.COM's table.

  build_song(parts, rhythm=(), voices=())
      A song in the format MC 4.7 compiles to (the file, type byte first): parts maps 'A'..'K'
      to the bytes of a part without its terminating 80h, rhythm is the list of the R
      patterns rhythm part K plays (each without its terminating FFh; none: one empty
      pattern), voices the 26-byte FM voice records (number, then 25 register bytes).
  synth_songs()
      {'SYNTH1.M': bytes, ...}: the test songs.
  synth_ssg_table()
      An SSG-effect table of 14 effects (0..13, one per drum bit of part K), in the format of
      PMD's own: 3-byte entries {priority, u16 offset from the table start}, then the effects,
      each a list of 11-byte frames ended by FFh (docs/pmd.md).

Command line:
    python3 -I tools/lrvne pmd ssgtab PMD.COM OUT       the table out of PMD.COM
    python3 -I tools/lrvne pmd synth DIR                 the test songs and table into DIR
    python3 -I tools/lrvne pmd check DIR                 DIR holds exactly what synth writes
"""
import argparse
import hashlib
import os
import struct
import sys

PMD_COM_MD5 = 'cd5634a197a3163734722da554ddadf8'     # PMD.COM 4.7g, the 26K build
SSG_TABLE_START = 0x2CE9                            # CS offsets (a .COM: file offset + 100h)
SSG_TABLE_END = 0x3096
PART_NAMES = 'ABCDEFGHIJK'
HEADER = 0x1A                                       # 11 part offsets, R table, voice table


def ssg_effect_table(pmd_com: bytes) -> bytes:
    """The built-in SSG-effect table of PMD.COM 4.7g."""
    if hashlib.md5(pmd_com).hexdigest() != PMD_COM_MD5:
        raise ValueError('not PMD.COM 4.7g (MD5 %s expected)' % PMD_COM_MD5)
    return pmd_com[SSG_TABLE_START - 0x100:SSG_TABLE_END - 0x100]


def build_song(parts, rhythm=(), voices=()) -> bytes:
    """A compiled song: type byte 00h, then the data D the offsets are relative to."""
    data = bytearray(HEADER)
    offsets = []
    for name in PART_NAMES:
        offsets.append(len(data))
        data += bytes(parts.get(name, b'')) + b'\x80'
    rtab = len(data)
    patterns = list(rhythm) or [b'']
    pos = rtab + 2 * len(patterns)
    table, body = bytearray(), bytearray()
    for pat in patterns:
        table += struct.pack('<H', pos + len(body))
        body += bytes(pat) + b'\xFF'
    data += table + body
    memo = len(data)
    data += b'\x00\x00'                             # the memo table: empty
    data += struct.pack('<H', memo) + b'\x47\xFE'   # memo pointer, version 4.7, marker
    vtab = len(data)
    for v in voices:
        data += bytes(v)
    data += b'\x00\xFF'
    for i, off in enumerate(offsets):
        struct.pack_into('<H', data, 2 * i, off)
    struct.pack_into('<HH', data, 0x16, rtab, vtab)
    return b'\x00' + bytes(data)


def _note(octave, key, length):
    return bytes([(octave << 4) | key, length])


# FM voices: number, DT/MUL x4, TL x4, KS/AR x4, AM/DR x4, SR x4, SL/RR x4, FB/ALG
VOICE1 = (1, 0x71, 0x32, 0x31, 0x02, 0x20, 0x18, 0x30, 0x05, 0x1F, 0x1F, 0x1F, 0x1F,
          0x05, 0x08, 0x05, 0x08, 0x02, 0x03, 0x02, 0x03, 0x26, 0x27, 0x26, 0x27, 0x3C)
VOICE2 = (2, 0x01, 0x02, 0x04, 0x01, 0x10, 0x20, 0x00, 0x00, 0x1C, 0x1D, 0x1E, 0x1F,
          0x0A, 0x0B, 0x0C, 0x0D, 0x00, 0x01, 0x02, 0x03, 0x14, 0x15, 0x16, 0x17, 0x07)


def _synth1():
    """FM and SSG commands: LFOs and their waves, depth modulation, envelopes, key-on delay,
    the FM3 parts, slot detune, portamento, masks, one-note volumes."""
    n = _note
    a = bytearray([0xFC, 0xB0, 0xFF, 1, 0xFD, 100, 0xFE, 2]) + n(4, 0, 12)
    a += bytes([0xC4, 0x80]) + n(4, 2, 16) + n(4, 4, 9)
    a += bytes([0xC4, 0x00, 0xB5, 0x03, 0x03]) + n(4, 5, 12)
    for fb in (0x05, 0x81, 0xFE):
        a += bytes([0xB6, fb]) + n(4, 7, 8)
    a += bytes([0xCB, 0x04, 0xF2, 0x00, 0x01, 0x08, 0x03, 0xF1, 0x01]) + n(4, 9, 30)
    a += bytes([0xCB, 0x05, 0xF2, 0x02, 0x02, 0x03, 0x02]) + n(5, 0, 30)
    a += bytes([0xCB, 0x06, 0xF2, 0x00, 0x01, 0x05, 0x06]) + n(5, 0, 20)
    a += bytes([0xCB, 0x01, 0xF2, 0x00, 0x01, 0x04, 0x04]) + n(4, 0, 30)
    a += bytes([0xCB, 0x00, 0xD6, 0x02, 0x01, 0xB7, 0x05, 0xF2, 0x00, 0x01, 0x04, 0x02])
    a += n(4, 0, 40)
    a += bytes([0xD6, 0x01, 0xFF, 0xB7, 0x00]) + n(4, 0, 20)
    a += bytes([0xF1, 0x06, 0xF2, 0x03, 0x01, 0x10, 0x02, 0xC5, 0x00]) + n(4, 0, 24) + n(4, 2, 24)
    a += bytes([0xF1, 0x00, 0xBF, 0x01, 0x01, 0x06, 0x02, 0xBC, 0x02, 0xBE, 0x01, 0xBA, 0x03])
    a += n(4, 4, 30)
    a += bytes([0xBE, 0x00, 0xC0, 0x01]) + n(4, 5, 12) + bytes([0xC0, 0x00]) + n(4, 5, 12)
    a += bytes([0xDE, 0x10]) + n(4, 7, 8) + n(4, 7, 8)
    a += bytes([0xDD, 0x30]) + n(4, 7, 8) + n(0, 0x0F, 4) + n(4, 7, 8)
    a += bytes([0xE7, 0x02, 0xD5, 0x05, 0x00]) + n(4, 0, 8) + b'\xC1' + n(4, 0, 8)
    a += bytes([0xB8, 0x82, 0x05, 0xB8, 0x01, 0x10]) + n(4, 0, 8)
    a += bytes([0xFF, 2, 0xCF, 0x50]) + n(4, 0, 12) + bytes([0xCF, 0xF0])
    a += bytes([0xC2, 0x05, 0xF1, 0x01]) + n(3, 0, 20)
    a += bytes([0xF1, 0x00, 0xFC, 0xFE, 0x10, 0xE3, 0x05, 0xE2, 0x02, 0xF4, 0xF3]) + n(5, 0, 12)
    a += bytes([0xDA, 0x40, 0x60, 0x20, 0xDA, 0x60, 0x34, 0x0D])
    a += bytes([0xF6]) + n(4, 0, 12) + n(4, 4, 12)
    c = bytearray([0xCF, 0x30, 0xC8, 0x03, 0x00, 0x00, 0xFF, 1, 0xFD, 90, 0xC5, 0x01,
                   0xF2, 0x00, 0x01, 0x05, 0x03, 0xF1, 0x01])
    c += n(4, 0, 24) + n(4, 4, 24) + bytes([0xC7, 0x01, 0x03, 0x00]) + n(4, 7, 24)
    c += bytes([0xC5, 0x00, 0xF1, 0x00]) + n(4, 0, 30)
    d = bytearray([0xCF, 0xC0, 0xC8, 0x0C, 0x05, 0x00, 0xFF, 1, 0xFD, 90])
    d += n(3, 7, 30) + n(3, 4, 30) + bytes([0xCF, 0x00]) + n(3, 0, 20) + bytes([0xCF, 0xC0])
    d += n(3, 7, 30)
    g = bytearray([0xFD, 12, 0xF0, 0x02, 0xFF, 0x02, 0x01, 0xFE, 1]) + n(4, 0, 20) + n(4, 4, 20)
    g += bytes([0xF0, 0x00, 0xFE, 0x03, 0x00]) + n(4, 7, 20)
    g += bytes([0xED, 0x38, 0xEE, 0x10]) + n(4, 0, 10) + bytes([0xD0, 0x05]) + n(4, 0, 10)
    g += bytes([0xED, 0x07])
    g += bytes([0xFA, 0x05, 0x00]) + n(4, 0, 10) + bytes([0xCC, 0x01]) + n(4, 0, 10)
    g += bytes([0xFA, 0x00, 0x00, 0xCC, 0x00])
    g += bytes([0xF2, 0x00, 0x01, 0x02, 0x02, 0xF1, 0x03]) + n(4, 5, 30) + bytes([0xF1, 0x00])
    g += bytes([0xCD, 0x1C, 0x12, 0x08, 0x84, 0x03]) + n(4, 0, 30) + n(4, 2, 30)
    g += bytes([0xC9, 0x01]) + n(4, 4, 30) + bytes([0xC9, 0x00])
    g += bytes([0xF0, 0x01, 0x01, 0x01, 0x01]) + n(4, 0, 20)
    g += bytes([0xC4, 0x40]) + n(4, 7, 20) + bytes([0xC4, 0x00, 0xDE, 0x02]) + n(4, 7, 10)
    g += n(4, 7, 10)
    g += bytes([0xC0, 0x01]) + n(4, 0, 10) + bytes([0xC0, 0x00]) + n(4, 0, 10)
    g += bytes([0xDA, 0x40, 0x50, 0x10])
    h = bytearray([0xFD, 10, 0xCD, 0x14, 0x10, 0x10, 0x31, 0x0F, 0xCA, 0x01,
                   0xF2, 0x00, 0x02, 0x01, 0x03, 0xF1, 0x01, 0xCC, 0x01, 0xFA, 0x02, 0x00])
    h += n(5, 0, 40) + n(5, 4, 40)
    h += bytes([0xCB, 0x02]) + n(5, 0, 40) + bytes([0xCB, 0x03]) + n(5, 2, 40)
    return build_song({'A': a, 'C': c, 'D': d, 'G': g, 'H': h}, voices=[VOICE1, VOICE2])


def _synth2():
    """Tempo in all its forms, the volume-down subcommands of C0h, the measure length DFh."""
    n = _note
    a = bytearray([0xFF, 1, 0xFD, 110, 0xFC, 0xFF, 120]) + n(4, 0, 24)
    a += bytes([0xFC, 0xFD, 0x10]) + n(4, 2, 24) + bytes([0xFC, 0xFD, 0xF0]) + n(4, 4, 24)
    for v in ((0xFF, 0x30), (0xFE, 0xF0), (0xFE, 0x00), (0xFE, 0x20)):
        a += bytes([0xC0, v[0], v[1]]) + n(4, 0, 24)
    a += bytes([0xFC, 0xFE, 0xF6, 0xDF, 0x30, 0xFC, 0x10]) + n(4, 0, 24)
    a += bytes([0xFC, 0xFF, 0x05]) + n(4, 0, 12) + bytes([0xFC, 0xFF, 0xFF]) + n(4, 0, 12)
    g = bytearray([0xFD, 14, 0xCD, 0x1F, 0x00, 0x00, 0x0F, 0x0F]) + n(4, 0, 48)
    for v in ((0xFD, 0x40), (0xFC, 0x40), (0xFC, 0x00)):
        g += bytes([0xC0, v[0], v[1]]) + n(4, 0, 48)
    return build_song({'A': a, 'G': g}, voices=[VOICE1])


def _drum(mask, length):
    """An R-pattern drum: bits 13..0 of the drums to play, then the length."""
    return bytes([0x80 | (mask >> 8), mask & 0xFF, length])


def _synth3():
    """Rhythm part K (patterns, every drum bit, commands in the K stream and in patterns, its
    mask, its loop), the measure counter (DFh in a part and in a pattern), SSG effects started
    by the music (D4h) and their priorities against the drums and part I, the effect commands
    in a masked part, and an invalid C0h subcommand."""
    n = _note
    a = bytearray([0xFF, 1, 0xFD, 100, 0xFC, 0xC0, 0xDF, 0x30])
    a += n(4, 0, 24) + n(4, 4, 24) + bytes([0xD4, 0x03]) + n(4, 7, 24)
    a += bytes([0xD4, 0x05]) + n(5, 0, 48) + bytes([0xD4, 0x00]) + n(4, 7, 24)
    a += bytes([0xD4, 0x00]) + n(4, 4, 24)                   # stop with none running
    a += bytes([0xC0, 0x01, 0xD4, 0x02, 0xC0, 0x00]) + n(4, 0, 48)   # masked: no effect
    a += bytes([0xF6]) + n(4, 0, 24) + n(4, 2, 24) + n(4, 4, 24) + n(4, 5, 24)
    b = bytearray([0xFF, 2, 0xFD, 90]) + n(3, 0, 96) + bytes([0xC0, 0x02, 0x00]) + n(3, 4, 96)
    g = bytearray([0xFD, 12, 0xCD, 0x1F, 0x00, 0x00, 0x0F, 0x0F, 0xF6])
    g += n(4, 0, 48) + n(4, 7, 48) + n(0, 0x0F, 24) + n(5, 0, 24)
    i = bytearray([0xFD, 11, 0xCD, 0x1F, 0x08, 0x02, 0x2F, 0x0F, 0xF6])
    i += n(3, 0, 24) + n(0, 0x0F, 48) + n(3, 7, 12) + n(0, 0x0F, 12) + n(3, 4, 96)
    pats = [
        # 0: every drum bit once, one with several bits, a rest
        b''.join(_drum(1 << bit, 6) for bit in range(14)) + _drum(0x0006, 6) + bytes([0x05, 12]),
        # 1: commands inside a pattern: volume, measure length, status1, then drums and a rest
        bytes([0xFD, 0x08, 0xDF, 0x10, 0xDB, 0x01]) + _drum(0x0003, 12) + bytes([0x00, 12]),
        # 2: the part's mask from inside a pattern, and a drum with no bits
        bytes([0xC0, 0x01]) + _drum(0x0001, 6) + _drum(0x0002, 6) + bytes([0xC0, 0x00])
        + _drum(0x0004, 6) + _drum(0x0000, 6),
        # 3: one long drum, for the effects to run their course
        _drum(0x0001, 48),
        # 4: empty
        b'',
        # 5: priority-2 effect, then drums that must not replace it
        _drum(0x0020, 4) + _drum(0x0001, 4) + _drum(0x0002, 4) + bytes([0x01, 24]),
    ]
    k = bytes([0xDF, 0x20, 0xFD, 0x0E, 0xF6, 0x00, 0x01, 0x04, 0x02, 0x05, 0x03,
               0xDC, 0x03, 0x00, 0xC0, 0x01, 0x01, 0xC0, 0x00, 0xE6, 0x10, 0x03])
    return build_song({'A': a, 'B': b, 'G': g, 'I': i, 'K': k}, rhythm=pats, voices=[VOICE1, VOICE2])


def synth_songs():
    """The test songs, by file name."""
    return {'SYNTH1.M': _synth1(), 'SYNTH2.M': _synth2(), 'SYNTH3.M': _synth3()}


def synth_ssg_table() -> bytes:
    """14 SSG effects of our own in the format of PMD's built-in table."""
    effects = []
    for e in range(14):
        frames = []
        nframes = 1 + e % 3
        for f in range(nframes):
            dur = 2 + (e + 3 * f) % 5
            tone = 0x80 + 0x23 * e + 0x40 * f
            noise = (e * 3 + f) & 0x1F
            mixer = (0, 1, 8)[(e + f) % 3]                  # tone+noise, noise only, tone only
            vol = 0x10 if (e == 7 and f == 0) else 15 - ((e + f) % 6)
            env = 0x0300 + 0x40 * e
            shape = (0x00, 0x09, 0x0D, 0x0E)[(e + f) % 4]
            delta = ((-3 if e % 2 else 2) * (1 + f)) & 0xFF
            sweep = ((0xF0 if e % 4 == 1 else 0x10) | (1 + f)) if e % 4 in (1, 3) else 0
            frames.append(bytes([dur, tone & 0xFF, tone >> 8, noise, mixer, vol,
                                 env & 0xFF, env >> 8, shape, delta, sweep]))
        effects.append(b''.join(frames) + b'\xFF')
    priority = [2 if e in (5, 9) else 1 for e in range(14)]
    table = bytearray(3 * len(effects))
    body = bytearray()
    for e, frames in enumerate(effects):
        struct.pack_into('<BH', table, 3 * e, priority[e], len(table) + len(body))
        body += frames
    return bytes(table + body)


def _synth_files():
    files = dict(synth_songs())
    files['SYNTH.SSG'] = synth_ssg_table()
    return files


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog='pmd', description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('ssgtab', help="the SSG-effect table out of PMD.COM 4.7g")
    p.add_argument('pmd_com')
    p.add_argument('out')
    p = sub.add_parser('synth', help='write the test songs and the test SSG-effect table')
    p.add_argument('dir')
    p = sub.add_parser('check', help='a directory holds exactly what synth writes')
    p.add_argument('dir')
    a = ap.parse_args(argv)
    if a.cmd == 'ssgtab':
        with open(a.pmd_com, 'rb') as f:
            try:
                table = ssg_effect_table(f.read())
            except ValueError as e:
                print('pmd: %s: %s' % (a.pmd_com, e), file=sys.stderr)
                return 1
        with open(a.out, 'wb') as f:
            f.write(table)
        return 0
    if a.cmd == 'synth':
        os.makedirs(a.dir, exist_ok=True)
        for name, data in _synth_files().items():
            with open(os.path.join(a.dir, name), 'wb') as f:
                f.write(data)
        return 0
    bad = 0
    for name, data in _synth_files().items():
        path = os.path.join(a.dir, name)
        try:
            with open(path, 'rb') as f:
                same = f.read() == data
        except OSError:
            same = False
        if not same:
            print('pmd: %s differs from what synth writes' % path, file=sys.stderr)
            bad = 1
    return bad


if __name__ == '__main__':
    sys.exit(main())
