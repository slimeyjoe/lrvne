/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests of the text modules: Shift-JIS and UTF-8, glyph tables and the lookups on them,
 * glyph cells on a planar surface, and reflow. The fonts are synthetic: every glyph is a
 * pattern computed from its code. */
#include "test_hal.h"
#include "lrvne/font.h"
#include "lrvne/reflow.h"
#include "lrvne/sjis.h"
#include "lrvne/utf8.h"

/* ---- encodings --------------------------------------------------------------------------- */

static void test_sjis(void)
{
    CHECK(!lrv_sjis_is_lead(0x80) && lrv_sjis_is_lead(0x81) && lrv_sjis_is_lead(0x9F));
    CHECK(!lrv_sjis_is_lead(0xA0) && !lrv_sjis_is_lead(0xDF) && lrv_sjis_is_lead(0xE0));
    CHECK(lrv_sjis_is_lead(0xFC) && !lrv_sjis_is_lead(0xFD) && !lrv_sjis_is_lead('A'));
    CHECK(lrv_sjis_to_jis(0x81, 0x40) == 0x2121);       /* the first cell */
    CHECK(lrv_sjis_to_jis(0x81, 0x41) == 0x2122);
    CHECK(lrv_sjis_to_jis(0x82, 0xA0) == 0x2422);       /* hiragana a: the even row */
    CHECK(lrv_sjis_to_jis(0x88, 0x9F) == 0x3021);       /* the first kanji */
    CHECK(lrv_sjis_to_jis(0x81, 0x80) == 0x2160);       /* past the gap at 7Fh */
    CHECK(lrv_sjis_to_jis(0xE0, 0x40) == 0x5F21);       /* the second lead range */
    CHECK(lrv_sjis_to_jis(0xEA, 0xA4) == 0x7426);
}

static void utf8_one(const char *s, int len, uint32_t want)
{
    uint32_t cp = 0;
    CHECK(lrv_utf8_decode((const uint8_t *)s, &cp) == len && cp == want);
}

static void test_utf8(void)
{
    utf8_one("A", 1, 'A');
    utf8_one("\x7F", 1, 0x7F);
    utf8_one("\xC4\x85", 2, 0x105);
    utf8_one("\xC2\x80", 2, 0x80);
    utf8_one("\xE2\x96\xBC", 3, 0x25BC);
    utf8_one("\xEF\xBF\xBF", 3, 0xFFFF);
    utf8_one("\xF0\x9F\x98\x80", 4, 0x1F600);
    utf8_one("\xF4\x8F\xBF\xBF", 4, 0x10FFFF);
    /* what is not UTF-8 is U+FFFD and one byte, so nothing after it is swallowed */
    utf8_one("\xC0\x80", 1, LRV_UTF8_REPLACEMENT);          /* overlong */
    utf8_one("\xE0\x80\x80", 1, LRV_UTF8_REPLACEMENT);
    utf8_one("\xED\xA0\x80", 1, LRV_UTF8_REPLACEMENT);      /* a surrogate */
    utf8_one("\xF4\x90\x80\x80", 1, LRV_UTF8_REPLACEMENT);  /* above U+10FFFF */
    utf8_one("\xF5\x80\x80\x80", 1, LRV_UTF8_REPLACEMENT);
    utf8_one("\x80", 1, LRV_UTF8_REPLACEMENT);              /* a stray trail byte */
    utf8_one("\xE2\x96", 1, LRV_UTF8_REPLACEMENT);          /* cut by the NUL */
    utf8_one("\xE2\x41\x41", 1, LRV_UTF8_REPLACEMENT);
}

/* ---- synthetic glyph tables -------------------------------------------------------------- */

/* byte k of the glyph of `code` in a table of `wbytes` columns */
static uint8_t glyph_byte(unsigned code, int wbytes, int k)
{
    return (uint8_t)(code * 7u + (unsigned)k * 13u + (unsigned)wbytes);
}

