/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fonts: glyph tables from the pack, the lookups the engines build on them, and drawing a glyph
 * cell into a planar surface.
 *
 * A glyph table ("XFNT", an LRV_PT_FONT pack entry made by the font builder) is, little-endian:
 *   0x00  4   "XFNT"
 *   0x04  u8  (not read here)
 *   0x05  u8  cell width in pixels (a multiple of 8)
 *   0x06  u8  cell height in pixels
 *   0x07  u8  (not read here)
 *   0x08  u16 glyph count
 *   0x0A  u16 (not read here)
 *   0x0C  count x u16 codes, ascending
 *         count x glyphs, cell_h rows of cell_w / 8 bytes, the most significant bit leftmost
 * What a code is depends on the table: a JIS X 0208 code, a JIS X 0201 byte, or a Unicode code
 * point of the Basic Multilingual Plane.
 *
 * By convention a pack names its tables "font16" (JIS X 0208, 16x16) and "font8" (JIS X 0201,
 * 8x16), and for translated text "latin8" (8x16) and "wide16" (16x16), both keyed by code
 * point. A font is an instance the caller owns; nothing here is global.
 *
 * Windows, cursors, printing and line breaking rules of an original are the engine's. */
#ifndef LRVNE_FONT_H
#define LRVNE_FONT_H

#include <stdint.h>
#include "lrvne/core.h"
#include "lrvne/pack.h"
#include "lrvne/surface.h"

#define LRV_FONT_HEADER_SIZE 12
#define LRV_GLYPH_ROWS       16     /* the cell height of the 8x16 / 16x16 lookups below */

struct lrv_font {
    const struct lrv_pack *pack;    /* where data came from; NULL when it is borrowed */
    uint8_t *data;                  /* the whole table; NULL = no font */
    uint32_t size;
    int cell_w, cell_h;             /* pixels */
    int count;
    int glyph_bytes;                /* cell_h * cell_w / 8 */
    const uint8_t *codes;           /* count x u16 LE, ascending */
    const uint8_t *glyphs;
};

/* Describes the table in data[0..size), which stays the caller's. cell_w / cell_h, when not 0,
 * are what the cell must be. Returns LRV_OK or LRV_ERR_FORMAT (the font is then empty). */
int  lrv_font_parse(struct lrv_font *f, const uint8_t *data, uint32_t size, int cell_w,
                    int cell_h);
/* Loads pack entry `name` (LRV_PT_FONT, in the pack's current language or neutral). Returns
 * LRV_OK, LRV_ERR_NOTFOUND, LRV_ERR_IO or LRV_ERR_FORMAT; on failure the font is empty and
 * nothing stays allocated. */
int  lrv_font_load(struct lrv_font *f, const struct lrv_pack *pk, const char *name, int cell_w,
                   int cell_h);
/* Releases a loaded font and empties it; an empty or borrowed one is only emptied. */
void lrv_font_free(struct lrv_font *f);

/* The glyph of a code: glyph_bytes bytes, or NULL when the table (or f) has none. */
const uint8_t *lrv_font_glyph(const struct lrv_font *f, unsigned code);

/* ---- the kanji ROM model ---------------------------------------------------------------- */

/* The glyph of JIS code `jis` as a PC-98 kanji ROM read gives it: 16 rows of `width` bytes,
 * left byte first, into rows; returns the width in bytes. Rows 29h and 2Ah are the half-width
 * characters (29xx = the JIS X 0201 roman byte xx, 2Axx = the katakana byte xx + 80h) from
 * font8; row 2Bh is half-width and blank; everything else is full-width from font16. A code
 * without a glyph is blank. Either font may be NULL or empty. */
int lrv_font_rom_glyph(const struct lrv_font *font16, const struct lrv_font *font8, unsigned jis,
                       uint8_t rows[32]);

/* ---- Unicode lookups for translated text ------------------------------------------------ */

/* Columns of code point cp: 1 when latin8 has it, otherwise 2 (it is then looked for in the
 * language's wide table); control codes (below U+0020) are 0 wide. */
int lrv_font_uni_cols(const struct lrv_font *latin8, uint32_t cp);
/* The same as a callback for lrvne/reflow.h; ctx is the latin8 font. */
int lrv_font_uni_cols_fn(void *ctx, uint32_t cp);
/* Fills 16 rows of 1 byte (width 1, from latin8) or 2 bytes (width 2, from wide16; blank when
 * absent) and returns the width; 0 and nothing for a control code. */
int lrv_font_uni_glyph(const struct lrv_font *latin8, const struct lrv_font *wide16, uint32_t cp,
                       uint8_t rows[32]);

/* ---- character streams ------------------------------------------------------------------ */

struct lrv_text_ch {
    unsigned code;          /* the glyph's key: in the 16-wide table if cols == 2, else 8-wide */
    int len;                /* bytes consumed */
    int cols;               /* 1 or 2 */
};

/* One character of cp932 text at s (s[0] != 0). A lead byte with a trail byte is one 2-column
 * glyph keyed by its JIS X 0208 code; any other byte is one 1-column glyph keyed by the byte.
 * A lead byte right before the NUL counts as a 1-column byte. Returns ch->len. */
int lrv_text_next_sjis(const uint8_t *s, struct lrv_text_ch *ch);
/* One character of UTF-8 text at s (s[0] != 0), for a pair of tables keyed by code point: 1
 * column when it is ASCII or font8 has it, otherwise 2. Malformed bytes and code points
 * outside the BMP (the tables have 16-bit codes) become U+FFFD. Returns ch->len. */
int lrv_text_next_utf8(const uint8_t *s, const struct lrv_font *font8, struct lrv_text_ch *ch);

/* ---- drawing ---------------------------------------------------------------------------- */

#define LRV_GLYPH_CLEAR (-1)     /* bg of lrv_glyph_blit: a transparent cell */

/* A glyph cell into an LRV_FMT_PLANAR4 surface: `rows` lines of `wbytes` columns at column
 * `col`, line y, every pixel of the cell set to colour fg where the glyph has a 1 and to bg
 * elsewhere; with bg LRV_GLYPH_CLEAR the pixels without ink keep what they had (a transparent
 * cell). Only the planes whose bit is set in `planes` are written (15 = all). The
 * cell is addressed as the machines do, by its plane offset y * stride + col: it is drawn
 * whole or, when it would start before or end past the plane, not at all (returns 0). The
 * rectangle written is added to *dirty when dirty is not NULL. Returns 1 when drawn. */
int lrv_glyph_blit(struct lrv_surface *s, int col, int y, const uint8_t *glyph, int wbytes,
                   int rows, int fg, int bg, unsigned planes, struct lrv_rect *dirty);

/* Emboldening of one glyph row, as a PC-98 text service does it at run time: ink is added one
 * pixel to the left of every pixel, keeping a one-pixel gap between two strokes (1-0-1) open. The _right form is the mirror image (ink to the right), for glyphs designed against
 * the left edge of their cell. */
uint8_t  lrv_glyph_bold8(uint8_t row);
uint8_t  lrv_glyph_bold8_right(uint8_t row);
uint16_t lrv_glyph_bold16(uint16_t row);
/* The plain form: every pixel also inks the pixel to its left (row | row << 1), strokes one
 * pixel apart merge; the leftmost pixel's extension leaves the cell. */
uint8_t  lrv_glyph_bold8_plain(uint8_t row);
uint16_t lrv_glyph_bold16_plain(uint16_t row);

#endif
