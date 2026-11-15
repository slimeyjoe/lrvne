/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests of the graphics modules: the screen and its overlays, palette fade arithmetic,
 * the frame hash, planar to chunky conversion and scaling. Pages are filled with pseudo-random
 * bytes; conversions are compared with pixel-by-pixel references. */
#include "test_hal.h"
#include "lrvne/gfx.h"
#include "lrvne/p2c.h"
#include "lrvne/pc98.h"
#include "lrvne/scale.h"
#include "lrvne/sys.h"

#define W      LRV_PC98_W
#define H      LRV_PC98_H
#define STRIDE LRV_PC98_STRIDE
#define PLANE  (STRIDE * H)

static uint32_t g_seed = 12345;

static uint32_t rnd(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return g_seed >> 8;
}

static void fill_random(struct lrv_surface *s)
{
    int p, i;
    for (p = 0; p < 4; p++)
        for (i = 0; i < PLANE; i++)
            s->plane[p][i] = (uint8_t)rnd();
}

static int ref_pixel(const struct lrv_surface *s, int x, int y, int scroll)
{
    int sy = (y + scroll) % H, p, v = 0;
    for (p = 0; p < 4; p++)
        if (s->plane[p][sy * STRIDE + x / 8] & (0x80 >> (x & 7)))
            v |= 1 << p;
    return v;
}

/* ---- screen ------------------------------------------------------------------------------ */

static struct lrv_frame g_frame;        /* the last frame presented, and what it looked like */
static struct lrv_rect g_frame_dirty;
static uint8_t g_frame_byte;            /* byte 0 of plane 0 as the backend saw it */
static int g_presents;
static unsigned g_alloc_flags;

static void t_present(const struct lrv_frame *f)
{
    g_frame = *f;
    g_frame_dirty = *f->dirty;
    g_frame_byte = f->page->plane[0][0];
    g_presents++;
}

static void *t_alloc(size_t n, unsigned flags)
{
    g_alloc_flags = flags;
    return th_alloc(n, flags);
}

/* an overlay that draws one byte with save-under, and records the order of the calls */
struct ovl {
    int id, off;
    uint8_t value, saved;
    int applied;
};
static int g_order[8], g_norder;

static void ovl_apply(void *ctx, struct lrv_surface *page, struct lrv_rect *dirty)
{
    struct ovl *o = ctx;
    o->saved = page->plane[0][o->off];
    page->plane[0][o->off] = o->value;
    lrv_rect_add(dirty, o->off % STRIDE, o->off / STRIDE, o->off % STRIDE + 1,
                 o->off / STRIDE + 1);
    o->applied = 1;
    g_order[g_norder++ & 7] = o->id;
}

static void ovl_restore(void *ctx, struct lrv_surface *page, struct lrv_rect *dirty)
{
    struct ovl *o = ctx;
    page->plane[0][o->off] = o->saved;
    lrv_rect_add(dirty, o->off % STRIDE, o->off / STRIDE, o->off % STRIDE + 1,
                 o->off / STRIDE + 1);
    o->applied = 0;
    g_order[g_norder++ & 7] = -o->id;
}

