/* SPDX-License-Identifier: GPL-3.0-or-later */
/* How the engine's page gets into a screen buffer (video.c): one of a few uploads, listed per
 * flavour (flavour.h). video.c opens the screen and decides which one draws where:
 *
 *   planar   chipset/upload_planar.c: the CPU copies the planes into a planar chip RAM
 *            bitmap (BMF_STANDARD, the native display, depth 4). The blitter cannot read the
 *            engine's planes in fast RAM, so this is the fast way there.
 *   rtg      upload_rtg.c: BltBitMap from the engine's planes (a planar bitmap in fast RAM)
 *            into a graphics card bitmap; the system converts (Picasso96 does it in software).
 *   chunky   upload_chunky.c: the page converted to 8-bit pixels here (lrvne/p2c.h), then
 *            WriteChunkyPixels. For systems whose planar blits are slow or broken (AROS,
 *            MorphOS), and selectable everywhere with UPLOAD=chunky.
 *
 * A planar upload draws into standard bitmaps only; the flavour's other (first, or the one
 * named by UPLOAD=) draws into everything else.
 *
 * The page is an LRV_FMT_PLANAR4 surface of the engine's video mode (am_mode: AM_W x AM_H
 * pixels, AM_BPR bytes per row of a plane); video.c refuses the modes the uploads cannot
 * show and checks every frame against the mode before an upload sees it. */
#ifndef LRVNE_BACKENDS_AMIGA_UPLOAD_H
#define LRVNE_BACKENDS_AMIGA_UPLOAD_H

#include "os_begin.h"
#include <exec/types.h>
#include <graphics/gfx.h>
#include "os_end.h"
#include "lrvne/hal.h"
#include "lrvne/p2c.h"
#include "lrvne/scale.h"

extern struct lrv_video_mode am_mode;   /* video.c: the mode of the open display */
#define AM_W   (am_mode.w)
#define AM_H   (am_mode.h)
#define AM_BPR (am_mode.w / 8)          /* bytes per row of a page plane */

extern struct lrv_p2c am_p2c;           /* video.c: the planar to chunky table, filled at open */

struct am_upload {
    const char *name;                   /* for UPLOAD= and the log */
    int planar;                         /* draws into BMF_STANDARD bitmaps (depth 4) only */
    /* after the screen is open: its depth and the pen of colour 0 (0, or 16 on a depth-8
     * screen, where pens 16..31 hold the page's colours). 0 = ready, else the screen is
     * reopened at depth 4 (pen 0) and init called again. */
    int  (*init)(int depth, int pen_base);
    void (*fini)(void);
    /* rows [y0,y1) x byte columns [x0,x1) of the page, page rows rotated by `scroll`
     * (display row y shows page row (y + scroll) mod AM_H), into bm at the same place moved
     * by dx pixels (a multiple of 8) and dy rows: a screen larger than the page shows it in
     * its middle */
    void (*copy)(struct BitMap *bm, const struct lrv_surface *page, int x0, int y0, int x1,
                 int y1, int scroll, int dx, int dy);
};

/* A display that shows the page larger (SCALE=, ASPECT; README.md "Window, scale, aspect"):
 * sx pixels across per page pixel, display row r on rows map.start[r] .. map.start[r + 1] - 1.
 * Set by video.c before the init of the two uploads that use it; sx = 0 while the plain path
 * (the page as it is) is in use. */
struct am_geom {
    int sx;
    int dst_w, dst_h;
    struct lrv_rowmap map;
};
extern struct am_geom am_geom;                      /* video.c */

/* video.c, for video_win.c too: a gun of `bits` bits as the 32-bit fraction LoadRGB32 and
 * SetRGB32 take; a frame's palette as 16 entries (the ones it lacks black); whether a frame
 * is a page of am_mode */
ULONG am_gun32(unsigned v, int bits);
void  am_palette_copy(uint8_t pal[48], const struct lrv_frame *f);
int   am_frame_ok(const struct lrv_frame *f);

extern const struct am_upload am_upload_planar;     /* chipset/upload_planar.c */
extern const struct am_upload am_upload_planar_map; /* chipset/: ASPECT on a native screen */
extern const struct am_upload am_upload_scaled;     /* SCALE/ASPECT on a graphics card */
extern const struct am_upload am_upload_rtg;        /* upload_rtg.c */
extern const struct am_upload am_upload_chunky;     /* upload_chunky.c */

/* upload_scaled.c for the window on a public screen: buffers for am_geom and the 16 pens of
 * the page's colours (0 or -1); rows [y0,y1) x byte columns [x0,x1) of the page into rp at
 * dx,dy through am_geom; free the buffers */
struct RastPort;
int  am_scaled_setup(const uint8_t pen[16]);
void am_scaled_draw(struct RastPort *rp, const struct lrv_surface *page, int x0, int y0, int x1,
                    int y1, int scroll, int dx, int dy);
void am_scaled_release(void);

#endif
