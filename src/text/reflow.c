/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reflow of translated text (lrvne/reflow.h). */
#include <string.h>
#include "lrvne/reflow.h"
#include "lrvne/utf8.h"

#define SP 0x20u
#define NL 0x0Au
#define PG 0x0Cu

int lrv_reflow_char(const uint8_t *s, lrv_cols_fn *cols, void *ctx, uint32_t *cp, int *ccols)
{
    int n = lrv_utf8_decode(s, cp);
    *ccols = cols(ctx, *cp);
    return n;
}

struct state {
    struct lrv_reflow *out;
    lrv_cols_fn *fn;
    void *ctx;
    int width, last, height, page, inpage, force;
    int first_col, indent;
    int nemitted;                   /* lines of the text so far (the first starts at first_col) */
    int have;                       /* a line in progress: */
    int indent_only;                /* ... holding only the kept spaces of an indented line */
    uint32_t start, end;
    int cols;
};

/* the column the line being filled starts at */
static int line_col(const struct state *st)
{
    return st->nemitted ? st->indent : st->first_col;
}

static void emit(struct state *st, uint32_t start, uint32_t end, int cols)
{
    struct lrv_reflow_line *l;
    int col = line_col(st);
    if (st->force || st->inpage >= st->height) {
        if (st->inpage)
            st->page++;
        st->inpage = 0;
        st->force = 0;
    }
    st->nemitted++;
    if (st->out->nlines == LRV_REFLOW_MAX_LINES) {
        st->out->dropped++;
        st->inpage++;
        return;
    }
    l = &st->out->line[st->out->nlines++];
    l->start = start;
    l->end = end;
    l->cols = cols;
    l->page = st->page;
    l->row = st->inpage;
    l->col = col;
    st->inpage++;
}

static void flush(struct state *st)
{
    if (st->have) {
        emit(st, st->start, st->end, st->cols);
        st->have = 0;
    }
}

/* the line of its page the line being filled will be */
static int slot(const struct state *st)
{
    return st->force || st->inpage >= st->height ? 0 : st->inpage;
}

/* the columns the line being filled holds */
static int cap(const struct state *st)
{
    return (slot(st) == st->height - 1 ? st->last : st->width) - line_col(st);
}

/* ... and the line after it (which starts at the indent) */
static int cap_next(const struct state *st)
{
    int s = slot(st) + 1 == st->height ? 0 : slot(st) + 1;
    return (s == st->height - 1 ? st->last : st->width) - st->indent;
}

/* the word text[i..j) (j at a separator or the NUL), w columns, at the start of a line */
static void place(struct state *st, const uint8_t *text, uint32_t i, uint32_t j, int w)
{
    for (;;) {
        uint32_t k = i, last = i;
        int cols = 0, width = cap(st);
        if (!st->nemitted && w > width && cap_next(st) > width) {
            emit(st, i, i, 0);                          /* the first line stays empty */
            continue;
        }
        while (k < j) {
            uint32_t cp;
            int c, n = lrv_reflow_char(text + k, st->fn, st->ctx, &cp, &c);
            if (cols + c > width && k != i)
                break;
            cols += c;
            last = k + (uint32_t)n;
            k += (uint32_t)n;
        }
        if (k >= j) {
            st->have = 1;
            st->indent_only = 0;
            st->start = i;
            st->end = last;
            st->cols = cols;
            return;
        }
        emit(st, i, last, cols);
        w -= cols;
        i = k;
    }
}

void lrv_reflow_layout_from(const uint8_t *text, int width, int height, int last,
                            const struct lrv_reflow_at *at, lrv_cols_fn *cols, void *ctx,
                            struct lrv_reflow *out)
{
    struct state st;
    uint32_t i = 0;
    int bol = 1;                                        /* at the start of a line of the text */
    int keep = at && (at->flags & LRV_REFLOW_KEEP_INDENT);
    memset(&st, 0, sizeof st);
    st.out = out;
    st.fn = cols;
    st.ctx = ctx;
    st.width = width;
    st.last = last;
    st.height = height;
    if (at) {
        st.inpage = at->first_line < 0 ? 0 : at->first_line > height ? height : at->first_line;
        st.first_col = at->first_col;
        st.indent = at->indent;
    }
    out->nlines = 0;
    out->dropped = 0;
    while (text[i]) {
        uint32_t cp, j;
        int c, w = 0;
        int n = lrv_reflow_char(text + i, cols, ctx, &cp, &c);
        if (cp == SP) {
            if (bol && keep && !st.have) {              /* an indented line keeps its spaces */
                int room = cap(&st);
                for (j = i; text[j] == SP; j++)
                    w++;
                if (w > room)
                    w = room < 0 ? 0 : room;
                st.have = 1;
                st.indent_only = 1;
                st.start = i;
                st.end = i + (uint32_t)w;
                st.cols = w;
                i = j;
                bol = 0;
                continue;
            }
            i += (uint32_t)n;
            continue;
        }
        if (cp == NL) {
            if (!st.have)
                emit(&st, i, i, 0);
            else
                flush(&st);
            i += (uint32_t)n;
            bol = 1;
            continue;
        }
        if (cp == PG) {
            flush(&st);
            if (st.inpage)
                st.force = 1;
            i += (uint32_t)n;
            bol = 1;
            continue;
        }
        bol = 0;
        for (j = i; text[j]; ) {                        /* the word and its width */
            uint32_t cq;
            int cc, m = lrv_reflow_char(text + j, cols, ctx, &cq, &cc);
            if (cq == SP || cq == NL || cq == PG)
                break;
            w += cc;
            j += (uint32_t)m;
        }
        if (st.have) {
            int gap = (int)(i - st.end);                /* the spaces between (1 byte each) */
            if (st.cols + gap + w <= cap(&st)) {
                st.end = j;
                st.cols += gap + w;
                st.indent_only = 0;
                i = j;
                continue;
            }
            if (st.indent_only)
                st.have = 0;                            /* the indent goes with the word */
            else
                flush(&st);
        }
        place(&st, text, i, j, w);
        i = j;
    }
    flush(&st);
    out->npages = out->nlines ? out->line[out->nlines - 1].page + 1 : 0;
}

void lrv_reflow_layout(const uint8_t *text, int width, int height, int last, lrv_cols_fn *cols,
                       void *ctx, struct lrv_reflow *out)
{
    lrv_reflow_layout_from(text, width, height, last, NULL, cols, ctx, out);
}

uint32_t lrv_reflow_fit(const uint8_t *text, int width, lrv_cols_fn *cols, void *ctx,
                        int *fit_cols)
{
    uint32_t i = 0;
    int w = 0;
    while (text[i]) {
        uint32_t cp;
        int c, n = lrv_reflow_char(text + i, cols, ctx, &cp, &c);
        if (cp == NL || cp == PG)
            c = 1;
        if (w + c > width)
            break;
        w += c;
        i += (uint32_t)n;
    }
    *fit_cols = w;
    return i;
}