static void test_screen(void)
{
    struct lrv_hal hal;
    struct lrv_screen scr;
    struct lrv_video_mode bad = lrv_pc98_video_mode;
    struct lrv_sys sys;
    struct lrv_rect r;
    struct ovl a = { 1, 0, 0xAA, 0, 0 }, b = { 2, 0, 0x55, 0, 0 };
    int p, i;

    th_hal(&hal);
    hal.video_present = t_present;
    hal.mem_alloc = t_alloc;

    /* what cannot be had */
    CHECK(lrv_screen_init(&scr, &hal, &lrv_pc98_video_mode, 0, 0, 0) == LRV_ERR_RANGE);
    CHECK(lrv_screen_init(&scr, &hal, &lrv_pc98_video_mode, 5, 0, 0) == LRV_ERR_RANGE);
    bad.format = LRV_FMT_NONE;
    CHECK(lrv_screen_init(&scr, &hal, &bad, 2, 0, 0) == LRV_ERR_RANGE);
    CHECK(th_live == 0);

    /* two pages and a buffer in one block, in order, with the flags asked for */
    CHECK(lrv_screen_init(&scr, &hal, &lrv_pc98_video_mode, 2, 1, LRV_MEM_CHIP) == LRV_OK);
    CHECK(g_alloc_flags == LRV_MEM_CHIP);
    CHECK(scr.mem_size == 3u * 4u * PLANE && th_live == (long)scr.mem_size);
    for (p = 0; p < 4; p++) {
        CHECK(scr.page[0].plane[p] == scr.mem + (0 * 4 + p) * PLANE);
        CHECK(scr.page[1].plane[p] == scr.mem + (1 * 4 + p) * PLANE);
        CHECK(scr.buffer[0].plane[p] == scr.mem + (2 * 4 + p) * PLANE);
    }
    CHECK(scr.page[0].stride == STRIDE && scr.page[1].w == W && scr.buffer[0].h == H);
    CHECK(scr.display_on == 1 && scr.page_switched == 1 && scr.access_page == 0);
    for (i = 0; i < (int)scr.mem_size; i++)
        if (scr.mem[i])
            break;
    CHECK(i == (int)scr.mem_size);

    /* pages */
    CHECK(lrv_screen_set_access(&scr, 1) == LRV_OK && lrv_screen_access(&scr) == &scr.page[1]);
    CHECK(lrv_screen_set_access(&scr, 2) == LRV_ERR_RANGE && scr.access_page == 1);
    CHECK(lrv_screen_set_display(&scr, -1) == LRV_ERR_RANGE && scr.display_page == 0);

    /* dirty rectangles: clipped, per page, the offset form and its spill over the edge */
    lrv_screen_mark(&scr, 1, -5, 10, 200, 12);
    CHECK(scr.dirty[1].x0 == 0 && scr.dirty[1].x1 == STRIDE && scr.dirty[1].y0 == 10 &&
          scr.dirty[1].y1 == 12);
    CHECK(lrv_rect_empty(&scr.dirty[0]));
    lrv_screen_mark(&scr, 7, 0, 0, 1, 1);                       /* no such page: ignored */
    lrv_screen_mark_off(&scr, 0, 20 * STRIDE + 5, 10, 3);
    CHECK(scr.dirty[0].x0 == 5 && scr.dirty[0].x1 == 15 && scr.dirty[0].y0 == 20 &&
          scr.dirty[0].y1 == 23);
    lrv_rect_clear(&scr.dirty[0]);
    lrv_screen_mark_off(&scr, 0, 20 * STRIDE + 75, 10, 3);
    CHECK(scr.dirty[0].x0 == 0 && scr.dirty[0].x1 == STRIDE && scr.dirty[0].y0 == 20 &&
          scr.dirty[0].y1 == 24);
    lrv_rect_clear(&scr.dirty[0]);
    lrv_rect_clear(&scr.dirty[1]);
    r.x0 = 1; r.y0 = 2; r.x1 = 3; r.y1 = 4;
    lrv_screen_mark_surface(&scr, &scr.buffer[0], &r);          /* not a page: nothing */
    CHECK(lrv_rect_empty(&scr.dirty[0]) && lrv_rect_empty(&scr.dirty[1]));
    lrv_screen_mark_surface(&scr, &scr.page[1], &r);
    CHECK(scr.dirty[1].x0 == 1 && scr.dirty[1].y1 == 4 && lrv_rect_empty(&scr.dirty[0]));

    /* clear and copy rows */
    fill_random(&scr.buffer[0]);
    fill_random(&scr.page[1]);
    lrv_screen_clear_access(&scr);
    CHECK(scr.page[1].plane[3][PLANE - 1] == 0 && scr.dirty[1].x1 == STRIDE &&
          scr.dirty[1].y1 == H);
    lrv_surface_copy_rows(&scr.page[1], &scr.buffer[0], 10 * STRIDE + 4, 6, 2);
    for (p = 0; p < 4; p++) {
        CHECK(!memcmp(scr.page[1].plane[p] + 10 * STRIDE + 4,
                      scr.buffer[0].plane[p] + 10 * STRIDE + 4, 6));
        CHECK(!memcmp(scr.page[1].plane[p] + 11 * STRIDE + 4,
                      scr.buffer[0].plane[p] + 11 * STRIDE + 4, 6));
        CHECK(scr.page[1].plane[p][10 * STRIDE + 3] == 0 &&
              scr.page[1].plane[p][10 * STRIDE + 10] == 0 &&
              scr.page[1].plane[p][12 * STRIDE + 4] == 0);
    }
    /* a line that would end past the plane is not copied */
    lrv_surface_copy_rows(&scr.page[1], &scr.buffer[0], (H - 1) * STRIDE + 70, 20, 1);
    CHECK(scr.page[1].plane[0][PLANE - 1] == 0);

    /* palette: masked to the mode's bits, out of range ignored */
    lrv_screen_set_colour(&scr, 3, 0x1F, 2, 15);
    lrv_screen_set_colour(&scr, 16, 1, 1, 1);
    CHECK(scr.rgb[9] == 15 && scr.rgb[10] == 2 && scr.rgb[11] == 15 && scr.rgb[48] == 0);

    /* present: the displayed page, its dirty rectangle, then clean */
    lrv_screen_reset(&scr);
    lrv_screen_mark(&scr, 0, 2, 3, 4, 5);
    lrv_screen_set_scroll(&scr, 7);
    lrv_screen_present(&scr);
    CHECK(g_presents == 1 && g_frame.page == &scr.page[0] && g_frame.page_switched == 1);
    CHECK(g_frame.scroll_line == 7 && g_frame.rgb == scr.rgb && g_frame.display_on == 1);
    CHECK(g_frame.colours == 16 && g_frame.rgb_bits == 4);      /* the frame describes itself */
    CHECK(g_frame_dirty.x0 == 2 && g_frame_dirty.y0 == 3 && g_frame_dirty.x1 == 4 &&
          g_frame_dirty.y1 == 5);
    CHECK(lrv_rect_empty(&scr.dirty[0]) && scr.page_switched == 0);
    lrv_screen_present(&scr);
    CHECK(g_frame.page_switched == 0 && lrv_rect_empty(&g_frame_dirty));

    /* switching the page or the display makes the next present a full one */
    lrv_screen_set_display(&scr, 1);
    lrv_screen_present(&scr);
    CHECK(g_frame.page == &scr.page[1] && g_frame.page_switched == 1);
    lrv_screen_set_display(&scr, 1);
    CHECK(scr.page_switched == 0);
    lrv_screen_set_display_on(&scr, 0);
    lrv_screen_present(&scr);
    CHECK(g_frame.display_on == 0 && g_frame.page_switched == 1);
    lrv_screen_set_display_on(&scr, 0);
    CHECK(scr.page_switched == 0);
    lrv_screen_set_display_on(&scr, 1);
    lrv_screen_set_display(&scr, 0);

    /* overlays: applied in order, the last on top, restored in reverse; the page is as it was
     * afterwards and what they touched is dirty for the next present */
    scr.page[0].plane[0][0] = 0x11;
    CHECK(lrv_screen_add_overlay(&scr, ovl_apply, ovl_restore, &a) == LRV_OK);
    CHECK(lrv_screen_add_overlay(&scr, ovl_apply, ovl_restore, &b) == LRV_OK);
    lrv_screen_present(&scr);
    lrv_rect_clear(&scr.dirty[0]);
    g_norder = 0;
    lrv_screen_present(&scr);
    CHECK(g_norder == 4 && g_order[0] == 1 && g_order[1] == 2 && g_order[2] == -2 &&
          g_order[3] == -1);
    CHECK(g_frame_byte == 0x55);                                /* the backend saw the top one */
    CHECK(g_frame_dirty.x0 == 0 && g_frame_dirty.x1 == 1 && g_frame_dirty.y1 == 1);
    CHECK(scr.page[0].plane[0][0] == 0x11 && !a.applied && !b.applied);
    CHECK(scr.dirty[0].x0 == 0 && scr.dirty[0].x1 == 1 && scr.dirty[0].y0 == 0 &&
          scr.dirty[0].y1 == 1);
    CHECK(lrv_screen_add_overlay(&scr, ovl_apply, ovl_restore, &a) == LRV_OK);
    CHECK(lrv_screen_add_overlay(&scr, ovl_apply, ovl_restore, &a) == LRV_OK);
    CHECK(lrv_screen_add_overlay(&scr, ovl_apply, ovl_restore, &a) == LRV_ERR_RANGE);
    scr.noverlays = 0;

    /* as the present callback of the frame loop */
    lrv_sys_init(&sys, &hal, W, H);
    lrv_sys_set_present(&sys, lrv_screen_present_hook, &scr);
    g_presents = 0;
    lrv_sys_frame_wait(&sys);
    lrv_sys_present_now(&sys);
    CHECK(g_presents == 2);

    lrv_screen_free(&scr);
    CHECK(th_live == 0 && scr.mem == 0);
}

