/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Glyph tables, the lookups on them and glyph cells on planar surfaces (lrvne/font.h). */
#include <string.h>
#include "lrvne/font.h"
#include "lrvne/endian.h"
#include "lrvne/sjis.h"
#include "lrvne/utf8.h"

/* ---- tables ------------------------------------------------------------------------------ */

static void font_empty(struct lrv_font *f)
{
    memset(f, 0, sizeof *f);
}

/* fills the description from f->data / f->size */
static int font_describe(struct lrv_font *f, int cell_w, int cell_h)
{
    if (f->size < LRV_FONT_HEADER_SIZE || memcmp(f->data, "XFNT", 4) != 0)
        return LRV_ERR_FORMAT;
    f->cell_w = f->data[5];
    f->cell_h = f->data[6];
    f->count = lrv_rd16le(f->data + 8);
    f->glyph_bytes = f->cell_h * f->cell_w / 8;
    f->codes = f->data + LRV_FONT_HEADER_SIZE;
    f->glyphs = f->codes + 2 * f->count;
    if (f->cell_w < 8 || f->cell_w % 8 || f->cell_h < 1 ||
        (cell_w && f->cell_w != cell_w) || (cell_h && f->cell_h != cell_h) ||
        LRV_FONT_HEADER_SIZE + (uint32_t)f->count * (2u + (uint32_t)f->glyph_bytes) > f->size)
        return LRV_ERR_FORMAT;
    return LRV_OK;
}

int lrv_font_parse(struct lrv_font *f, const uint8_t *data, uint32_t size, int cell_w,
                   int cell_h)
{
    int rc;
    font_empty(f);
    /* the table is only ever read: the pointer is not const in the structure because a loaded
     * font's is what is released */
    f->data = (uint8_t *)(uintptr_t)data;
    f->size = size;
    rc = font_describe(f, cell_w, cell_h);
    if (rc != LRV_OK)
        font_empty(f);
    return rc;
}

int lrv_font_load(struct lrv_font *f, const struct lrv_pack *pk, const char *name, int cell_w,
                  int cell_h)
{
    const struct lrv_pack_entry *e = lrv_pack_find(pk, name, LRV_PT_FONT);
    int rc;
    font_empty(f);
    if (!e)
        return LRV_ERR_NOTFOUND;
    f->data = lrv_pack_load(pk, e, &f->size);
    if (!f->data) {
        font_empty(f);
        return LRV_ERR_IO;
    }
    f->pack = pk;
    rc = font_describe(f, cell_w, cell_h);
    if (rc != LRV_OK)
        lrv_font_free(f);
    return rc;
}

void lrv_font_free(struct lrv_font *f)
{
    if (f->pack && f->data)
        lrv_pack_free(f->pack, f->data, f->size);
    font_empty(f);
}

