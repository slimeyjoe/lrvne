# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of lrvne.xfont: BDF, PCF and extra-glyph input, emboldening, the XFNT bytes."""
import contextlib
import gzip
import io
import os
import struct
import tempfile
import unittest

from lrvne import xfont

# An 8x16 font with two glyphs. 'A' (0x41) is a 5x3 box one pixel in from the left, standing on
# the baseline; 0xB1 is a full-height bar in column 1.
BDF8 = """STARTFONT 2.1
FONT -test-fixture-medium-r-normal--16-150-75-75-c-80-jisx0201.1976-0
SIZE 16 75 75
FONTBOUNDINGBOX 8 16 0 -2
STARTPROPERTIES 2
FONT_ASCENT 14
FONT_DESCENT 2
ENDPROPERTIES
CHARS 3
STARTCHAR A
ENCODING 65
SWIDTH 480 0
DWIDTH 8 0
BBX 5 3 1 0
BITMAP
F8
88
F8
ENDCHAR
STARTCHAR bar
ENCODING 177
SWIDTH 480 0
DWIDTH 8 0
BBX 1 16 1 -2
BITMAP
""" + '80\n' * 16 + """ENDCHAR
STARTCHAR unencoded
ENCODING -1
SWIDTH 480 0
DWIDTH 8 0
BBX 1 1 0 0
BITMAP
80
ENDCHAR
ENDFONT
"""

# A 16x16 font with one glyph at JIS 0x2121 that uses column 0 and has a 1-0-1 gap.
BDF16 = """STARTFONT 2.1
FONTBOUNDINGBOX 16 16 0 -2
STARTPROPERTIES 1
FONT_ASCENT 14
ENDPROPERTIES
CHARS 1
STARTCHAR jis2121
ENCODING 8481
BBX 16 16 0 -2
BITMAP
8005
""" + '0000\n' * 15 + """ENDCHAR
ENDFONT
"""

ART = """# extra glyphs for the tests
#
U+0104 Ą a comment may follow the code
........
...##...
""" + '........\n' * 13 + """.......#

U+0041 replaces the font's A
""" + '#.......\n' * 16

A_ROWS = [0] * 11 + [0x7C, 0x44, 0x7C] + [0, 0]
BAR_ROWS = [0x40] * 16


def make_pcf(glyphs, ascent=14, descent=2, big_endian=False):
    """A minimal PCF font (compressed metrics, MSB-first bitmaps padded to bytes) of 8-wide
    glyphs: {code: 16 row bytes}."""
    order = '>' if big_endian else '<'
    fmt_bits = 4 if big_endian else 0
    codes = sorted(glyphs)
    metrics = struct.pack('<I', 0x100 | fmt_bits) + struct.pack(order + 'H', len(codes))
    bitmaps = b''
    offsets = []
    for c in codes:
        metrics += bytes((0x80, 0x80 + 8, 0x80 + 8, 0x80 + ascent, 0x80 + descent))
        offsets.append(len(bitmaps))
        bitmaps += bytes(glyphs[c])
    bitmap_table = struct.pack('<I', 8 | fmt_bits) + struct.pack(order + 'I', len(codes)) + \
        struct.pack(order + '%dI' % len(codes), *offsets) + \
        struct.pack(order + '4I', len(bitmaps), 0, 0, 0) + bitmaps
    lo, hi = codes[0], codes[-1]
    index = [codes.index(c) if c in glyphs else 0xFFFF for c in range(lo, hi + 1)]
    encodings = struct.pack('<I', fmt_bits) + struct.pack(order + 'HHHHh', lo, hi, 0, 0, 0) + \
        struct.pack(order + '%dH' % len(index), *index)
    tables = [(4, 0x100 | fmt_bits, metrics), (8, 8 | fmt_bits, bitmap_table),
              (0x20, fmt_bits, encodings)]
    out = b'\x01fcp' + struct.pack('<I', len(tables))
    at = 8 + 16 * len(tables)
    body = b''
    for typ, fmt, data in tables:
        out += struct.pack('<IIII', typ, fmt, len(data), at + len(body))
        body += data
    return out + body