/* an XFNT table of n ascending codes; returns its size */
static uint32_t make_font(uint8_t *buf, int cell_w, int cell_h, const uint16_t *codes, int n)
{
    int gb = cell_w / 8 * cell_h, i, k;
    uint8_t *p = buf + LRV_FONT_HEADER_SIZE;
    memset(buf, 0, LRV_FONT_HEADER_SIZE);
    memcpy(buf, "XFNT", 4);
    buf[5] = (uint8_t)cell_w;
    buf[6] = (uint8_t)cell_h;
    lrv_wr16le(buf + 8, (uint16_t)n);
    for (i = 0; i < n; i++, p += 2)
        lrv_wr16le(p, codes[i]);
    for (i = 0; i < n; i++)
        for (k = 0; k < gb; k++)
            *p++ = glyph_byte(codes[i], cell_w / 8, k);
    return (uint32_t)(p - buf);
}

static int glyph_is(const uint8_t *g, unsigned code, int wbytes, int n)
{
    int k;
    for (k = 0; k < n; k++)
        if (g[k] != glyph_byte(code, wbytes, k))
            return 0;
    return 1;
}

static int all_zero(const uint8_t *p, int n)
{
    int k;
    for (k = 0; k < n; k++)
        if (p[k])
            return 0;
    return 1;
}

static const uint16_t codes16[] = { 0x2121, 0x2422, 0x3021, 0x7426 };
static const uint16_t codes8[] = { 0x20, 0x41, 0x7E, 0xB1, 0xDF };
static const uint16_t codes_latin[] = { 0x20, 0x41, 0x61, 0x105, 0x142 };
static const uint16_t codes_wide[] = { 0x25BC, 0x3042, 0xE000 };

