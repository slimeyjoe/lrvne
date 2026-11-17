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
 * starts the next one; a word wider than a whole line is broken where the line is full.
 *
 * tools/lrvne/reflow.py is the same layout in Python (the reference for an engine's tests). */
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
    int row;                        /* the line of its page, 0-based */
    int col;                        /* the column it starts at */
};

/* Where a text starts, for lrv_reflow_layout_from. */
#define LRV_REFLOW_KEEP_INDENT 1u   /* spaces at the start of the text and after U+000A are
                                     * kept and drawn (an indented line stays indented) */
struct lrv_reflow_at {
    int first_line;                 /* the text's first line is this line of its page: the
                                     * lines above are the caller's (height or more: the text
                                     * starts on the next page, its lines' page 1) */
    int first_col;                  /* ... and starts at this column */
    int indent;                     /* the column every later line starts at */
    unsigned flags;                 /* LRV_REFLOW_KEEP_INDENT */
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

/* The same for a text that continues what the caller put on the page (a translated passage in
 * the middle of a page, a speaker's name before it): it starts on line at->first_line of the
 * page, at column at->first_col; every later line starts at column at->indent (a hanging
 * indent, also on the pages after the first). `width` and `last` are the columns of a whole
 * line, so a line holds width (last) minus its start column. When the first word does not fit
 * after first_col but would fit on a later line, the first line stays empty (a name with
 * nothing after it) and the word starts the next. at = NULL is lrv_reflow_layout. */
void lrv_reflow_layout_from(const uint8_t *text, int width, int height, int last,
                            const struct lrv_reflow_at *at, lrv_cols_fn *cols, void *ctx,
                            struct lrv_reflow *out);

/* A single-line item (a menu entry): the number of bytes of its longest prefix of at most
 * `width` columns, U+000A and U+000C counting as one-column spaces; *fit_cols receives the
 * prefix's width. */
uint32_t lrv_reflow_fit(const uint8_t *text, int width, lrv_cols_fn *cols, void *ctx,
                        int *fit_cols);

/* One character at s (s[0] != 0): its length in bytes (1..4), code point and columns. */
int lrv_reflow_char(const uint8_t *s, lrv_cols_fn *cols, void *ctx, uint32_t *cp, int *ccols);

#endif
