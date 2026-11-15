/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic presentation test (ptest.h). */
#include <stdlib.h>
#include <string.h>
#include "lrvne/core.h"
#include "ptest.h"
#include "shot.h"

#define NPLANES 4
#define REF_COLS 80             /* the sequence is written for 80 byte columns x 400 lines */
#define REF_H    400

static const struct lrv_ptest_io *g_io;
static struct lrv_video_mode g_mode;
static struct lrv_surface g_pg[2];
static struct lrv_rect g_dirty[2];
static int g_disp, g_switched, g_scroll, g_on, g_nsnap;
static int g_cols, g_h;         /* byte columns and lines of the mode */
static uint8_t *g_pal;
static int g_npal;              /* palette bytes */
static unsigned g_max;          /* the largest value of a gun */
static uint32_t g_rng;

/* palettes of 16 colours with guns 0..15 (scaled to the mode): A distinct colours with black
 * as colour 0, B another set */
static const uint8_t PAL_A[48] = {
    0, 0, 0,   15, 15, 15,  15, 0, 0,   0, 15, 0,   0, 0, 15,   15, 15, 0,  0, 15, 15,  15, 0, 15,
    8, 8, 8,   8, 0, 0,     0, 8, 0,    0, 0, 8,    8, 8, 0,    0, 8, 8,    8, 0, 8,    4, 9, 14
};
static const uint8_t PAL_B[48] = {
    0, 0, 0,   1, 2, 3,     4, 5, 6,    7, 8, 9,    10, 11, 12, 13, 14, 15, 15, 7, 3,   3, 7, 15,
    12, 4, 1,  1, 12, 4,    4, 1, 12,   9, 9, 2,    2, 9, 9,    9, 2, 9,    14, 14, 6,  6, 6, 14
};

static uint8_t gun(unsigned v15)
{
    return (uint8_t)(v15 * g_max / 15u);
}

static void set_palette(const uint8_t *pal16)
{
    int i;
    for (i = 0; i < g_npal; i++)
        g_pal[i] = gun(pal16[i % 48]);
}

static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* pseudo-random bytes in byte columns [x0,x1) x lines [y0,y1) of all planes */
static void draw(int page, int x0, int y0, int x1, int y1, uint32_t seed)
{
    int p, y, x;
    g_rng = seed * 2654435761u + 1u;
    for (p = 0; p < NPLANES; p++)
        for (y = y0; y < y1; y++)
            for (x = x0; x < x1; x++)
                g_pg[page].plane[p][y * g_pg[page].stride + x] = (uint8_t)(rnd() >> 11);
    lrv_rect_add(&g_dirty[page], x0, y0, x1, y1);
}

static int scale(int v, int ref, int size)
{
    return (int)((long)v * size / ref);
}

/* the same, the rectangle given on the reference picture */
static void draw_ref(int page, int x0, int y0, int x1, int y1, uint32_t seed)
{
    x0 = scale(x0, REF_COLS, g_cols);
    x1 = scale(x1, REF_COLS, g_cols);
    y0 = scale(y0, REF_H, g_h);
    y1 = scale(y1, REF_H, g_h);
    if (x1 <= x0)
        x1 = x0 + 1;
    if (y1 <= y0)
        y1 = y0 + 1;
    draw(page, x0, y0, x1, y1, seed);
}

/* colour index c at pixel (x, y) */
static void put(int page, int x, int y, int c)
{
    int p, off = y * g_pg[page].stride + x / 8;
    uint8_t bit = (uint8_t)(0x80u >> (x & 7));
    for (p = 0; p < NPLANES; p++) {
        if (c & (1 << p))
            g_pg[page].plane[p][off] |= bit;
        else
            g_pg[page].plane[p][off] &= (uint8_t)~bit;
    }
}

/* registration image: black, a one-pixel white frame on the outermost pixels, and corner
 * blocks of colours 2..5 (top left, top right, bottom left, bottom right) */
