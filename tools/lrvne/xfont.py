# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert bitmap fonts (BDF or PCF, optionally gzipped) to XFNT glyph tables.

XFNT format, version 1 (little-endian; a pack entry of type FONT):
  0x00  4   "XFNT"
  0x04  u8  version (1)
  0x05  u8  cell width in pixels: 8 or 16
  0x06  u8  cell height in pixels (16 in every table the engines use)
  0x07  u8  flags: bit 0 = the glyphs were emboldened when the table was built
  0x08  u16 glyph count N
  0x0A  u16 reserved (0)
  0x0C  N x u16 code, ascending and unique (a reader finds a glyph by binary search)
  0x0C + 2*N
        N glyphs in the order of the codes, each cell_h * cell_w/8 bytes: the rows top to
        bottom, each row cell_w/8 bytes, the most significant bit of the first byte the
        leftmost pixel; a set bit is ink.
The file ends after the last glyph: its size is 12 + N * (2 + cell_h * cell_w/8), and a reader
must check that against the entry's size before trusting N.

A table does not say what its codes are; that is a convention between the build and the
engine. The ones in use: 16-wide tables of a JIS font are keyed by the JIS X 0208 code
(0x2121..), 8-wide ones by the JIS X 0201 byte; tables for translated text (UTF-8) are keyed
by Unicode code point (BMP only: codes are 16 bits).

Input fonts are BDF or PCF files, or a text file of extra glyphs (.txt), used for letters a
font lacks:
    # comment (outside a glyph block)
    U+0104 anything, e.g. the character itself
    ........        cell_h rows of '.' and '#', as wide as the cell, already placed in the
    ...#....        cell and not emboldened; a blank line ends the block
All inputs must have the same cell size; later inputs replace earlier codes.

Emboldening: without --no-bold every pixel is doubled sideways at build time (8-wide: r | r<<1;
16-wide the same, keeping the clear pixel of every 1-0-1 pattern clear). That doubles to the
left and suits glyphs that leave column 0 free, as the character ROMs of the machines do.
Fonts drawn from column 0 are doubled to the right instead (--bold right; auto picks it when
any glyph uses column 0), which equals shifting them one column right first. An engine that
emboldens at run time takes its tables with --no-bold.

As a library, `FontTable` is the converter; `add_glyph` adds user-defined glyphs (a symbol an
engine draws itself, say) from the caller:

    t = xfont.FontTable()
    t.add_file('font.bdf')
    t.embolden('auto')
    t.add_glyph(0x7621, rows)           # 16 ints, bit 15 = leftmost pixel; stored as given
    data = t.build()

Command line (python3 -I tools/lrvne xfont ...):
  xfont [--no-bold] [--bold auto|left|right] [--cell 8|16] -o OUT.xfnt FONT [FONT...]
  xfont --info TABLE.xfnt
