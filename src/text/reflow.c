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
    int have;                       /* a line in progress: */
    uint32_t start, end;
    int cols;
};

static void emit(struct state *st, uint32_t start, uint32_t end, int cols)
{
    struct lrv_reflow_line *l;
    if (st->force || st->inpage == st->height) {
        if (st->inpage)
            st->page++;
        st->inpage = 0;
        st->force = 0;
    }
    if (st->out->nlines == LRV_REFLOW_MAX_LINES) {
        st->out->dropped++;
        return;
    }
    l = &st->out->line[st->out->nlines++];
    l->start = start;
    l->end = end;
    l->cols = cols;
    l->page = st->page;
    st->inpage++;
}

static void flush(struct state *st)
{
    if (st->have) {
        emit(st, st->start, st->end, st->cols);
        st->have = 0;
    }
}

/* the width of the line being filled: which line of its page it will be */
static int cap(const struct state *st)
{
    int slot = st->force || st->inpage == st->height ? 0 : st->inpage;
    return slot == st->height - 1 ? st->last : st->width;
}

/* the word text[i..j) (j at a separator or the NUL) at the start of a line */
static void place(struct state *st, const uint8_t *text, uint32_t i, uint32_t j)
{
    for (;;) {
        uint32_t k = i, last = i;
        int cols = 0, width = cap(st);
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
            st->start = i;
            st->end = last;
            st->cols = cols;
            return;
        }
        emit(st, i, last, cols);
        i = k;
    }
}

void lrv_reflow_layout(const uint8_t *text, int width, int height, int last, lrv_cols_fn *cols,
                       void *ctx, struct lrv_reflow *out)
{
    struct state st;
    uint32_t i = 0;
    memset(&st, 0, sizeof st);
    st.out = out;
    st.fn = cols;
    st.ctx = ctx;
    st.width = width;
    st.last = last;
    st.height = height;
    out->nlines = 0;
    out->dropped = 0;
    while (text[i]) {
        uint32_t cp, j;
        int c, w = 0;
        int n = lrv_reflow_char(text + i, cols, ctx, &cp, &c);
        if (cp == SP) {
            i += (uint32_t)n;
            continue;
        }
        if (cp == NL) {
            if (!st.have)
                emit(&st, i, i, 0);
            else
                flush(&st);
            i += (uint32_t)n;
            continue;
        }
        if (cp == PG) {
            flush(&st);
            if (st.inpage)
                st.force = 1;
            i += (uint32_t)n;
            continue;
        }
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
                i = j;
                continue;
            }
            flush(&st);
        }
        place(&st, text, i, j);
        i = j;
    }
    flush(&st);
    out->npages = out->nlines ? out->line[out->nlines - 1].page + 1 : 0;
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