/* ---- palette ----------------------------------------------------------------------------- */

static void test_palette(void)
{
    uint8_t cur[48], tgt[48], from[48], out[48];
    int i, steps, v, level;

    /* one unit per step towards the target; a 4-bit palette arrives in at most 15 steps */
    for (i = 0; i < 48; i++) {
        cur[i] = (uint8_t)(rnd() & 15);
        tgt[i] = (uint8_t)(rnd() & 15);
    }
    memcpy(from, cur, 48);
    for (steps = 0; lrv_pal_step_toward(cur, tgt, 48) != 0; steps++) {
        for (i = 0; i < 48; i++) {
            int d0 = abs(from[i] - tgt[i]), d = abs(cur[i] - tgt[i]);
            CHECK(d == (d0 > steps + 1 ? d0 - steps - 1 : 0));
        }
    }
    CHECK(steps >= 1 && steps <= 15 && !memcmp(cur, tgt, 48));
    CHECK(lrv_pal_step_toward(cur, tgt, 48) == 0);
    cur[0] = 3; tgt[0] = 5; cur[1] = 9; tgt[1] = 8; cur[2] = tgt[2] = 4;
    CHECK(lrv_pal_step_toward(cur, tgt, 3) == 2 && cur[0] == 4 && cur[1] == 8 && cur[2] == 4);

    /* linear interpolation truncating toward zero, both directions */
    from[0] = 0; tgt[0] = 15; from[1] = 15; tgt[1] = 0; from[2] = 7; tgt[2] = 7;
    lrv_pal_lerp(out, from, tgt, 3, 1, 4);
    CHECK(out[0] == 3 && out[1] == 12 && out[2] == 7);          /* 15/4 = 3; 15 - 3 = 12 */
    lrv_pal_lerp(out, from, tgt, 3, 3, 4);
    CHECK(out[0] == 11 && out[1] == 4);                         /* 45/4 = 11; 15 - 11 = 4 */
    lrv_pal_lerp(out, from, tgt, 3, 4, 4);
    CHECK(out[0] == 15 && out[1] == 0 && out[2] == 7);
    lrv_pal_lerp(from, from, tgt, 2, 2, 4);                     /* in place */
    CHECK(from[0] == 7 && from[1] == 8);

    /* brightness: round(v * level / 15), halves up; the ends are exact */
    for (v = 0; v < 16; v++) {
        CHECK(lrv_pal_bright(v, 15, 15) == v && lrv_pal_bright(v, 0, 15) == 0);
        for (level = 0; level < 16; level++) {
            int want = (int)((double)v * level / 15.0 + 0.5);
            CHECK(lrv_pal_bright(v, level, 15) == want);
        }
    }
    CHECK(lrv_pal_bright(1, 8, 15) == 1 && lrv_pal_bright(1, 7, 15) == 0);
    for (i = 0; i < 48; i++)
        from[i] = (uint8_t)(i & 15);
    lrv_pal_brightness(out, from, 48, 99, 15);                  /* clamped to full */
    CHECK(!memcmp(out, from, 48));
    lrv_pal_brightness(out, from, 48, -3, 15);
    for (i = 0; i < 48; i++)
        CHECK(out[i] == 0);
    lrv_pal_brightness(out, from, 48, 8, 15);
    CHECK(out[15] == 8 && out[1] == 1 && out[7] == 4);
}

