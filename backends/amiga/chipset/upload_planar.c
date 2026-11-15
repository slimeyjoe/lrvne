/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The planar upload (upload.h): the CPU copies the engine's planes into a planar chip RAM
 * bitmap (BMF_STANDARD). The blitter cannot read the engine's planes in fast RAM; a CPU copy
 * of the changed rows and byte columns is the fast way (measured on a 68040 with AGA). */
#include "os_begin.h"
#include <exec/types.h>
#include <graphics/gfx.h>
#include "os_end.h"
#include <string.h>
#include "amiga.h"
#include "amsys.h"
#include "upload.h"

static int planar_init(int depth, int pen_base)
{
    (void)depth; (void)pen_base;
    return 0;
}

static void planar_fini(void)
{
}

static void planar_copy(struct BitMap *dst, const struct lrv_surface *page, int x0, int y0, int x1,
                        int y1, int scroll, int dx, int dy)
{
    int bpr = dst->BytesPerRow, sbpr = AM_BPR, h = AM_H;
    int wbytes = x1 - x0;
    int p, y;
    for (p = 0; p < 4; p++) {
        UBYTE *d = dst->Planes[p] + dy * bpr + dx / 8;
        const uint8_t *s = page->plane[p];
        if (scroll == 0 && x0 == 0 && x1 == sbpr && bpr == sbpr) {
            memcpy(d + y0 * sbpr, s + y0 * sbpr, (size_t)(y1 - y0) * (size_t)sbpr);
            continue;
        }
        for (y = y0; y < y1; y++) {
            int sy = y + scroll;
            if (sy >= h)
                sy -= h;
            memcpy(d + y * bpr + x0, s + sy * sbpr + x0, (size_t)wbytes);
        }
    }
    /* WarpOS: the display DMA reads the chip planes the PPC just wrote */
    for (p = 0; p < 4; p++)
        am_xcpu_flush(dst->Planes[p] + (y0 + dy) * bpr, (ULONG)(y1 - y0) * (ULONG)bpr);
}

const struct am_upload am_upload_planar = {
    "planar", 1, planar_init, planar_fini, planar_copy
};