static void draw_registration(int page)
{
    int w = g_pg[page].w, h = g_h, p, x, y;
    for (p = 0; p < NPLANES; p++)
        memset(g_pg[page].plane[p], 0, lrv_surface_plane_bytes(&g_pg[page]));
    for (x = 0; x < w; x++) {
        put(page, x, 0, 1);
        put(page, x, h - 1, 1);
    }
    for (y = 0; y < h; y++) {
        put(page, 0, y, 1);
        put(page, w - 1, y, 1);
    }
    for (y = 4; y < 20; y++)
        for (x = 4; x < 20; x++) {
            put(page, x, y, 2);
            put(page, w - 1 - x, y, 3);
            put(page, x, h - 1 - y, 4);
            put(page, w - 1 - x, h - 1 - y, 5);
        }
    lrv_rect_add(&g_dirty[page], 0, 0, g_cols, h);
}

static void make_frame(struct lrv_frame *f)
{
    memset(f, 0, sizeof *f);
    f->page = &g_pg[g_disp];
    f->dirty = &g_dirty[g_disp];
    f->page_switched = g_switched;
    f->scroll_line = g_scroll;
    f->rgb = g_pal;
    f->display_on = g_on;
}

static void present(void)
{
    struct lrv_frame f;
    make_frame(&f);
    g_io->present(&f);
    lrv_rect_clear(&g_dirty[g_disp]);
    g_switched = 0;
}

static void flip(int page)
{
    if (page != g_disp) {
        g_disp = page;
        g_switched = 1;
    }
}

static void snap(const char *name)
{
    struct lrv_frame f;
    make_frame(&f);
    f.dirty = 0;
    f.page_switched = 0;
    g_io->snap(g_nsnap, name, &f, lrv_shot_hash32(&f, &g_mode));
    g_nsnap++;
}