/* ---- frame hash -------------------------------------------------------------------------- */

/* the algorithm the recorded hashes were made with, written out for the PC-98 screen: FNV-1a
 * over 400 lines from the scroll line, four planes of 80 bytes each per line, then 48 palette
 * bytes, then the display flag */
static uint32_t old_hash(uint8_t *const plane[4], int scroll, const uint8_t rgb4[48], int on)
{
    uint32_t h = 2166136261u;
    int p, y, x;
    for (y = 0; y < 400; y++) {
        int sy = (y + scroll) % 400;
        for (p = 0; p < 4; p++) {
            const uint8_t *row = plane[p] + sy * 80;
            for (x = 0; x < 80; x++)
                h = (h ^ row[x]) * 16777619u;
        }
    }
    for (x = 0; x < 48; x++)
        h = (h ^ rgb4[x]) * 16777619u;
    h = (h ^ (uint32_t)(on ? 1 : 0)) * 16777619u;
    return h;
}

static void test_hash(struct lrv_surface *page)
{
    struct lrv_frame f;
    uint8_t rgb[48];
    uint32_t h0;
    int i;

    for (i = 0; i < 48; i++)
        rgb[i] = (uint8_t)(rnd() & 15);
    memset(&f, 0, sizeof f);
    f.page = page;
    f.rgb = rgb;
    f.colours = 16;
    f.rgb_bits = 4;
    f.display_on = 1;
    for (i = 0; i < 20; i++) {
        f.scroll_line = i == 0 ? 0 : i == 1 ? 399 : (int)(rnd() % 400);
        f.display_on = i & 1;
        fill_random(page);
        CHECK(lrv_frame_hash32(&f) ==
              old_hash(page->plane, f.scroll_line, rgb, f.display_on));
    }
    /* a known value: the empty screen, black palette, display on */
    for (i = 0; i < 4; i++)
        memset(page->plane[i], 0, PLANE);
    memset(rgb, 0, sizeof rgb);
    f.scroll_line = 0;
    f.display_on = 1;
    h0 = lrv_frame_hash32(&f);
    CHECK(h0 == old_hash(page->plane, 0, rgb, 1));
    /* it depends on every part: a pixel, the scroll, a colour, the flag; not on what a
     * backend does not show (the dirty rectangle, the switch flag) */
    page->plane[2][PLANE - 1] ^= 1;
    CHECK(lrv_frame_hash32(&f) != h0);
    f.scroll_line = 1;
    CHECK(lrv_frame_hash32(&f) != old_hash(page->plane, 0, rgb, 1));
    f.scroll_line = 400;                                        /* a whole turn */
    CHECK(lrv_frame_hash32(&f) == old_hash(page->plane, 0, rgb, 1));
    f.scroll_line = 0;
    page->plane[2][PLANE - 1] ^= 1;
    rgb[47] = 1;
    CHECK(lrv_frame_hash32(&f) != h0);
    rgb[47] = 0;
    f.display_on = 0;
    CHECK(lrv_frame_hash32(&f) != h0);
    f.display_on = 2;                                           /* any nonzero is "on" */
    f.page_switched = 1;
    CHECK(lrv_frame_hash32(&f) == h0);
}