"""
import argparse
import gzip
import struct
import sys
from typing import Dict, List, Optional, Sequence, Tuple

from ._util import write_file

MAGIC = b'XFNT'
VERSION = 1
HEADER_SIZE = 12
FLAG_BOLD = 1
Glyphs = Dict[int, List[int]]                           # code -> rows, bit cell_w-1 leftmost
Outlines = Dict[int, Tuple[List[int], List[int]]]       # code -> (BBX w h xoff yoff, rows)


class FontError(Exception):
    """A font cannot be read or a glyph table cannot be built."""


# ---- XFNT tables -------------------------------------------------------------------------------

def write_xfnt(cell_w: int, cell_h: int, bold: bool, table: Glyphs) -> bytes:
    """The XFNT bytes of a table {code: rows}."""
    if cell_w not in (8, 16) or not 1 <= cell_h <= 255:
        raise FontError('cell %dx%d: the width must be 8 or 16' % (cell_w, cell_h))
    codes = sorted(table)
    if len(codes) > 0xFFFF:
        raise FontError('%d glyphs: a table holds at most 65535' % len(codes))
    out = bytearray(MAGIC)
    out += struct.pack('<BBBBHH', VERSION, cell_w, cell_h, FLAG_BOLD if bold else 0,
                       len(codes), 0)
    for c in codes:
        if not 0 <= c <= 0xFFFF:
            raise FontError('code %#x does not fit 16 bits' % c)
        out += struct.pack('<H', c)
    for c in codes:
        rows = table[c]
        if len(rows) != cell_h or any(not 0 <= r < 1 << cell_w for r in rows):
            raise FontError('glyph %#x is not %dx%d' % (c, cell_w, cell_h))
        for r in rows:
            out += r.to_bytes(cell_w // 8, 'big')
    return bytes(out)


def read_xfnt(data: bytes) -> Tuple[int, int, bool, Glyphs]:
    """(cell width, cell height, emboldened, {code: rows}) of an XFNT table."""
    if len(data) < HEADER_SIZE or data[:4] != MAGIC:
        raise FontError('not an XFNT table')
    version, cell_w, cell_h, flags, count, _ = struct.unpack_from('<BBBBHH', data, 4)
    wb = cell_w // 8
    if version != VERSION or cell_w not in (8, 16) or \
            HEADER_SIZE + count * (2 + cell_h * wb) > len(data):
        raise FontError('damaged or unknown XFNT table (version %d, cell %dx%d, %d glyphs)'
                        % (version, cell_w, cell_h, count))
    codes = struct.unpack_from('<%dH' % count, data, HEADER_SIZE)
    base = HEADER_SIZE + 2 * count
    table = {}
    for i, c in enumerate(codes):
        g = base + i * cell_h * wb
        table[c] = [int.from_bytes(data[g + r * wb:g + (r + 1) * wb], 'big')
                    for r in range(cell_h)]
    return cell_w, cell_h, bool(flags & FLAG_BOLD), table


# ---- extra glyphs as text ----------------------------------------------------------------------

def parse_art(data: bytes) -> Tuple[Optional[int], Glyphs]:
    """(cell width, {code: rows}) of an extra-glyph text file; the width is that of the rows."""
    glyphs: Glyphs = {}
    code: Optional[int] = None
    width: Optional[int] = None
    for no, ln in enumerate(data.decode('utf-8').splitlines(), 1):
        if not ln.strip():
            code = None                         # a blank line ends a glyph block
            continue
        if code is None and ln.startswith('#'):
            continue                            # comment (only outside glyph blocks)
        if ln.startswith('U+'):
            try:
                code = int(ln.split()[0][2:], 16)
            except ValueError:
                raise FontError('line %d: bad code in %r' % (no, ln)) from None
            glyphs[code] = []
            continue
        if code is None or set(ln) - set('.#') or (width is not None and len(ln) != width):
            raise FontError('line %d: bad glyph row %r' % (no, ln))
        width = len(ln)
        glyphs[code].append(int(ln.replace('.', '0').replace('#', '1'), 2))
    return width, glyphs


# ---- BDF ---------------------------------------------------------------------------------------

def parse_bdf(data: bytes) -> Tuple[int, int, int, Outlines]:
    """(bounding box width, height, ascent, glyphs) of a BDF font. The ascent is FONT_ASCENT,
    14 when the font does not state it."""
    glyphs: Outlines = {}
    lines = data.decode('latin-1').splitlines()
    w = h = None
    ascent = None
    i = 0
    try:
        while i < len(lines):
            ln = lines[i]
            if ln.startswith('FONTBOUNDINGBOX'):
                w, h = int(ln.split()[1]), int(ln.split()[2])
            elif ln.startswith('FONT_ASCENT') and ascent is None:
                ascent = int(ln.split()[1])
            elif ln.startswith('STARTCHAR'):
                enc = None
                bbx = None
                rows: List[int] = []
                i += 1
                while not lines[i].startswith('ENDCHAR'):
                    t = lines[i]
                    if t.startswith('ENCODING'):
                        enc = int(t.split()[1])
                    elif t.startswith('BBX'):
                        bbx = [int(v) for v in t.split()[1:5]]
                    elif t.startswith('BITMAP'):
                        i += 1
                        while not lines[i].startswith('ENDCHAR'):
                            rows.append(int(lines[i].strip(), 16))
                            i += 1
                        continue
                    i += 1
                if enc is not None and enc >= 0 and bbx:
                    glyphs[enc] = (bbx, rows)
            i += 1
    except (ValueError, IndexError):
        raise FontError('damaged BDF font near line %d' % (i + 1)) from None
    if w is None or h is None:
        raise FontError('not a BDF font (no FONTBOUNDINGBOX)')
    return w, h, 14 if ascent is None else ascent, glyphs


# ---- PCF ---------------------------------------------------------------------------------------

_PCF_METRICS, _PCF_BITMAPS, _PCF_ENCODINGS = 4, 8, 0x20
_PCF_COMPRESSED_METRICS = 0x100


def parse_pcf(data: bytes) -> Outlines:
    """The glyphs of a PCF font, as (BBX, rows) like those of a BDF font."""
    if data[:4] != b'\x01fcp':
        raise FontError('not a PCF font')
    try:
        return _parse_pcf(data)
    except (struct.error, KeyError, IndexError):
        raise FontError('damaged PCF font') from None


def _parse_pcf(data: bytes) -> Outlines:
    ntab, = struct.unpack_from('<I', data, 4)
    tables = {}
    for k in range(ntab):
        typ, fmt, size, off = struct.unpack_from('<IIII', data, 8 + 16 * k)
        tables[typ] = (fmt, size, off)

    def order(fmt: int) -> str:
        return '>' if fmt & 4 else '<'

    fmt, _size, off = tables[_PCF_METRICS]
    o = order(fmt)
    p = off + 4
    metrics = []                    # left bearing, right bearing, width, ascent, descent
    if fmt & _PCF_COMPRESSED_METRICS:
        n, = struct.unpack_from(o + 'H', data, p)
        p += 2
        for _ in range(n):
            metrics.append([v - 0x80 for v in data[p:p + 5]])
            p += 5
    else:
        n, = struct.unpack_from(o + 'I', data, p)
        p += 4
        for _ in range(n):
            metrics.append(list(struct.unpack_from(o + '5h', data, p)))
            p += 12
    fmt, _size, off = tables[_PCF_BITMAPS]
    o = order(fmt)
    count, = struct.unpack_from(o + 'I', data, off + 4)
    offsets = struct.unpack_from(o + '%dI' % count, data, off + 8)
    sizes = struct.unpack_from(o + '4I', data, off + 8 + 4 * count)
    bitmaps_off = off + 8 + 4 * count + 16
    pad = 1 << (fmt & 3)
    msb_first = bool(fmt & 8)
    unit = 1 << ((fmt >> 4) & 3)
    bitmaps = data[bitmaps_off:bitmaps_off + sizes[fmt & 3]]
    fmt, _size, off = tables[_PCF_ENCODINGS]
    o = order(fmt)
    min2, max2, min1, max1, _default = struct.unpack_from(o + 'HHHHh', data, off + 4)
    p = off + 14
    glyphs: Outlines = {}
    for b1 in range(min1, max1 + 1):
        for b2 in range(min2, max2 + 1):
            idx, = struct.unpack_from(o + 'H', data, p)
            p += 2
            if idx == 0xFFFF:
                continue
            lsb, _rsb, width, ascent, descent = metrics[idx]
            h = ascent + descent
            rowbytes = ((width + 7) // 8 + pad - 1) // pad * pad
            start = offsets[idx]
            rows = []
            for r in range(h):
                rb = bytearray(bitmaps[start + r * rowbytes:start + (r + 1) * rowbytes])
                # bytes within a unit are stored in the table's order: make them big-endian
                if unit > 1 and o == '<':
                    for u in range(0, len(rb), unit):
                        rb[u:u + unit] = rb[u:u + unit][::-1]
                if not msb_first:
                    rb = bytearray(int('{:08b}'.format(b)[::-1], 2) for b in rb)
                rows.append(int.from_bytes(rb[:(width + 7) // 8], 'big'))
            glyphs[(b1 << 8) | b2] = ([width, h, lsb, -descent], rows)
    return glyphs


# ---- glyph normalisation -----------------------------------------------------------------------

def to_cell(bbx: Sequence[int], rows: Sequence[int], cell_w: int, cell_h: int,
            ascent: int) -> List[int]:
    """Places a glyph (BBX w h xoff yoff, its rows padded to whole bytes on the right) into a
    cell whose baseline is `ascent` rows below the top."""
    w, h, xoff, yoff = bbx
    out = [0] * cell_h
    top = ascent - (yoff + h)               # the glyph's first row in the cell
    padded = ((w + 7) // 8) * 8
    for r, v in enumerate(rows):
        y = top + r
        if 0 <= y < cell_h:
            row = (v >> (padded - w)) & ((1 << w) - 1)
            shift = cell_w - w - xoff
            row = row << shift if shift >= 0 else row >> -shift
            out[y] |= row & ((1 << cell_w) - 1)
    return out


def bold16(r: int, right: bool = False) -> int:
    """A 16-wide row emboldened; the clear pixel of a 1-0-1 pattern stays clear."""
    gap = (r >> 1) & ~r & (r << 1)
    if right:
        return ((r | (r >> 1)) & ~gap) & 0xFFFF
    return ((r | (r << 1)) & ~gap) & 0xFFFF


def bold8(r: int, right: bool = False) -> int:
    """An 8-wide row emboldened."""
    if right:
        return (r | (r >> 1)) & 0xFF
    return (r | (r << 1)) & 0xFF


def read_font_file(path: str) -> bytes:
    """The bytes of a font file, unzipped when it is gzipped."""
    try:
        with open(path, 'rb') as f:
            data = f.read()
        return gzip.decompress(data) if data[:2] == b'\x1f\x8b' else data
    except (OSError, EOFError) as e:
        raise FontError('%s: %s' % (path, getattr(e, 'strerror', None) or e)) from e


class FontTable:
    """A glyph table being built: fonts and glyphs go in, XFNT bytes come out.

    `cell_w` is 8 or 16, None to take it from the first input (16 for a font wider than 8
    pixels). Attributes: `glyphs` {code: rows}, `bold` (set by `embolden`), `skipped` (codes
    left out because they do not fit 16 bits).
    """

    def __init__(self, cell_w: Optional[int] = None, cell_h: int = 16) -> None:
        if cell_w not in (None, 8, 16):
            raise FontError('the cell width must be 8 or 16')
        self.cell_w = cell_w
        self.cell_h = cell_h
        self.glyphs: Glyphs = {}
        self.bold = False
        self.skipped = 0

    def _put(self, code: int, rows: List[int]) -> None:
        if not 0 <= code <= 0xFFFF:
            self.skipped += 1
            return
        self.glyphs[code] = rows

    def add_outlines(self, glyphs: Outlines, width: int, ascent: int) -> None:
        """Adds parsed BDF/PCF glyphs of a font `width` pixels wide."""
        if self.cell_w is None:
            self.cell_w = 16 if width > 8 else 8
        for code, (bbx, rows) in glyphs.items():
            self._put(code, to_cell(bbx, rows, self.cell_w, self.cell_h, ascent))

    def add_bdf(self, data: bytes) -> None:
        """Adds a BDF font."""
        w, _h, ascent, glyphs = parse_bdf(data)
        self.add_outlines(glyphs, w, ascent)

    def add_pcf(self, data: bytes) -> None:
        """Adds a PCF font."""
        glyphs = parse_pcf(data)
        if not glyphs:
            raise FontError('the PCF font has no glyphs')
        width = max(g[0][0] for g in glyphs.values())
        ascent = max(g[0][1] + g[0][3] for g in glyphs.values())    # height + y offset
        self.add_outlines(glyphs, width, ascent)

    def add_art(self, data: bytes) -> None:
        """Adds the glyphs of an extra-glyph text file."""
        width, glyphs = parse_art(data)
        if not glyphs:
            return
        if self.cell_w is None:
            self.cell_w = width
        if width != self.cell_w:
            raise FontError('the glyphs are %s pixels wide, the cell is %d' % (width, self.cell_w))
        for code, rows in glyphs.items():
            self.add_glyph(code, rows)

    def add_glyph(self, code: int, rows: Sequence[int]) -> None:
        """Adds one glyph as its rows (ints, bit cell_w-1 the leftmost pixel), replacing the
        code's glyph. It is stored as given: emboldened only if `embolden` is called after."""
        if self.cell_w is None:
            raise FontError('the cell width is not known yet: add a font first, or state it')
        if len(rows) != self.cell_h or any(not 0 <= r < 1 << self.cell_w for r in rows):
            raise FontError('glyph %#x is not %dx%d' % (code, self.cell_w, self.cell_h))
        if not 0 <= code <= 0xFFFF:
            raise FontError('code %#x does not fit 16 bits' % code)
        self.glyphs[code] = list(rows)

    def add_file(self, path: str) -> None:
        """Adds a font file: extra glyphs (.txt), PCF or BDF, gzipped or not."""
        data = read_font_file(path)
        try:
            if path.lower().endswith('.txt'):
                self.add_art(data)
            elif data[:4] == b'\x01fcp':
                self.add_pcf(data)
            else:
                self.add_bdf(data)
        except FontError as e:
            raise FontError('%s: %s' % (path, e)) from None

    def first_column_users(self) -> int:
        """How many glyphs have ink in column 0."""
        first = 1 << ((self.cell_w or 8) - 1)
        return sum(1 for rows in self.glyphs.values() if any(r & first for r in rows))

    def embolden(self, direction: str = 'auto') -> str:
        """Emboldens every glyph in the table; returns the direction used ('left' or 'right').
        'auto' is 'right' when any glyph uses column 0."""
        if direction not in ('auto', 'left', 'right'):
            raise FontError('bold direction %r' % direction)
        if direction == 'auto':
            direction = 'right' if self.first_column_users() else 'left'
        right = direction == 'right'
        fn = bold16 if self.cell_w == 16 else bold8
        for code, rows in self.glyphs.items():
            self.glyphs[code] = [fn(r, right) for r in rows]
        self.bold = True
        return direction

    def build(self) -> bytes:
        """The XFNT table."""
        if self.cell_w is None or not self.glyphs:
            raise FontError('no glyphs')
        return write_xfnt(self.cell_w, self.cell_h, self.bold, self.glyphs)


