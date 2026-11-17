# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of lrvne.reflow: the cases of tests/test_text.c (test_reflow, test_reflow_from), so
the Python reference and src/text/reflow.c are held to the same answers."""
import unittest

from lrvne import reflow


def cols(cp):
    """ASCII one column, everything else two, controls none (test_text.c's cols_fn)"""
    return 0 if cp < 0x20 else 1 if cp < 0x80 else 2


def lay(text, width, height, last, *at, **kw):
    b = text.encode('utf-8') if isinstance(text, str) else text
    return reflow.layout_from(b, cols, width, height, last, *at, **kw)


def spans(lines):
    return [(ln.start, ln.end, ln.cols, ln.page) for ln in lines]


def places(lines):
    return [(ln.row, ln.col) for ln in lines]


class Layout(unittest.TestCase):
    def test_words_and_lines(self):
        self.assertEqual(lay('', 10, 3, 10), [])
        self.assertEqual(spans(lay('aaa bbb ccc', 7, 3, 7)), [(0, 7, 7, 0), (8, 11, 3, 0)])
        self.assertEqual(spans(lay('aaa bbb ccc', 6, 3, 6)),
                         [(0, 3, 3, 0), (4, 7, 3, 0), (8, 11, 3, 0)])
        self.assertEqual(spans(lay('  a  b  ', 10, 3, 10)), [(2, 6, 4, 0)])

    def test_pages_and_breaks(self):
        self.assertEqual(spans(lay('aaa bbb cccccc dd', 7, 2, 5)),
                         [(0, 7, 7, 0), (8, 13, 5, 0), (13, 17, 4, 1)])
        self.assertEqual(spans(lay('abcdefghij', 4, 5, 4)),
                         [(0, 4, 4, 0), (4, 8, 4, 0), (8, 10, 2, 0)])
        self.assertEqual(spans(lay('a\n\nb', 10, 5, 10)),
                         [(0, 1, 1, 0), (2, 2, 0, 0), (3, 4, 1, 0)])
        self.assertEqual(spans(lay('a\fb\f', 10, 5, 10)), [(0, 1, 1, 0), (2, 3, 1, 1)])
        self.assertEqual(spans(lay('\fa', 10, 5, 10)), [(1, 2, 1, 0)])
        got = lay('a\nb\nc', 10, 2, 10)
        self.assertEqual(spans(got)[2], (4, 5, 1, 1))
        self.assertEqual(places(got), [(0, 0), (1, 0), (0, 0)])

    def test_wide(self):
        self.assertEqual(spans(lay('ąę ą', 5, 3, 5)), [(0, 4, 4, 0), (5, 7, 2, 0)])
        self.assertEqual(spans(lay('ąęą', 5, 3, 5)), [(0, 4, 4, 0), (4, 6, 2, 0)])
        self.assertEqual(spans(lay('ą', 1, 3, 1)), [(0, 2, 2, 0)])

    def test_max_lines(self):
        got = lay('x\n' * 300, 10, 4, 10)
        self.assertEqual(len(got), reflow.MAX_LINES)
        self.assertEqual(got[-1].page + 1, reflow.MAX_LINES // 4)

    def test_fit(self):
        def fit(t, w):
            b, c = reflow.fit(t.encode('utf-8'), cols, w)
            return len(b), c
        self.assertEqual(fit('abcdef', 4), (4, 4))
        self.assertEqual(fit('abc', 4), (3, 3))
        self.assertEqual(fit('ab\ncd', 10), (5, 5))
        self.assertEqual(fit('abcą', 4), (3, 3))
        self.assertEqual(fit('', 4), (0, 0))


class LayoutFrom(unittest.TestCase):
    def test_first_line(self):
        got = lay('aaa bbb ccc ddd', 5, 4, 3, 2, 0, 0)
        self.assertEqual(spans(got), [(0, 3, 3, 0), (4, 7, 3, 0), (8, 11, 3, 1),
                                      (12, 15, 3, 1)])
        self.assertEqual(places(got), [(2, 0), (3, 0), (0, 0), (1, 0)])

    def test_prefix_and_indent(self):
        got = lay('aa bb cc dd ee', 10, 3, 10, 0, 4, 2)
        self.assertEqual(spans(got), [(0, 5, 5, 0), (6, 14, 8, 0)])
        self.assertEqual(places(got), [(0, 4), (1, 2)])
        got = lay('aa bb cc dd', 7, 1, 7, 0, 3, 2)
        self.assertEqual(places(got), [(0, 3), (0, 2), (0, 2)])
        self.assertEqual(got[2].page, 2)

    def test_first_word_below(self):
        got = lay('abcdef', 10, 3, 10, 0, 8, 2)
        self.assertEqual(spans(got), [(0, 0, 0, 0), (0, 6, 6, 0)])
        self.assertEqual(places(got), [(0, 8), (1, 2)])
        self.assertEqual(spans(lay('abcdefghijk', 10, 3, 10, 0, 2, 2)),
                         [(0, 8, 8, 0), (8, 11, 3, 0)])

    def test_keep_indent(self):
        self.assertEqual(spans(lay('  ab\n   cd ef', 6, 5, 6, keep_indent=True)),
                         [(0, 4, 4, 0), (5, 10, 5, 0), (11, 13, 2, 0)])
        self.assertEqual(spans(lay('    abcdef', 6, 5, 6, keep_indent=True)), [(4, 10, 6, 0)])
        self.assertEqual(spans(lay('  ab', 6, 5, 6)), [(2, 4, 2, 0)])

    def test_full_page_before(self):
        got = lay('a', 10, 4, 10, 4, 0, 0)
        self.assertEqual(spans(got), [(0, 1, 1, 1)])
        self.assertEqual(places(got), [(0, 0)])
        got = lay('\fa', 10, 4, 10, 2, 0, 0)
        self.assertEqual(spans(got), [(1, 2, 1, 1)])
        self.assertEqual(places(got), [(0, 0)])
        self.assertEqual(lay('', 10, 4, 10, 2, 3, 0), [])

    def test_columns(self):
        c = reflow.columns({ord('a')})
        self.assertEqual((c(0x0A), c(ord('a')), c(ord('b'))), (0, 1, 2))


if __name__ == '__main__':
    unittest.main()