/* ---- planar to chunky -------------------------------------------------------------------- */

static uint8_t g_out8[(size_t)(W * LRV_SCALE_MAX + 24) * 1700];
static uint32_t g_out32[(W + 16) * H];

static void p2c_one(const struct lrv_p2c *t, const struct lrv_surface *pg, int x0, int y0, int x1,
                    int y1, int scroll, uint8_t base)
{
    size_t pitch = (size_t)(x1 - x0) * 8 + (rnd() % 3) * 8;    /* also wider than the rect */
    int x, y, bad = 0;
    memset(g_out8, 0xEE, (size_t)(W + 16) * H);
    lrv_p2c8(t, pg, x0, y0, x1, y1, scroll, g_out8, pitch, base);
    for (y = y0; y < y1 && !bad; y++) {
        for (x = x0 * 8; x < x1 * 8; x++)
            if (g_out8[(size_t)(y - y0) * pitch + (size_t)(x - x0 * 8)] !=
                base + ref_pixel(pg, x, y, scroll))
                bad = 1;
        for (x = (x1 - x0) * 8; x < (int)pitch && y + 1 < y1; x++)     /* padding untouched */
            if (g_out8[(size_t)(y - y0) * pitch + (size_t)x] != 0xEE)
                bad = 1;
    }
    CHECK(!bad);
}