def convert(paths: Sequence[str], no_bold: bool = False, bold: str = 'auto',
            cell_w: Optional[int] = None, cell_h: int = 16,
            extra: Optional[Glyphs] = None) -> bytes:
    """Font files to an XFNT table in one call. `extra` are user-defined glyphs added last
    (after the emboldening: they are stored as given)."""
    table = FontTable(cell_w, cell_h)
    for path in paths:
        table.add_file(path)
    if not no_bold:
        table.embolden(bold)
    for code, rows in (extra or {}).items():
        table.add_glyph(code, rows)
    return table.build()


# ---- command line ------------------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    """The command line (see the module's description)."""
    ap = argparse.ArgumentParser(prog='lrvne xfont', description=__doc__.split('\n')[0])
    ap.add_argument('fonts', nargs='*', help='BDF or PCF fonts (gzipped or not), .txt extra glyphs')
    ap.add_argument('-o', '--output')
    ap.add_argument('--no-bold', action='store_true', help='leave the glyphs as they are')
    ap.add_argument('--bold', choices=('auto', 'left', 'right'), default='auto',
                    help='direction of the emboldening: left suits glyphs that leave column 0 '
                         'free, right those drawn from column 0; auto picks right when any '
                         'glyph uses column 0 (default)')
    ap.add_argument('--cell', type=int, choices=(8, 16), help='force the cell width')
    ap.add_argument('--height', type=int, default=16, help='cell height (default 16)')
    ap.add_argument('--info', metavar='TABLE', help='describe an XFNT table instead')
    a = ap.parse_args(argv)
    try:
        if a.info:
            with open(a.info, 'rb') as f:
                cell_w, cell_h, bold, glyphs = read_xfnt(f.read())
            codes = sorted(glyphs)
            print('%s: %d glyphs, cell %dx%d%s, codes %#06x..%#06x'
                  % (a.info, len(codes), cell_w, cell_h, ', emboldened' if bold else '',
                     codes[0] if codes else 0, codes[-1] if codes else 0))
            return 0
        if not a.fonts or not a.output:
            ap.error('fonts and -o OUT are required')
        table = FontTable(a.cell, a.height)
        for path in a.fonts:
            table.add_file(path)
        if not a.no_bold:
            used = table.first_column_users()
            direction = table.embolden(a.bold)
            print('bold: %d of %d glyphs use column 0 -> emboldened to the %s'
                  % (used, len(table.glyphs), direction))
        out = table.build()
        write_file(a.output, out)
        if table.skipped:
            print('%d glyphs with codes above 0xFFFF left out' % table.skipped)
        print('%s: %d glyphs, cell %dx%d, %d bytes' % (a.output, len(table.glyphs), table.cell_w,
                                                       table.cell_h, len(out)))
        return 0
    except (FontError, OSError) as e:
        print('xfont: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
