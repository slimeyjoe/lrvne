/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic presentation test, shared by the backends.
 *
 * lrv_ptest_run drives a fixed sequence of presents through the paths a backend's
 * video_present has: the first upload, full and dirty uploads (odd byte columns, the screen
 * edges), palette-only changes, the display off and on, page flips (the first one, flips of a
 * page drawn while hidden, several in a row), scroll changes, dirty redraws while scrolled with
 * an unchanged scroll line (also right after a flip), colour 0 non-black and an all-white
 * palette (a border around the picture must stay black), and a pseudo-random soak. After
 * selected presents it asks for a snapshot of what must now be on the screen.
 *
 * The frames follow the contract of lrvne/hal.h: a dirty rectangle covers every change of the
 * displayed page since its previous present, in page coordinates; a page drawn while hidden is
 * uploaded because of page_switched; a scroll change is not reported as dirty.
 *
 * The sequence is laid out on a 640x400 picture and scaled to the video mode. Everything is
 * deterministic and the same C everywhere, so the hash of each snapshot (lrv_frame_hash32 of
 * lrvne/gfx.h) proves that
 * the sequence ran identically on another machine, and a backend with a real display can be
 * checked against the snapshots the headless backend prints. */
#ifndef LRVNE_BACKENDS_PTEST_H
#define LRVNE_BACKENDS_PTEST_H

#include <stdint.h>
#include "lrvne/app.h"
#include "lrvne/hal.h"

struct lrv_ptest_io {
    void (*present)(const struct lrv_frame *frame);
    /* the screen must now show `frame` (black when its display is off); hash is its
     * lrv_frame_hash32 */
    void (*snap)(int index, const char *name, const struct lrv_frame *frame, uint32_t hash);
};

/* Returns the number of snapshots taken, LRV_ERR_RANGE for a video mode the test does not
 * support (only LRV_FMT_PLANAR4, at least 64x64 and 16 colours), LRV_ERR_NOMEM. */
int lrv_ptest_run(const struct lrv_video_mode *mode, const struct lrv_ptest_io *io);

/* The presentation test without an engine: the video configuration a backend opens for it, in
 * the mode of machine `machine` (lrvne/machine.h) with the display settings of the command
 * line. The engine and its pack are not needed. Returns LRV_ERR_NOTFOUND for an unknown name. */
int lrv_ptest_video_cfg(struct lrv_video_cfg *vc, const char *machine,
                        const struct lrv_run_cfg *cfg, const char *title);

#endif
