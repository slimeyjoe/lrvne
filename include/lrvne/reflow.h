/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reflow of translated text into a text window: word wrap of UTF-8 text into lines and pages.
 *
 * A translation is longer or shorter than the text it replaces, so it is laid out again for
 * the window of the original: `width` columns by `height` lines a page, the last line of every
 * page `last` columns wide (where an original keeps a cell free for its page-wait mark).
 * Columns are 8 pixels; how many a character takes is asked of a callback, so the layout knows
 * no font.
 *
 * Rules: words are separated by spaces, which are never drawn at the start or the end of a
 * line (runs of spaces inside a line are kept). U+000A ends the line (an empty line when
 * nothing is on it), U+000C ends the page. A word that does not fit on the rest of a line
 * starts the next one; a word wider than a whole line is broken where the line is full. */
#ifndef LRVNE_REFLOW_H
#define LRVNE_REFLOW_H

#include <stdint.h>

#define LRV_REFLOW_MAX_LINES 256

/* Columns of a code point (0 for what is not drawn). */
typedef int lrv_cols_fn(void *ctx, uint32_t cp);

struct lrv_reflow_line {
    uint32_t start, end;            /* bytes text[start..end) */
    int cols;
    int page;                       /* 0-based */
};

struct lrv_reflow {
    int nlines;
    int npages;
    int dropped;                    /* lines past LRV_REFLOW_MAX_LINES, which are not in line[] */
    struct lrv_reflow_line line[LRV_REFLOW_MAX_LINES];
};

/* The lines and pages of a NUL-terminated UTF-8 text. */
void lrv_reflow_layout(const uint8_t *text, int width, int height, int last, lrv_cols_fn *cols,
                       void *ctx, struct lrv_reflow *out);

/* A single-line item (a menu entry): the number of bytes of its longest prefix of at most
 * `width` columns, U+000A and U+000C counting as one-column spaces; *fit_cols receives the
 * prefix's width. */
uint32_t lrv_reflow_fit(const uint8_t *text, int width, lrv_cols_fn *cols, void *ctx,
                        int *fit_cols);

/* One character at s (s[0] != 0): its length in bytes (1..4), code point and columns. */
int lrv_reflow_char(const uint8_t *s, lrv_cols_fn *cols, void *ctx, uint32_t *cp, int *ccols);

#endif
