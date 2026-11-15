/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Frame buffers as the emulated machine has them, and rectangles over them.
 *
 * A surface is described by data, not by constants, so that machines other than the first one
 * supported (the PC-98: 640x400, four bit-planes) fit without changing the interfaces. Only
 * LRV_FMT_PLANAR4 is implemented by the modules and backends today.
 *
 * LRV_FMT_PLANAR4: four bit-planes, plane k holding bit k of the colour index, `stride` bytes
 * per line, the most significant bit the leftmost pixel. A pixel's byte offset in every plane
 * is y * stride + x / 8, as in the video memory of the machines this models.
 *
 * The lines of a plane follow one another without padding, and the originals address their
 * video memory by offset. A run of bytes that crosses the right edge therefore continues at
 * the left edge of the next line; it is not clipped. The functions that take a plane offset or
 * a column that may run over keep this (lrv_surface_copy_rows, lrv_glyph_blit), and what they
 * report as changed is then the whole band of lines touched (lrv_screen_mark_off). Only the
 * ends of the plane are a limit: nothing is written before its first or past its last byte. */
#ifndef LRVNE_SURFACE_H
#define LRVNE_SURFACE_H

#include <stdint.h>

enum lrv_pixfmt {
    LRV_FMT_NONE = 0,
    LRV_FMT_PLANAR4,        /* 4 bit-planes, 16 colours */
    LRV_FMT_INDEXED8        /* reserved: one byte per pixel, 256 colours (not implemented) */
};

#define LRV_MAX_PLANES 4

struct lrv_surface {
    int format;                     /* enum lrv_pixfmt */
    int w, h;                       /* pixels */
    int stride;                     /* bytes per line of one plane */
    uint8_t *plane[LRV_MAX_PLANES];
};

/* Pixels per horizontal rectangle unit: rectangles address whole bytes of a planar surface. */
static inline int lrv_format_xunit(int format)
{
    return format == LRV_FMT_PLANAR4 ? 8 : 1;
}

/* Number of planes of a format. */
static inline int lrv_format_planes(int format)
{
    return format == LRV_FMT_PLANAR4 ? 4 : format == LRV_FMT_INDEXED8 ? 1 : 0;
}

/* Bytes of one plane. */
static inline uint32_t lrv_surface_plane_bytes(const struct lrv_surface *s)
{
    return (uint32_t)s->stride * (uint32_t)s->h;
}

/* A rectangle of columns [x0, x1) and lines [y0, y1). A column is lrv_format_xunit() pixels
 * wide: a byte of a planar surface. Empty when x1 <= x0 or y1 <= y0. */
struct lrv_rect {
    int x0, y0, x1, y1;
};

static inline void lrv_rect_clear(struct lrv_rect *r)
{
    r->x0 = r->y0 = 0x7fff;
    r->x1 = r->y1 = 0;
}

static inline int lrv_rect_empty(const struct lrv_rect *r)
{
    return r->x1 <= r->x0 || r->y1 <= r->y0;
}

static inline void lrv_rect_union(struct lrv_rect *r, const struct lrv_rect *o)
{
    if (lrv_rect_empty(o))
        return;
    if (o->x0 < r->x0) r->x0 = o->x0;
    if (o->y0 < r->y0) r->y0 = o->y0;
    if (o->x1 > r->x1) r->x1 = o->x1;
    if (o->y1 > r->y1) r->y1 = o->y1;
}

static inline void lrv_rect_add(struct lrv_rect *r, int x0, int y0, int x1, int y1)
{
    struct lrv_rect o;
    o.x0 = x0; o.y0 = y0; o.x1 = x1; o.y1 = y1;
    lrv_rect_union(r, &o);
}

/* Clips r to the columns and lines of s. */
static inline void lrv_rect_clip(struct lrv_rect *r, const struct lrv_surface *s)
{
    int cols = s->w / lrv_format_xunit(s->format);
    if (r->x0 < 0) r->x0 = 0;
    if (r->y0 < 0) r->y0 = 0;
    if (r->x1 > cols) r->x1 = cols;
    if (r->y1 > s->h) r->y1 = s->h;
}

#endif
