# SPDX-License-Identifier: GPL-3.0-or-later
"""Reflow of translated text into a text window: the layout of src/text/reflow.c
(lrvne/reflow.h) in Python, the reference an engine's tests compare its layout with.

A text is UTF-8. How many columns a character takes is the caller's (`cols(cp)`; columns()
makes the usual one: 0 below U+0020, 1 for the characters a narrow font has, else 2).
  U+0020 space  separates words; the spaces between two words on a line are kept as written,
                spaces at a line break are dropped (and at the start of a line, unless
                keep_indent: then spaces at the start of the text and after U+000A are kept)
  U+000A        ends the line (an empty line when the line is empty)
  U+000C        ends the page
Words are placed greedily: a word goes on the current line when it fits with the spaces
before it, otherwise it starts the next line; a word wider than a line is cut where the line
is full (at least one character a line). A page holds `height` lines; a line is `width`
columns, the last line of a page `last`, both counted from column 0 of the window.

layout_from(): the text continues what the caller put on the page. It starts on line
first_line of the page at column first_col; every later line starts at column indent (a
hanging indent, on later pages too), so it holds width (last) minus that. When the first word
does not fit after first_col but would on a later line, the first line stays empty.

The lines are Line(start, end, cols, page, row, col): the line is text[start:end], `cols`
columns wide, on page `page` (0-based) as its line `row`, starting at column `col`.
"""
from collections import namedtuple

SP, NL, PG = 0x20, 0x0A, 0x0C
MAX_LINES = 256                     # LRV_REFLOW_MAX_LINES: the C layout keeps no more

Line = namedtuple('Line', 'start end cols page row col')


def columns(narrow):
    """the usual cols(cp): 0 for a control, 1 for a code point in `narrow`, else 2"""
    return lambda cp: 0 if cp < 0x20 else 1 if cp in narrow else 2


def chars(b, cols):
    """(start, end, code point, columns) of each character of the UTF-8 bytes b"""
    out, p = [], 0
    for c in b.decode('utf-8'):
        n = len(c.encode('utf-8'))
        out.append((p, p + n, ord(c), cols(ord(c))))
        p += n
    return out


def layout(b, cols, width, height, last=None):
    return layout_from(b, cols, width, height, last)


def layout_from(b, cols, width, height, last=None, first_line=0, first_col=0, indent=0,
                keep_indent=False):
    ch = chars(b, cols)
    last = width if last is None else last
    lines = []
    st = dict(page=0, inpage=min(max(first_line, 0), height), force=False, emitted=0,
              line=None, indent_only=False)      # line = [start, end, cols]

    def line_col():
        return indent if st['emitted'] else first_col

    def emit(start, end, c):
        col = line_col()
        if st['force'] or st['inpage'] >= height:
            if st['inpage']:
                st['page'] += 1
            st['inpage'] = 0
            st['force'] = False
        st['emitted'] += 1
        lines.append(Line(start, end, c, st['page'], st['inpage'], col))
        st['inpage'] += 1

    def flush():
        if st['line'] is not None:
            emit(*st['line'])
            st['line'] = None

    def slot():
        return 0 if st['force'] or st['inpage'] >= height else st['inpage']

    def cap():
        return (last if slot() == height - 1 else width) - line_col()

    def cap_next():
        s = 0 if slot() + 1 == height else slot() + 1
        return (last if s == height - 1 else width) - indent

    def place(i, j, w):
        """the word ch[i:j], w columns, at the start of a line"""
        while True:
            room = cap()
            if not st['emitted'] and w > room and cap_next() > room:
                emit(ch[i][0], ch[i][0], 0)           # the first line stays empty
                continue
            c, k = 0, i
            while k < j and (c + ch[k][3] <= room or k == i):
                c += ch[k][3]
                k += 1
            if k == j:
                st['line'] = [ch[i][0], ch[j - 1][1], c]
                st['indent_only'] = False
                return
            emit(ch[i][0], ch[k - 1][1], c)
            w -= c
            i = k

    i, n, bol = 0, len(ch), True
    while i < n:
        cp = ch[i][2]
        if cp == SP:
            if bol and keep_indent and st['line'] is None:
                j = i
                while j < n and ch[j][2] == SP:
                    j += 1
                w = min(j - i, max(cap(), 0))
                st['line'] = [ch[i][0], ch[i][0] + w, w]
                st['indent_only'] = True
                i, bol = j, False
                continue
            i += 1
        elif cp == NL:
            if st['line'] is None:
                emit(ch[i][0], ch[i][0], 0)
            else:
                flush()
            i, bol = i + 1, True
        elif cp == PG:
            flush()
            if st['inpage']:
                st['force'] = True
            i, bol = i + 1, True
        else:
            bol = False
            j = i
            while j < n and ch[j][2] not in (SP, NL, PG):
                j += 1
            w = sum(c[3] for c in ch[i:j])
            line = st['line']
            if line is not None:
                gap = ch[i][0] - line[1]               # the spaces between, a byte each
                if line[2] + gap + w <= cap():
                    line[1] = ch[j - 1][1]
                    line[2] += gap + w
                    st['indent_only'] = False
                    i = j
                    continue
                if st['indent_only']:
                    st['line'] = None                 # the indent goes with the word
                else:
                    flush()
            place(i, j, w)
            i = j
    flush()
    return lines[:MAX_LINES]


def fit(b, cols, width):
    """a single-line item (lrv_reflow_fit): (the bytes of its longest prefix of at most
    `width` columns, their columns); U+000A and U+000C count as one-column spaces"""
    n = w = 0
    for s, e, cp, c in chars(b, cols):
        if cp in (NL, PG):
            c = 1
        if w + c > width:
            break
        w += c
        n = e
    return b[:n], w


def words(b):
    """the words of a text, for an engine's 'nothing lost' check"""
    return b.replace(bytes([NL]), b' ').replace(bytes([PG]), b' ').split()