static void test_font(void)
{
    static struct th_builder b;
    static uint8_t f16[256], f8[256], lat[256], wide[256], big[512], cut[256];
    uint32_t n16, n8, nlat, nwide, nbig, ncut;
    struct lrv_hal hal;
    struct lrv_pack pk;
    struct lrv_font font16, font8, latin8, wide16, f;
    struct lrv_text_ch ch;
    uint8_t rows[32];
    const uint8_t *g;

    n16 = make_font(f16, 16, 16, codes16, 4);
    n8 = make_font(f8, 8, 16, codes8, 5);
    nlat = make_font(lat, 8, 16, codes_latin, 5);
    nwide = make_font(wide, 16, 16, codes_wide, 3);
    nbig = make_font(big, 24, 24, codes16, 2);
    ncut = make_font(cut, 16, 16, codes16, 4) - 1;      /* the last glyph is a byte short */

    th_hal(&hal);
    th_pack_begin(&b, 8);
    th_pack_add(&b, "font16", LRV_PT_FONT, 0, f16, n16);
    th_pack_add(&b, "font8", LRV_PT_FONT, 0, f8, n8);
    th_pack_add(&b, "latin8", LRV_PT_FONT, 0, lat, nlat);
    th_pack_add(&b, "wide16", LRV_PT_FONT, 0, wide, nwide);
    th_pack_add(&b, "big", LRV_PT_FONT, 0, big, nbig);
    th_pack_add(&b, "cut", LRV_PT_FONT, 0, cut, ncut);
    th_pack_add(&b, "junk", LRV_PT_FONT, 0, "XFNx............", 16);
    th_pack_add(&b, "blob", LRV_PT_BLOB, 0, f16, n16);
    th_pack_end(&b, LRV_PACK_MAIN);
    CHECK(lrv_pack_open(&pk, &hal, LRV_PACK_MAIN) == LRV_OK);

    /* loading: the cell asked for, any cell, and everything that is refused leaves nothing */
    CHECK(lrv_font_load(&font16, &pk, "font16", 16, 16) == LRV_OK);
    CHECK(font16.cell_w == 16 && font16.cell_h == 16 && font16.count == 4 &&
          font16.glyph_bytes == 32);
    CHECK(lrv_font_load(&font8, &pk, "FONT8", 8, 16) == LRV_OK && font8.glyph_bytes == 16);
    CHECK(lrv_font_load(&latin8, &pk, "latin8", 8, 16) == LRV_OK);
    CHECK(lrv_font_load(&wide16, &pk, "wide16", 16, 16) == LRV_OK);
    CHECK(lrv_font_load(&f, &pk, "big", 0, 0) == LRV_OK && f.cell_w == 24 && f.glyph_bytes == 72);
    g = lrv_font_glyph(&f, 0x2422);
    CHECK(g && glyph_is(g, 0x2422, 3, 72));
    lrv_font_free(&f);
    CHECK(f.data == 0 && lrv_font_glyph(&f, 0x2422) == 0);
    {
        long live = th_live;
        CHECK(lrv_font_load(&f, &pk, "big", 16, 16) == LRV_ERR_FORMAT && f.data == 0);
        CHECK(lrv_font_load(&f, &pk, "font8", 16, 16) == LRV_ERR_FORMAT);
        CHECK(lrv_font_load(&f, &pk, "cut", 16, 16) == LRV_ERR_FORMAT);
        CHECK(lrv_font_load(&f, &pk, "junk", 0, 0) == LRV_ERR_FORMAT);
        CHECK(lrv_font_load(&f, &pk, "nofont", 0, 0) == LRV_ERR_NOTFOUND);
        CHECK(lrv_font_load(&f, &pk, "blob", 0, 0) == LRV_ERR_NOTFOUND);    /* not a font entry */
        CHECK(th_live == live);
        lrv_font_free(&f);                                      /* an empty font: nothing */
        CHECK(th_live == live);
    }

    /* a table in the caller's memory */
    CHECK(lrv_font_parse(&f, f8, n8, 8, 16) == LRV_OK && f.pack == 0 && f.count == 5);
    CHECK(lrv_font_glyph(&f, 0x41) == f8 + LRV_FONT_HEADER_SIZE + 10 + 16);
    lrv_font_free(&f);
    CHECK(lrv_font_parse(&f, f8, 11, 0, 0) == LRV_ERR_FORMAT && f.data == 0);

    /* lookup: the first, the last, between, outside */
    CHECK(glyph_is(lrv_font_glyph(&font16, 0x2121), 0x2121, 2, 32));
    CHECK(glyph_is(lrv_font_glyph(&font16, 0x7426), 0x7426, 2, 32));
    CHECK(!lrv_font_glyph(&font16, 0x2120) && !lrv_font_glyph(&font16, 0x3022) &&
          !lrv_font_glyph(&font16, 0x7427) && !lrv_font_glyph(0, 0x2121));

    /* the kanji ROM: full-width, the two half-width rows, the blank row, no glyph */
    memset(rows, 0xEE, sizeof rows);
    CHECK(lrv_font_rom_glyph(&font16, &font8, 0x3021, rows) == 2 && glyph_is(rows, 0x3021, 2, 32));
    CHECK(lrv_font_rom_glyph(&font16, &font8, 0x2941, rows) == 1 &&
          glyph_is(rows, 0x41, 1, 16) && all_zero(rows + 16, 16));
    CHECK(lrv_font_rom_glyph(&font16, &font8, 0x2A31, rows) == 1 && glyph_is(rows, 0xB1, 1, 16));
    CHECK(lrv_font_rom_glyph(&font16, &font8, 0x2B41, rows) == 1 && all_zero(rows, 32));
    CHECK(lrv_font_rom_glyph(&font16, &font8, 0x2942, rows) == 1 && all_zero(rows, 32));
    CHECK(lrv_font_rom_glyph(&font16, &font8, 0x3022, rows) == 2 && all_zero(rows, 32));
    CHECK(lrv_font_rom_glyph(0, 0, 0x3021, rows) == 2 && all_zero(rows, 32));
    CHECK(lrv_font_rom_glyph(&font8, &font16, 0x3021, rows) == 2 && all_zero(rows, 32));

    /* translated text: one column from the Latin table, two otherwise, none for controls */
    CHECK(lrv_font_uni_cols(&latin8, 0x0A) == 0 && lrv_font_uni_cols(&latin8, 'A') == 1);
    CHECK(lrv_font_uni_cols(&latin8, 0x105) == 1 && lrv_font_uni_cols(&latin8, 'B') == 2);
    CHECK(lrv_font_uni_cols(&latin8, 0x3042) == 2 && lrv_font_uni_cols(&latin8, 0x1F600) == 2);
    CHECK(lrv_font_uni_cols(0, 'A') == 2 && lrv_font_uni_cols_fn(&latin8, 0x142) == 1);
    CHECK(lrv_font_uni_glyph(&latin8, &wide16, 0x142, rows) == 1 &&
          glyph_is(rows, 0x142, 1, 16) && all_zero(rows + 16, 16));
    CHECK(lrv_font_uni_glyph(&latin8, &wide16, 0x3042, rows) == 2 &&
          glyph_is(rows, 0x3042, 2, 32));
    CHECK(lrv_font_uni_glyph(&latin8, &wide16, 0x3043, rows) == 2 && all_zero(rows, 32));
    CHECK(lrv_font_uni_glyph(&latin8, 0, 0x3042, rows) == 2 && all_zero(rows, 32));
    CHECK(lrv_font_uni_glyph(&latin8, &wide16, 0x0C, rows) == 0 && all_zero(rows, 32));

    /* character streams */
    CHECK(lrv_text_next_sjis((const uint8_t *)"\x82\xA0", &ch) == 2 && ch.code == 0x2422 &&
          ch.cols == 2);
    CHECK(lrv_text_next_sjis((const uint8_t *)"\xB1", &ch) == 1 && ch.code == 0xB1 &&
          ch.cols == 1);
    CHECK(lrv_text_next_sjis((const uint8_t *)"\x82", &ch) == 1 && ch.code == 0x82 &&
          ch.cols == 1);                                        /* a lead byte before the NUL */
    CHECK(lrv_text_next_utf8((const uint8_t *)"B", &latin8, &ch) == 1 && ch.cols == 1);
    CHECK(lrv_text_next_utf8((const uint8_t *)"\xC4\x85", &latin8, &ch) == 2 &&
          ch.code == 0x105 && ch.cols == 1);
    CHECK(lrv_text_next_utf8((const uint8_t *)"\xE3\x81\x82", &latin8, &ch) == 3 &&
          ch.code == 0x3042 && ch.cols == 2);
    CHECK(lrv_text_next_utf8((const uint8_t *)"\xF0\x9F\x98\x80", &latin8, &ch) == 4 &&
          ch.code == LRV_UTF8_REPLACEMENT && ch.cols == 2);
    CHECK(lrv_text_next_utf8((const uint8_t *)"\xC4\x85", 0, &ch) == 2 && ch.cols == 2);

    lrv_font_free(&font16);
    lrv_font_free(&font8);
    lrv_font_free(&latin8);
    lrv_font_free(&wide16);
    lrv_pack_close(&pk);
    CHECK(th_live == 0);
}