class Convert(unittest.TestCase):
    def test_bdf_to_xfnt_bytes(self):
        t = xfont.FontTable()
        t.add_bdf(BDF8.encode('latin-1'))
        self.assertEqual((t.cell_w, t.cell_h), (8, 16))
        self.assertEqual(t.glyphs, {0x41: A_ROWS, 0xB1: BAR_ROWS})
        data = t.build()
        self.assertEqual(data, b'XFNT' + bytes((1, 8, 16, 0)) + struct.pack('<HH', 2, 0)
                         + struct.pack('<HH', 0x41, 0xB1) + bytes(A_ROWS) + bytes(BAR_ROWS))
        self.assertEqual(len(data), 12 + 2 * (2 + 16))
        self.assertEqual(xfont.read_xfnt(data), (8, 16, False, {0x41: A_ROWS, 0xB1: BAR_ROWS}))

    def test_bold_left_for_glyphs_clear_of_column_0(self):
        t = xfont.FontTable()
        t.add_bdf(BDF8.encode('latin-1'))
        self.assertEqual(t.first_column_users(), 0)
        self.assertEqual(t.embolden('auto'), 'left')
        self.assertEqual(t.glyphs[0x41][11:14], [0xFC, 0xCC, 0xFC])
        self.assertEqual(t.glyphs[0xB1][0], 0xC0)
        data = t.build()
        self.assertEqual(data[7], 1)                        # the flag: emboldened
        self.assertTrue(xfont.read_xfnt(data)[2])

    def test_16_wide_and_bold_right(self):
        t = xfont.FontTable()
        t.add_bdf(BDF16.encode('latin-1'))
        self.assertEqual(t.cell_w, 16)
        self.assertEqual(t.glyphs[0x2121][0], 0x8005)
        plain = t.build()
        self.assertEqual(plain[12:14], b'\x21\x21')
        self.assertEqual(plain[14:16], b'\x80\x05')         # the left byte of the row first
        self.assertEqual(len(plain), 12 + 2 + 32)
        self.assertEqual(t.embolden('auto'), 'right')       # the glyph uses column 0
        self.assertEqual(t.glyphs[0x2121][0], 0xC005)       # the 1-0-1 gap stays clear
        self.assertEqual(xfont.bold16(0x0005), 0x000D)
        self.assertEqual(xfont.bold16(0x8000), 0x8000)      # a left bold loses column 0's copy
        self.assertEqual(xfont.bold8(0x81), 0x83)
        self.assertEqual(xfont.bold8(0x81, right=True), 0xC1)

    def test_extra_glyphs_and_the_callers_own(self):
        t = xfont.FontTable()
        t.add_bdf(BDF8.encode('latin-1'))
        t.add_art(ART.encode('utf-8'))
        self.assertEqual(sorted(t.glyphs), [0x41, 0xB1, 0x104])
        self.assertEqual(t.glyphs[0x104], [0, 0x18] + [0] * 13 + [0x01])
        self.assertEqual(t.glyphs[0x41], [0x80] * 16)       # later inputs replace earlier codes
        t.embolden('left')
        t.add_glyph(0x7621, [0xAA] * 16)                    # a user-defined glyph, stored as given
        _w, _h, bold, table = xfont.read_xfnt(t.build())
        self.assertTrue(bold)
        self.assertEqual(table[0x7621], [0xAA] * 16)
        self.assertEqual(table[0x104][1], 0x38)
        for code, rows in ((0x10000, [0] * 16), (1, [0] * 15), (1, [0x100] * 16)):
            with self.assertRaises(xfont.FontError):
                t.add_glyph(code, rows)

    def test_pcf(self):
        glyphs = {0x41: A_ROWS, 0x43: BAR_ROWS}
        for big in (False, True):
            with self.subTest(big_endian=big):
                t = xfont.FontTable()
                t.add_pcf(make_pcf(glyphs, big_endian=big))
                self.assertEqual(t.cell_w, 8)
                self.assertEqual(t.glyphs, glyphs)

    def test_files_gzip_and_command_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            bdf = os.path.join(tmp, 'font.bdf.gz')
            pcf = os.path.join(tmp, 'more.pcf')
            art = os.path.join(tmp, 'extra.txt')
            out = os.path.join(tmp, 'out', 'font.xfnt')
            with gzip.open(bdf, 'wb') as f:
                f.write(BDF8.encode('latin-1'))
            with open(pcf, 'wb') as f:
                f.write(make_pcf({0x43: BAR_ROWS}))
            with open(art, 'w', encoding='utf-8') as f:
                f.write(ART)
            text = io.StringIO()
            with contextlib.redirect_stdout(text):
                self.assertEqual(xfont.main(['--no-bold', '-o', out, bdf, pcf, art]), 0)
            self.assertIn('4 glyphs, cell 8x16, 84 bytes', text.getvalue())
            with open(out, 'rb') as f:
                data = f.read()
            self.assertEqual(data, xfont.convert([bdf, pcf, art], no_bold=True))
            w, h, bold, table = xfont.read_xfnt(data)
            self.assertEqual((w, h, bold, sorted(table)), (8, 16, False, [0x41, 0x43, 0xB1, 0x104]))
            with contextlib.redirect_stdout(text):
                self.assertEqual(xfont.main(['-o', out, bdf]), 0)
                self.assertEqual(xfont.main(['--info', out]), 0)
            self.assertIn('emboldened to the left', text.getvalue())
            self.assertIn('2 glyphs, cell 8x16, emboldened, codes 0x0041..0x00b1', text.getvalue())
            extra = xfont.convert([bdf], extra={0x7621: [1] * 16})
            self.assertEqual(xfont.read_xfnt(extra)[3][0x7621], [1] * 16)
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(xfont.main(['-o', out, os.path.join(tmp, 'absent.bdf')]), 1)
                self.assertEqual(xfont.main(['--cell', '16', '-o', out, bdf, art]), 1)
            self.assertIn('xfont:', err.getvalue())

    def test_errors(self):
        with self.assertRaises(xfont.FontError):
            xfont.parse_bdf(b'not a font')
        with self.assertRaises(xfont.FontError):
            xfont.parse_pcf(b'\x01fcp' + bytes(3))
        with self.assertRaises(xfont.FontError):
            xfont.parse_art(b'U+0041\n..#\n....\n')
        with self.assertRaises(xfont.FontError):
            xfont.parse_art(b'..#.\n')
        with self.assertRaises(xfont.FontError):
            xfont.read_xfnt(b'XFNT' + bytes((1, 8, 16, 0)) + struct.pack('<HH', 5, 0))
        with self.assertRaises(xfont.FontError):
            xfont.FontTable().build()
        with self.assertRaises(xfont.FontError):
            xfont.FontTable(12)
        t = xfont.FontTable(16)
        with self.assertRaisesRegex(xfont.FontError, '8 pixels wide'):
            t.add_art(ART.encode('utf-8'))

    def test_raw_glyph_arrays(self):
        rows8 = [bytes(range(r, r + 16)) for r in (0x10, 0x40)]
        data = b'\xEE' * 3 + rows8[0] + b'\x00\x00' + rows8[1]      # offset 3, stride 18
        t = xfont.FontTable(8)
        t.add_raw(data, [0x41, 0x42], offset=3, stride=18)
        self.assertEqual(t.glyphs[0x41], list(range(0x10, 0x20)))
        self.assertEqual(t.glyphs[0x42], list(range(0x40, 0x50)))
        t16 = xfont.FontTable(16)
        t16.add_raw(bytes(range(32)), [0x2121])                       # rows are big-endian
        self.assertEqual(t16.glyphs[0x2121][0], 0x0001)
        self.assertEqual(t16.glyphs[0x2121][15], 0x1E1F)
        with self.assertRaises(xfont.FontError):
            t.add_raw(data, [1, 2, 3], offset=3, stride=18)          # runs past the end
        with self.assertRaises(xfont.FontError):
            t.add_raw(data, [1], stride=8)                           # less than a cell
        with self.assertRaises(xfont.FontError):
            xfont.FontTable().add_raw(data, [1])                     # no cell width yet
        self.assertEqual(xfont.parse_raw_spec('a@b.bin@10:41-43'), ('a@b.bin', 0x10, [0x41, 0x42, 0x43]))
        self.assertEqual(xfont.parse_raw_spec('f@0:7F'), ('f', 0, [0x7F]))
        for bad in ('f:41', 'f@x:41', 'f@0:43-41', '@0:41'):
            with self.assertRaises(xfont.FontError):
                xfont.parse_raw_spec(bad)
        with tempfile.TemporaryDirectory() as tmp:
            raw = os.path.join(tmp, 'GAME.EXE')
            out = os.path.join(tmp, 'ank.xfnt')
            with open(raw, 'wb') as f:
                f.write(bytes(5) + bytes(range(32)))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(xfont.main(['--no-bold', '--cell', '8', '--raw',
                                             raw + '@5:30-31', '-o', out]), 0)
            with open(out, 'rb') as f:
                table = xfont.read_xfnt(f.read())[3]
            self.assertEqual(table, {0x30: list(range(16)), 0x31: list(range(16, 32))})

    def test_codes_beyond_16_bits_are_left_out(self):
        t = xfont.FontTable()
        t.add_bdf(BDF8.replace('ENCODING 177', 'ENCODING 131072').encode('latin-1'))
        self.assertEqual((sorted(t.glyphs), t.skipped), ([0x41], 1))


if __name__ == '__main__':
    unittest.main()