static void rgba_one(const struct lrv_p2rgba *t, const struct lrv_surface *pg, int x0, int y0,
                     int x1, int y1, int scroll, const uint32_t lut[16])
{
    size_t pitch = (size_t)(x1 - x0) * 8 + (rnd() % 3) * 8;
    int x, y, bad = 0;
    memset(g_out32, 0xEE, sizeof g_out32);
    lrv_p2rgba(t, pg, x0, y0, x1, y1, scroll, lut, g_out32, pitch);
    for (y = y0; y < y1 && !bad; y++) {
        for (x = x0 * 8; x < x1 * 8; x++)
            if (g_out32[(size_t)(y - y0) * pitch + (size_t)(x - x0 * 8)] !=
                lut[ref_pixel(pg, x, y, scroll)])
                bad = 1;
        for (x = (x1 - x0) * 8; x < (int)pitch && y + 1 < y1; x++)
            if (g_out32[(size_t)(y - y0) * pitch + (size_t)x] != 0xEEEEEEEEu)
                bad = 1;
    }
    CHECK(!bad);
}

static void test_p2c(const struct lrv_surface *pg)
{
    static struct lrv_p2c t8;
    static struct lrv_p2rgba t32;
    uint32_t lut[16];
    uint8_t rgb[48];
    int i;

    lrv_p2c_init(&t8);
    lrv_p2rgba_init(&t32);
    for (i = 0; i < 48; i++)
        rgb[i] = (uint8_t)(rnd() & 15);
    rgb[0] = 15; rgb[1] = 0; rgb[2] = 8;

    /* the colour table: 4-bit guns times 17, R in the low byte, opaque; black when off */
    lrv_rgba_palette(lut, rgb, 16, 4, 1);
    CHECK(lut[0] == 0xFF8800FFu);
    for (i = 0; i < 16; i++)
        CHECK(lut[i] == (0xFF000000u | (uint32_t)rgb[i * 3 + 2] * 17u << 16 |
                         (uint32_t)rgb[i * 3 + 1] * 17u << 8 | (uint32_t)rgb[i * 3] * 17u));
    lrv_rgba_palette(lut, rgb, 16, 4, 0);
    for (i = 0; i < 16; i++)
        CHECK(lut[i] == 0xFF000000u);
    lrv_rgba_palette(lut, rgb, 16, 4, 1);

    p2c_one(&t8, pg, 0, 0, STRIDE, H, 0, 0);                    /* whole page */
    p2c_one(&t8, pg, 0, 0, STRIDE, H, 123, 16);                 /* whole page, scrolled */
    p2c_one(&t8, pg, 79, 399, 80, 400, 399, 16);                /* the last byte */
    rgba_one(&t32, pg, 0, 0, STRIDE, H, 0, lut);
    rgba_one(&t32, pg, 0, 0, STRIDE, H, 321, lut);
    rgba_one(&t32, pg, 79, 399, 80, 400, 399, lut);
    for (i = 0; i < 300; i++) {
        int x0 = (int)(rnd() % STRIDE), y0 = (int)(rnd() % H);
        int x1 = x0 + 1 + (int)(rnd() % (unsigned)(STRIDE - x0));
        int y1 = y0 + 1 + (int)(rnd() % (unsigned)(H - y0));
        int scroll = (int)(rnd() % H);
        p2c_one(&t8, pg, x0, y0, x1, y1, scroll, (uint8_t)((rnd() % 16) * 16));
        if (i < 60)
            rgba_one(&t32, pg, x0, y0, x1, y1, scroll, lut);
    }
}