/* ---- glyph cells ------------------------------------------------------------------------- */

#define SW     64
#define SH     40
#define SSTR   (SW / 8)
#define SPLANE (SSTR * SH)

static int pixel(const struct lrv_surface *s, int x, int y)
{
    int p, v = 0;
    for (p = 0; p < 4; p++)
        if (s->plane[p][y * s->stride + x / 8] & (0x80 >> (x & 7)))
            v |= 1 << p;
    return v;
}

static void test_blit(void)
{
    static uint8_t mem[4][SPLANE], before[4][SPLANE];
    struct lrv_surface s;
    struct lrv_rect dirty;
    uint8_t glyph[32];
    int p, i, t;

    memset(&s, 0, sizeof s);
    s.format = LRV_FMT_PLANAR4;
    s.w = SW;
    s.h = SH;
    s.stride = SSTR;
    for (p = 0; p < 4; p++)
        s.plane[p] = mem[p];
    for (i = 0; i < 32; i++)
        glyph[i] = (uint8_t)(i * 37 + 11);

    for (t = 0; t < 64; t++) {
        int wb = 1 + (t & 1), fg = (t * 5) & 15;
        int bg = t % 5 == 4 ? LRV_GLYPH_CLEAR : (t * 3 + 1) & 15;
        int col = (t * 3) % (SSTR - 1), y = (t * 7) % (SH - 16), x, yy, bad = 0;
        unsigned planes = t < 48 ? 15u : (unsigned)(t & 15);
        for (p = 0; p < 4; p++)
            for (i = 0; i < SPLANE; i++)
                mem[p][i] = before[p][i] = (uint8_t)(i * 31 + p * 17 + t);
        lrv_rect_clear(&dirty);
        CHECK(lrv_glyph_blit(&s, col, y, glyph, wb, 16, fg, bg, planes, &dirty) == 1);
        CHECK(dirty.x0 == col && dirty.x1 == col + wb && dirty.y0 == y && dirty.y1 == y + 16);
        for (yy = 0; yy < SH; yy++)
            for (x = 0; x < SW; x++) {
                int in = x >= col * 8 && x < (col + wb) * 8 && yy >= y && yy < y + 16;
                int old = (before[0][yy * SSTR + x / 8] >> (7 - (x & 7)) & 1) |
                          (before[1][yy * SSTR + x / 8] >> (7 - (x & 7)) & 1) << 1 |
                          (before[2][yy * SSTR + x / 8] >> (7 - (x & 7)) & 1) << 2 |
                          (before[3][yy * SSTR + x / 8] >> (7 - (x & 7)) & 1) << 3;
                int want = old;
                if (in) {
                    int gx = x - col * 8;
                    int ink = glyph[(yy - y) * wb + gx / 8] >> (7 - (gx & 7)) & 1;
                    /* the planes written take the colour's bit, the others keep theirs */
                    int c = ink ? fg : bg == LRV_GLYPH_CLEAR ? old : bg;
                    /* (a transparent cell keeps the old colour where there is no ink) */
                    want = (int)(((unsigned)c & planes) | ((unsigned)old & ~planes));
                }
                if (pixel(&s, x, yy) != want)
                    bad = 1;
            }
        CHECK(!bad);
    }

    /* a cell that would start before or end past the plane is not drawn at all */
    for (p = 0; p < 4; p++)
        memcpy(before[p], mem[p], SPLANE);
    lrv_rect_clear(&dirty);
    CHECK(lrv_glyph_blit(&s, 0, SH - 15, glyph, 2, 16, 15, 0, 15, &dirty) == 0);
    CHECK(lrv_glyph_blit(&s, 0, -1, glyph, 2, 16, 15, 0, 15, &dirty) == 0);
    CHECK(lrv_glyph_blit(&s, SSTR - 1, SH - 16, glyph, 2, 16, 15, 0, 15, &dirty) == 0);
    CHECK(lrv_glyph_blit(&s, 0, 0, glyph, 0, 16, 15, 0, 15, &dirty) == 0);
    for (p = 0; p < 4; p++)
        CHECK(!memcmp(before[p], mem[p], SPLANE));
    CHECK(lrv_rect_empty(&dirty));
    /* the last cell that fits, and without a dirty rectangle */
    CHECK(lrv_glyph_blit(&s, SSTR - 2, SH - 16, glyph, 2, 16, 15, 0, 15, 0) == 1);
    CHECK(mem[0][SPLANE - 1] == glyph[31] && mem[3][SPLANE - 2] == glyph[30]);
}

