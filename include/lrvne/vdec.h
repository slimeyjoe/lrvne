/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Voice clip decoders. A decoder streams one clip (an LRV_PT_VOICE entry of the voice pack)
 * in small reads, so no clip is ever held whole in memory. The codec is chosen by the clip's
 * first four bytes:
 *
 *   "pcm8"          signed 8-bit samples, as the sample players of the old machines took them
 *   "ldp4" "ldp6"   4- or 6-bit codes into a table of deltas, with runs of a repeated sample
 *   "opus"          Opus packets (with LRV_HAVE_OPUS: the vendored fixed-point libopus)
 *
 * All produce mono 16-bit samples at the clip's own rate. A struct lrv_vdec is set up once
 * with lrv_vdec_init, then opened for clip after clip; what it allocates (the read-ahead
 * window, the Opus decoder's state) is kept between clips and released by lrv_vdec_close. */
#ifndef LRVNE_VDEC_H
#define LRVNE_VDEC_H

#include <stddef.h>
#include <stdint.h>
#include "lrvne/core.h"
#include "lrvne/pack.h"

/* Read-ahead over one clip: the decoders' small reads are served from a window of the entry,
 * refilled with one large pack read. On a slow file system every pack read costs a seek
 * (milliseconds even for a short one), so the window keeps the reads to about one per second
 * of compressed audio. */
#define LRV_VDEC_READAHEAD 16384

struct lrv_vdec_in {
    const struct lrv_pack *pk;
    const struct lrv_pack_entry *e;
    uint8_t *buf;               /* LRV_VDEC_READAHEAD bytes from the pack's HAL, or NULL */
    uint32_t off, len;          /* entry offset of buf[0], valid bytes */
};
/* Like lrv_pack_read on in->e: bytes [off, off + n) of the clip; returns the count read. */
size_t lrv_vdec_in_read(struct lrv_vdec_in *in, uint32_t off, void *dst, size_t n);

/* pcm8: "pcm8", u32 LE rate, u32 LE sample count, then the samples, signed 8-bit. Each
 * becomes a 16-bit sample (x 256). */
#define LRV_VDEC_PCM8_HEADER 12
struct lrv_vdec_pcm8 {
    struct lrv_vdec_in *in;
    uint32_t pos;               /* next byte of the entry to read */
    uint32_t left;              /* samples of the clip still to produce */
    uint32_t rate;
};

/* ldp: "ldp" + '4' | '6' (the code width), u32 BE rate, u32 BE sample count, then blocks:
 * u16 BE byte count, u16 BE sample count and an MSB-first stream of codes. In every block the
 * accumulator and the bit buffer start at 0. A code c != 0 adds table[c] to the accumulator
 * (16-bit wrap) and outputs it; code 0 is followed by a count n and outputs the accumulator
 * n + 1 times. */
struct lrv_vdec_ldp {
    struct lrv_vdec_in *in;
    uint32_t pos;               /* next byte of the entry to read */
    uint32_t left;              /* samples of the clip still to produce */
    uint32_t blk_bytes;         /* bytes of the current block not yet read into buf */
    uint32_t blk_samples;       /* samples of the current block still to produce */
    int bits;                   /* code width: 4 or 6 */
    const int16_t *table;
    uint32_t bitbuf;
    int nbits;
    uint8_t buf[256];
    int buf_n, buf_i;
    uint16_t acc;
    uint32_t pend;              /* samples of the current code still to output */
    uint32_t rate;
};

#ifdef LRV_HAVE_OPUS
/* opus: "opus", u32 BE rate (8000, 12000, 16000 or 24000), u32 BE sample count (after the
 * pre-skip), u16 BE pre-skip, u16 0, then packets to the end of the entry: u16 BE byte count
 * and one Opus packet (RFC 6716). */
#define LRV_VDEC_OPUS_FRAME 2880        /* the longest Opus frame (120 ms) at 24 kHz */
struct lrv_vdec_opus {
    struct lrv_vdec_in *in;
    uint32_t pos;
    uint32_t left;              /* samples of the clip still to produce */
    uint32_t skip;              /* pre-skip samples still to drop */
    uint32_t rate;
    int pcm_n, pcm_i;           /* decoded samples in pcm[], next one to output */
    const struct lrv_hal *hal;  /* the decoder state's allocator */
    void *od;                   /* the decoder's state: allocated at the first clip, kept */
    uint32_t od_size;
    uint32_t od_rate;           /* the rate od was set up for */
    int16_t pcm[LRV_VDEC_OPUS_FRAME];
    uint8_t pkt[1275];
};
#endif

enum lrv_vdec_kind { LRV_VDEC_NONE = 0, LRV_VDEC_PCM8, LRV_VDEC_LDP, LRV_VDEC_OPUS };

struct lrv_vdec {
    int kind;                   /* enum lrv_vdec_kind of the open clip */
    uint32_t rate;              /* the clip's sample rate */
    struct lrv_vdec_in in;
    struct lrv_vdec_pcm8 pcm8;
    struct lrv_vdec_ldp ldp;
#ifdef LRV_HAVE_OPUS
    struct lrv_vdec_opus opus;
#endif
};

void lrv_vdec_init(struct lrv_vdec *d);
/* Opens a clip. Returns LRV_OK, or LRV_ERR_FORMAT when the entry is not a clip of a codec
 * this build decodes. */
int  lrv_vdec_open(struct lrv_vdec *d, const struct lrv_pack *pk, const struct lrv_pack_entry *e);
/* Decodes up to n samples. Returns the number produced; 0 at the end of the clip. */
int  lrv_vdec_read(struct lrv_vdec *d, int16_t *out, int n);
/* Releases what is kept between clips. */
void lrv_vdec_close(struct lrv_vdec *d);
/* 1 when this build decodes `codec` ("pcm8", "ldp4", "ldp6", "opus"). */
int  lrv_vdec_supports(const char *codec);

/* The codecs: each opens the clip of `in` (LRV_OK or LRV_ERR_FORMAT) and reads like
 * lrv_vdec_read. */
int  lrv_vdec_pcm8_open(struct lrv_vdec_pcm8 *d, struct lrv_vdec_in *in);
int  lrv_vdec_pcm8_read(struct lrv_vdec_pcm8 *d, int16_t *out, int n);
int  lrv_vdec_ldp_open(struct lrv_vdec_ldp *d, struct lrv_vdec_in *in);
int  lrv_vdec_ldp_read(struct lrv_vdec_ldp *d, int16_t *out, int n);
#ifdef LRV_HAVE_OPUS
int  lrv_vdec_opus_open(struct lrv_vdec_opus *d, struct lrv_vdec_in *in);
int  lrv_vdec_opus_read(struct lrv_vdec_opus *d, int16_t *out, int n);
void lrv_vdec_opus_close(struct lrv_vdec_opus *d);
#endif

#endif