/* ---- scaling ----------------------------------------------------------------------------- */

static void check_map(int dst_h)
{
    static struct lrv_rowmap m;
    int r, want = dst_h < H ? H : dst_h, bad = 0;
    lrv_rowmap_init(&m, H, dst_h);
    CHECK(m.src_h == H && m.dst_h == want && m.start[0] == 0 && m.start[H] == want);
    for (r = 0; r < H; r++) {
        int n = m.start[r + 1] - m.start[r];
        /* every row shown at least once, at most one more time than the average */
        if (n < 1 || n > want / H + 1)
            bad = 1;
        if (want == H && m.start[r] != r)
            bad = 1;
    }
    CHECK(!bad);
}

static void expand_one(const struct lrv_p2c *t, const struct lrv_surface *pg, int x0, int y0,
                       int x1, int y1, int scroll, int sx, int dst_h)
{
    static struct lrv_rowmap m;
    uint8_t pen[16];
    int w = (x1 - x0) * 8 * sx, pad = (int)(rnd() % 3) * 8, i, x, d, d0, d1, bad = 0;
    size_t pitch = (size_t)(w + pad);
    for (i = 0; i < 16; i++)
        pen[i] = (uint8_t)(rnd() & 0xFF);
    lrv_rowmap_init(&m, H, dst_h);
    d0 = m.start[y0];
    d1 = m.start[y1];
    memset(g_out8, 0xEE, (size_t)(d1 - d0 + 1) * pitch);
    lrv_expand8(t, pg, x0, y0, x1, y1, scroll, sx, &m, pen, g_out8, pitch);
    for (d = d0; d < d1 && !bad; d++) {
        int r = y0;
        while (m.start[r + 1] <= d)
            r++;
        for (x = 0; x < (int)pitch; x++) {
            uint8_t got = g_out8[(size_t)(d - d0) * pitch + (size_t)x];
            if (x < w ? got != pen[ref_pixel(pg, x0 * 8 + x / sx, r, scroll)] : got != 0xEE)
                bad = 1;
        }
    }
    for (x = 0; x < (int)pitch; x++)                            /* nothing below the last row */
        if (g_out8[(size_t)(d1 - d0) * pitch + (size_t)x] != 0xEE)
            bad = 1;
    CHECK(!bad);
}