static void test_bold(void)
{
    int v;
    CHECK(lrv_glyph_bold8(0x00) == 0x00 && lrv_glyph_bold16(0x0000) == 0x0000);
    CHECK(lrv_glyph_bold8(0x08) == 0x18);               /* ink one pixel to the left */
    CHECK(lrv_glyph_bold8(0x18) == 0x38);
    CHECK(lrv_glyph_bold8(0x05) == 0x0B);               /* . # . #  ->  # . # # : the gap stays */
    CHECK(lrv_glyph_bold16(0x0008) == 0x0018 && lrv_glyph_bold16(0x0500) == 0x0B00);
    CHECK(lrv_glyph_bold16(0x8001) == 0x8003);          /* nothing leaves the cell */
    CHECK(lrv_glyph_bold8(0x80) == 0x80);
    CHECK(lrv_glyph_bold8_right(0x10) == 0x18 && lrv_glyph_bold8_right(0x01) == 0x01);
    /* the plain form merges strokes one pixel apart and lets the leftmost pixel go */
    CHECK(lrv_glyph_bold8_plain(0x05) == 0x0F && lrv_glyph_bold8_plain(0x81) == 0x83);
    CHECK(lrv_glyph_bold16_plain(0x0500) == 0x0F00 && lrv_glyph_bold16_plain(0x8080) == 0x8180);
    for (v = 0; v < 0x10000; v++)
        CHECK(lrv_glyph_bold16_plain((uint16_t)v) == (uint16_t)(v | v << 1) &&
              lrv_glyph_bold8_plain((uint8_t)v) == (uint8_t)(v | v << 1));
    /* the two routines agree wherever the 8-pixel row does not touch its left edge */
    for (v = 0; v < 0x80; v++)
        CHECK(lrv_glyph_bold8((uint8_t)v) == (lrv_glyph_bold16((uint16_t)v) & 0xFF));
    /* and the mirrored form is the mirror image */
    for (v = 0; v < 0x100; v++) {
        unsigned m = 0, r = lrv_glyph_bold8_right((uint8_t)v), b, i;
        for (i = 0; i < 8; i++)
            if ((unsigned)v & (1u << i))
                m |= 0x80u >> i;
        b = lrv_glyph_bold8((uint8_t)m);
        for (i = 0; i < 8; i++)
            CHECK(((r >> i) & 1) == ((b >> (7 - i)) & 1));
    }
}