const uint8_t *lrv_font_glyph(const struct lrv_font *f, unsigned code)
{
    int lo = 0, hi;
    if (!f || !f->data)
        return 0;
    hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        unsigned c = lrv_rd16le(f->codes + 2 * mid);
        if (c == code)
            return f->glyphs + (size_t)mid * (size_t)f->glyph_bytes;
        if (c < code)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

/* the glyph of a table whose cells are w pixels by LRV_GLYPH_ROWS, else NULL */
static const uint8_t *glyph16(const struct lrv_font *f, unsigned code, int w)
{
    if (!f || f->cell_w != w || f->cell_h != LRV_GLYPH_ROWS)
        return 0;
    return lrv_font_glyph(f, code);
}

/* ---- the kanji ROM model ----------------------------------------------------------------- */

int lrv_font_rom_glyph(const struct lrv_font *font16, const struct lrv_font *font8, unsigned jis,
                       uint8_t rows[32])
{
    unsigned row = (jis >> 8) & 0xFF, cell = jis & 0xFF;
    const uint8_t *g;
    memset(rows, 0, 32);
    if (row == 0x29 || row == 0x2A || row == 0x2B) {
        g = row == 0x2B ? 0 : glyph16(font8, row == 0x29 ? cell : (cell + 0x80) & 0xFF, 8);
        if (g)
            memcpy(rows, g, 16);
        return 1;
    }
    g = glyph16(font16, jis & 0xFFFF, 16);
    if (g)
        memcpy(rows, g, 32);
    return 2;
}

/* ---- Unicode lookups --------------------------------------------------------------------- */

int lrv_font_uni_cols(const struct lrv_font *latin8, uint32_t cp)
{
    if (cp < 0x20)
        return 0;
    return cp <= 0xFFFF && glyph16(latin8, cp, 8) ? 1 : 2;
}

int lrv_font_uni_cols_fn(void *ctx, uint32_t cp)
{
    return lrv_font_uni_cols(ctx, cp);
}

int lrv_font_uni_glyph(const struct lrv_font *latin8, const struct lrv_font *wide16, uint32_t cp,
                       uint8_t rows[32])
{
    const uint8_t *g;
    memset(rows, 0, 32);
    if (cp < 0x20)
        return 0;
    if (cp <= 0xFFFF && (g = glyph16(latin8, cp, 8)) != 0) {
        memcpy(rows, g, 16);
        return 1;
    }
    if (cp <= 0xFFFF && (g = glyph16(wide16, cp, 16)) != 0)
        memcpy(rows, g, 32);
    return 2;
}

/* ---- character streams ------------------------------------------------------------------- */

int lrv_text_next_sjis(const uint8_t *s, struct lrv_text_ch *ch)
{
    if (lrv_sjis_is_lead(s[0]) && s[1]) {
        ch->code = lrv_sjis_to_jis(s[0], s[1]);
        ch->len = 2;
        ch->cols = 2;
    } else {
        ch->code = s[0];
        ch->len = 1;
        ch->cols = 1;
    }
    return ch->len;
}

int lrv_text_next_utf8(const uint8_t *s, const struct lrv_font *font8, struct lrv_text_ch *ch)
{
    uint32_t cp;
    ch->len = lrv_utf8_decode(s, &cp);
    if (cp > 0xFFFF)
        cp = LRV_UTF8_REPLACEMENT;
    ch->code = cp;
    ch->cols = (cp < 0x80 || lrv_font_glyph(font8, cp)) ? 1 : 2;
    return ch->len;
}

/* ---- drawing ----------------------------------------------------------------------------- */

int lrv_glyph_blit(struct lrv_surface *s, int col, int y, const uint8_t *glyph, int wbytes,
                   int rows, int fg, int bg, unsigned planes, struct lrv_rect *dirty)
{
    long off = (long)y * s->stride + col;
    int p, r, k;
    if (s->format != LRV_FMT_PLANAR4 || wbytes < 1 || rows < 1 || off < 0 ||
        off + (long)(rows - 1) * s->stride + wbytes > (long)lrv_surface_plane_bytes(s))
        return 0;
    for (p = 0; p < 4; p++) {
        uint8_t *d = s->plane[p] + off;
        int fgb = (fg >> p) & 1, bgb = (bg >> p) & 1;
        uint8_t cell = bgb ? 0xFF : 0x00;
        if (!(planes & (1u << p)))
            continue;
        for (r = 0; r < rows; r++) {
            const uint8_t *g = glyph + r * wbytes;
            for (k = 0; k < wbytes; k++)
                d[k] = (uint8_t)(fgb ? (cell | g[k]) : (cell & ~g[k]));
            d += s->stride;
        }
    }
    if (dirty) {
        struct lrv_rect r4;
        /* a cell that runs over the right edge continues on the next line: the band */
        if (col < 0 || col + wbytes > s->stride) {
            r4.x0 = 0; r4.x1 = s->stride;
            r4.y0 = (int)(off / s->stride); r4.y1 = r4.y0 + rows + 1;
        } else {
            r4.x0 = col; r4.x1 = col + wbytes;
            r4.y0 = y; r4.y1 = y + rows;
        }
        lrv_rect_clip(&r4, s);
        lrv_rect_union(dirty, &r4);
    }
    return 1;
}

uint8_t lrv_glyph_bold8(uint8_t al)
{
    uint8_t bl = (uint8_t)(al << 1), bh;
    int k;
    al |= bl;
    for (k = 0; k < 6; k++) {
        bh = bl & 7;
        if (bh == 5)
            al &= 0xFD;
        bl >>= 1;
        al = (uint8_t)((al >> 1) | (al << 7));
    }
    return (uint8_t)((al >> 2) | (al << 6));
}

static uint8_t rev8(uint8_t v)
{
    v = (uint8_t)((v & 0xF0) >> 4 | (v & 0x0F) << 4);
    v = (uint8_t)((v & 0xCC) >> 2 | (v & 0x33) << 2);
    return (uint8_t)((v & 0xAA) >> 1 | (v & 0x55) << 1);
}

uint8_t lrv_glyph_bold8_right(uint8_t row)
{
    return rev8(lrv_glyph_bold8(rev8(row)));
}

uint16_t lrv_glyph_bold16(uint16_t row)
{
    unsigned ax = row, dx = (ax << 1) & 0xFFFF, bp = ax >> 1;
    bp &= dx;
    bp = ~bp & 0xFFFF;
    bp = ((bp << 1) | 1) & 0xFFFF;
    return (uint16_t)((ax & bp) | dx);
}
