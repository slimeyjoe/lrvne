/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Frames and sound taken out of a running engine, for tests: a copy of a presented frame, its
 * hashes, its snapshot line and picture file, and a WAV writer.
 *
 * A frame describes itself (lrvne/hal.h), so nothing here needs the video mode; only
 * LRV_FMT_PLANAR4 pages are understood.
 *
 * A snapshot line is one JSON object on a line of its own:
 *   {"ev":"snap","name":NAME,"t":TICK,"hash":"<8 hex>","size":[W,H],"pal":"<hex>","gon":0|1,
 *    "scroll":N,"data":"<base64>"}
 * pal is the palette as presented (colours x R,G,B), data the displayed picture (the scroll
 * line applied) as colour indices, two pixels per byte, the left one in the high nibble. hash
 * is lrv_frame_hash32 of the frame (lrvne/gfx.h): the same on every backend, so a snapshot
 * taken on one machine can be compared with another's by this number alone. */
#ifndef LRVNE_BACKENDS_SHOT_H
#define LRVNE_BACKENDS_SHOT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "lrvne/app.h"
#include "lrvne/hal.h"

/* A frame kept after its present returned. */
struct lrv_shot {
    struct lrv_surface page;        /* the copy */
    struct lrv_frame frame;         /* the copy as a frame; frame.page is &page */
    uint8_t *mem;                   /* the planes, then the palette */
    size_t size;
    int valid;                      /* a frame was captured */
};

/* Copies a frame. Returns 0, LRV_ERR_RANGE for a format that is not supported, LRV_ERR_NOMEM. */
int lrv_shot_capture(struct lrv_shot *s, const struct lrv_frame *f);
void lrv_shot_free(struct lrv_shot *s);

/* 64-bit FNV-1a over everything a present is given (planes, palette, display-on, scroll line):
 * equal hashes mean the frame did not change. */
uint64_t lrv_shot_hash64(const struct lrv_frame *f);

/* Bytes of the nibble-packed picture, and the picture itself into out. */
size_t lrv_shot_indices_size(const struct lrv_frame *f);
void lrv_shot_indices(const struct lrv_frame *f, uint8_t *out);

/* The members "pal", "gon", "scroll" and, when with_data, "data" of a JSON object. */
void lrv_shot_json_body(FILE *out, const struct lrv_frame *f, int with_data);
/* A whole snapshot line, flushed. */
void lrv_shot_emit_snap(FILE *out, const struct lrv_frame *f, const char *name, uint32_t tick);

/* The displayed picture as a binary PPM, the palette applied. Returns 0 or LRV_ERR_IO. */
int lrv_shot_write_ppm(const char *path, const struct lrv_frame *f);

/* ---- WAV: mono, 16 bits ------------------------------------------------------------------ */

struct lrv_wav {
    FILE *f;
    uint32_t frames;
};

int  lrv_wav_open(struct lrv_wav *w, const char *path);         /* 0 or LRV_ERR_IO */
void lrv_wav_write(struct lrv_wav *w, const int16_t *pcm, int frames);
/* Writes the header for what was written so far and flushes: the file is whole up to here even
 * if the process is killed. */
void lrv_wav_sync(struct lrv_wav *w, uint32_t rate);
void lrv_wav_close(struct lrv_wav *w, uint32_t rate);

/* Plays song `song` for `seconds` through app->render_music into the WAV file `path`. Returns
 * the process exit code. */
int lrv_wav_render(const struct lrv_app *app, const struct lrv_hal *hal,
                   const struct lrv_run_cfg *cfg, const char *path, const char *song,
                   double seconds);

#endif