/* ---- reflow ------------------------------------------------------------------------------ */

/* ASCII one column, everything else two, controls none */
static int cols_fn(void *ctx, uint32_t cp)
{
    (*(int *)ctx)++;
    return cp < 0x20 ? 0 : cp < 0x80 ? 1 : 2;
}

static int g_calls;
static struct lrv_reflow g_lay;

static void layout(const char *text, int width, int height, int last)
{
    lrv_reflow_layout((const uint8_t *)text, width, height, last, cols_fn, &g_calls, &g_lay);
}

static int line_is(int k, unsigned start, unsigned end, int cols, int page)
{
    const struct lrv_reflow_line *l = &g_lay.line[k];
    return k < g_lay.nlines && l->start == start && l->end == end && l->cols == cols &&
           l->page == page;
}

static void test_reflow(void)
{
    static char many[700];
    uint32_t cp;
    int c, i;

    layout("", 10, 3, 10);
    CHECK(g_lay.nlines == 0 && g_lay.npages == 0 && g_lay.dropped == 0);

    /* words fill a line; the next word starts the next line */
    layout("aaa bbb ccc", 7, 3, 7);
    CHECK(g_lay.nlines == 2 && g_lay.npages == 1 && g_calls > 0);
    CHECK(line_is(0, 0, 7, 7, 0) && line_is(1, 8, 11, 3, 0));
    layout("aaa bbb ccc", 6, 3, 6);
    CHECK(g_lay.nlines == 3 && line_is(1, 4, 7, 3, 0) && line_is(2, 8, 11, 3, 0));

    /* spaces: none at the start or the end of a line, a run inside a line is kept */
    layout("  a  b  ", 10, 3, 10);
    CHECK(g_lay.nlines == 1 && line_is(0, 2, 6, 4, 0));

    /* pages of `height` lines; the last line of a page is `last` wide; a word wider than its
     * line is broken where the line is full */
    layout("aaa bbb cccccc dd", 7, 2, 5);
    CHECK(g_lay.nlines == 3 && g_lay.npages == 2);
    CHECK(line_is(0, 0, 7, 7, 0) && line_is(1, 8, 13, 5, 0) && line_is(2, 13, 17, 4, 1));
    layout("abcdefghij", 4, 5, 4);
    CHECK(g_lay.nlines == 3 && line_is(0, 0, 4, 4, 0) && line_is(1, 4, 8, 4, 0) &&
          line_is(2, 8, 10, 2, 0));

    /* a line break ends the line, or makes an empty one; a page break ends the page */
    layout("a\n\nb", 10, 5, 10);
    CHECK(g_lay.nlines == 3 && line_is(0, 0, 1, 1, 0) && line_is(1, 2, 2, 0, 0) &&
          line_is(2, 3, 4, 1, 0));
    layout("a\fb\f", 10, 5, 10);
    CHECK(g_lay.nlines == 2 && g_lay.npages == 2 && line_is(0, 0, 1, 1, 0) &&
          line_is(1, 2, 3, 1, 1));
    layout("\fa", 10, 5, 10);                           /* nothing on the page yet: no break */
    CHECK(g_lay.nlines == 1 && line_is(0, 1, 2, 1, 0));
    layout("a\nb\nc", 10, 2, 10);                       /* a full page turns by itself */
    CHECK(g_lay.npages == 2 && line_is(2, 4, 5, 1, 1));

    /* characters two columns wide, by the callback */
    layout("\xC4\x85\xC4\x99 \xC4\x85", 5, 3, 5);       /* 2 + 2, then 1 + 2 does not fit */
    CHECK(g_lay.nlines == 2 && line_is(0, 0, 4, 4, 0) && line_is(1, 5, 7, 2, 0));
    layout("\xC4\x85\xC4\x99\xC4\x85", 5, 3, 5);        /* one word: broken after two */
    CHECK(g_lay.nlines == 2 && line_is(0, 0, 4, 4, 0) && line_is(1, 4, 6, 2, 0));
    layout("\xC4\x85", 1, 3, 1);                        /* wider than the window: still placed */
    CHECK(g_lay.nlines == 1 && line_is(0, 0, 2, 2, 0));

    /* more lines than the layout holds are counted, not stored */
    for (i = 0; i < 300; i++) {
        many[2 * i] = 'x';
        many[2 * i + 1] = '\n';
    }
    many[600] = 0;
    layout(many, 10, 4, 10);
    CHECK(g_lay.nlines == LRV_REFLOW_MAX_LINES && g_lay.dropped == 300 - LRV_REFLOW_MAX_LINES);
    CHECK(g_lay.npages == LRV_REFLOW_MAX_LINES / 4);

    /* a single-line item */
    CHECK(lrv_reflow_fit((const uint8_t *)"abcdef", 4, cols_fn, &g_calls, &c) == 4 && c == 4);
    CHECK(lrv_reflow_fit((const uint8_t *)"abc", 4, cols_fn, &g_calls, &c) == 3 && c == 3);
    CHECK(lrv_reflow_fit((const uint8_t *)"ab\ncd", 10, cols_fn, &g_calls, &c) == 5 && c == 5);
    CHECK(lrv_reflow_fit((const uint8_t *)"abc\xC4\x85", 4, cols_fn, &g_calls, &c) == 3 && c == 3);
    CHECK(lrv_reflow_fit((const uint8_t *)"", 4, cols_fn, &g_calls, &c) == 0 && c == 0);

    CHECK(lrv_reflow_char((const uint8_t *)"\xC4\x85", cols_fn, &g_calls, &cp, &c) == 2 &&
          cp == 0x105 && c == 2);
}

int main(void)
{
    test_sjis();
    test_utf8();
    test_font();
    test_blit();
    test_bold();
    test_reflow();
    if (th_fails) {
        fprintf(stderr, "test_text: %d failed\n", th_fails);
        return 1;
    }
    printf("test_text: ok\n");
    return 0;
}