static void sequence(void)
{
    int i, cols = g_cols, h = g_h;

    draw_registration(0);                       /* first upload */
    present();
    snap("registration");

    draw(0, 0, 0, cols, h, 1);
    present();
    snap("full");

    draw_ref(0, 3, 13, 8, 58, 2);               /* odd byte columns and lines */
    present();
    snap("dirty-odd");

    draw(0, cols - 1, 0, cols, 1, 3);           /* the four edges, one present each */
    present();
    draw(0, 0, h - 1, 1, h, 4);
    present();
    draw(0, 0, 0, cols, 2, 5);
    present();
    draw_ref(0, 77, 150, 80, 400, 6);
    present();
    snap("dirty-edges");

    set_palette(PAL_B);                         /* palette only, empty dirty rectangle */
    present();
    snap("palette");

    g_on = 0;
    present();
    snap("graphics-off");

    g_on = 1;
    draw_ref(0, 10, 100, 30, 140, 7);
    present();
    snap("graphics-on-dirty");

    draw(1, 0, 0, cols, h, 8);                  /* page 1 drawn while hidden, first flip */
    flip(1);
    present();
    snap("flip-1");

    draw_ref(1, 20, 200, 60, 260, 9);
    present();
    snap("flip-1-dirty");

    draw_ref(0, 40, 0, 80, 100, 10);            /* page 0 changed while hidden */
    flip(0);
    present();
    snap("flip-0");

    g_scroll = scale(40, REF_H, h);
    present();
    snap("scroll-40");

    draw_ref(0, 5, 100, 25, 140, 11);           /* redraws while scrolled, scroll unchanged */
    present();
    draw_ref(0, 50, 380, 70, 400, 12);          /* lines that wrap to the top of the screen */
    present();
    draw_ref(0, 60, 0, 70, 10, 13);
    present();
    draw_ref(0, 30, 30, 34, 50, 14);            /* lines that straddle the wrap */
    present();
    snap("scroll-40-dirty");

    g_scroll = scale(360, REF_H, h);
    present();
    snap("scroll-360");

    draw_ref(0, 0, 350, 80, 370, 15);           /* straddles display line 0 */
    present();
    draw_ref(0, 33, 395, 34, 400, 16);
    present();
    snap("scroll-360-dirty");

    draw(1, 0, 0, cols, h, 17);                 /* flip while scrolled */
    flip(1);
    present();
    snap("flip-scrolled");

    draw_ref(1, 70, 300, 80, 399, 18);          /* redraw in the other buffer, still scrolled */
    present();
    snap("flip-scrolled-dirty");

    g_scroll = 0;
    draw(1, 1, 1, 2, 2, 19);
    present();
    snap("unscroll");

    set_palette(PAL_A);                         /* colour 0 non-black: a border stays black */
    g_pal[0] = gun(15);
    g_pal[1] = 0;
    g_pal[2] = 0;
    present();
    snap("border-red");

    memset(g_pal, gun(15), (size_t)g_npal);     /* fade to white */
    present();
    snap("fade-white");

    g_on = 0;
    present();
    snap("fade-white-off");

    g_on = 1;                                   /* three flips in a row, one snapshot */
    set_palette(PAL_A);
    draw(0, 0, 0, cols, h, 20);
    flip(0);
    present();
    g_scroll = scale(123, REF_H, h);
    draw_ref(1, 10, 10, 20, 20, 21);
    flip(1);
    present();
    flip(0);
    present();
    snap("flip-flip-flip");

    /* soak: random redraws, scrolls and flips */
    g_rng = 0x5eed;
    for (i = 0; i < 200; i++) {
        uint32_t r = rnd();
        int page = (r & 0x30) == 0x30 ? 1 - g_disp : g_disp;
        int x0 = (int)((r >> 8) % (uint32_t)cols), y0 = (int)((r >> 16) % (uint32_t)h);
        int x1 = x0 + 1 + (int)((r >> 4) % (uint32_t)(cols - x0));
        int y1 = y0 + 1 + (int)((r >> 20) % (uint32_t)(h - y0) / 4u);
        uint32_t saved = g_rng;
        draw(page, x0, y0, x1, y1, r);
        g_rng = saved;
        if ((r & 0x700) == 0x700)
            g_scroll = (int)((r >> 12) % (uint32_t)h);
        else if ((r & 0x700) == 0x600)
            g_scroll = 0;
        if ((r & 0x3000) == 0x3000)
            flip(1 - g_disp);
        if ((r & 0xC000) == 0xC000)
            g_pal[(r >> 24) % (uint32_t)g_npal] = gun((r >> 28) & 15);
        present();
        if (i % 50 == 49) {
            static const char *const names[] = { "soak-1", "soak-2", "soak-3", "soak-4" };
            snap(names[i / 50]);
        }
    }
}

int lrv_ptest_run(const struct lrv_video_mode *mode, const struct lrv_ptest_io *io)
{
    size_t plane;
    uint8_t *mem;
    int i, p;

    if (mode->format != LRV_FMT_PLANAR4 || mode->w < 64 || mode->h < 64 || mode->colours < 16 ||
        mode->colours > 256 || mode->rgb_bits < 1 || mode->rgb_bits > 8)
        return LRV_ERR_RANGE;
    g_mode = *mode;
    g_cols = (mode->w + 7) / 8;
    g_h = mode->h;
    g_npal = mode->colours * 3;
    g_max = (1u << mode->rgb_bits) - 1u;
    plane = (size_t)g_cols * (size_t)g_h;
    mem = calloc(2 * NPLANES * plane + (size_t)g_npal, 1);
    if (!mem)
        return LRV_ERR_NOMEM;
    for (i = 0; i < 2; i++) {
        g_pg[i].format = LRV_FMT_PLANAR4;
        g_pg[i].w = mode->w;
        g_pg[i].h = mode->h;
        g_pg[i].stride = g_cols;
        for (p = 0; p < NPLANES; p++)
            g_pg[i].plane[p] = mem + (size_t)(i * NPLANES + p) * plane;
        lrv_rect_clear(&g_dirty[i]);
    }
    g_pal = mem + 2 * NPLANES * plane;
    g_io = io;
    g_nsnap = 0;
    g_disp = 0;
    g_switched = 0;
    g_scroll = 0;
    g_on = 1;
    set_palette(PAL_A);

    sequence();

    free(mem);
    g_pal = 0;
    return g_nsnap;
}