static void test_scale(const struct lrv_surface *pg)
{
    static const int heights[] = { 400, 480, 512, 800, 960, 1200, 1440, 1600 };
    static const uint16_t first480[7] = { 0, 1, 2, 3, 4, 6, 7 };
    static struct lrv_p2c t;
    static struct lrv_rowmap m;
    const struct lrv_video_mode *pc98 = &lrv_pc98_video_mode;
    struct lrv_video_mode square = lrv_pc98_video_mode;
    int i;

    lrv_p2c_init(&t);
    for (i = 0; i < (int)(sizeof heights / sizeof heights[0]); i++)
        check_map(heights[i]);
    check_map(0);
    check_map(401);
    check_map(799);
    /* 400 -> 480: the fifth row of each five is the repeated one (start = floor(1.2 r)) */
    lrv_rowmap_init(&m, H, 480);
    CHECK(!memcmp(m.start, first480, sizeof first480) && m.start[399] == 478);
    /* other page heights, and the limits */
    lrv_rowmap_init(&m, 200, 400);
    CHECK(m.src_h == 200 && m.start[1] == 2 && m.start[200] == 400);
    lrv_rowmap_init(&m, 9999, 100);
    CHECK(m.src_h == LRV_ROWMAP_ROWS && m.dst_h == LRV_ROWMAP_ROWS);

    CHECK(lrv_aspect_height(pc98, 1, 1, 1) == 480);             /* square pixels */
    CHECK(lrv_aspect_height(pc98, 2, 1, 1) == 960);
    CHECK(lrv_aspect_height(pc98, 3, 1, 1) == 1440);
    CHECK(lrv_aspect_height(pc98, 1, 16, 15) == 512);           /* pixels wider than tall */
    CHECK(lrv_aspect_height(pc98, 1, 5, 6) == 400);             /* 640x400 already fills 4:3 */
    CHECK(lrv_aspect_height(pc98, 2, 1, 2) == 800);             /* never below h * sx */
    CHECK(lrv_aspect_height(pc98, 1, 0, 0) == 480);             /* unknown: square */
    CHECK(lrv_aspect_height(pc98, 1, 22, 22) == 480);
    CHECK(lrv_aspect_height(pc98, 1, 22, 26) == 400);           /* 406: within 2 % */
    CHECK(lrv_aspect_height(pc98, 2, 22, 26) == 800);
    square.aspect_w = 8;                                        /* a mode with square pixels */
    square.aspect_h = 5;
    CHECK(lrv_aspect_height(&square, 2, 1, 1) == 800);

    expand_one(&t, pg, 0, 0, STRIDE, H, 0, 1, 400);             /* identity */
    expand_one(&t, pg, 0, 0, STRIDE, H, 0, 2, 800);             /* x2 */
    expand_one(&t, pg, 0, 0, STRIDE, H, 77, 3, 1440);           /* x3 aspect, scrolled */
    expand_one(&t, pg, 0, 0, STRIDE, H, 0, 1, 512);
    expand_one(&t, pg, 79, 399, 80, 400, 399, 3, 1200);         /* the last byte */
    for (i = 0; i < 200; i++) {
        int x0 = (int)(rnd() % STRIDE), y0 = (int)(rnd() % H);
        int x1 = x0 + 1 + (int)(rnd() % (unsigned)(STRIDE - x0));
        int y1 = y0 + 1 + (int)(rnd() % (unsigned)(H - y0));
        int sx = 1 + (int)(rnd() % LRV_SCALE_MAX);
        int h = heights[rnd() % (sizeof heights / sizeof heights[0])];
        if (h < H * sx)
            h = H * sx + (int)(rnd() % 200);
        expand_one(&t, pg, x0, y0, x1, y1, (int)(rnd() % H), sx, h);
    }
}

int main(void)
{
    static uint8_t mem[4][PLANE];
    struct lrv_surface pg;
    int p;

    memset(&pg, 0, sizeof pg);
    pg.format = LRV_FMT_PLANAR4;
    pg.w = W;
    pg.h = H;
    pg.stride = STRIDE;
    for (p = 0; p < 4; p++)
        pg.plane[p] = mem[p];

    test_screen();
    test_palette();
    test_hash(&pg);
    fill_random(&pg);
    test_p2c(&pg);
    test_scale(&pg);
    if (th_fails) {
        fprintf(stderr, "test_gfx: %d failed\n", th_fails);
        return 1;
    }
    printf("test_gfx: ok\n");
    return 0;
}
