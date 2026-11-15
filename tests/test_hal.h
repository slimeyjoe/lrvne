/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A HAL for unit tests: packs in memory, malloc with leak accounting, messages collected, and a
 * scripted input. Nothing here touches a file, so the tests need no data. */
#ifndef LRVNE_TESTS_TEST_HAL_H
#define LRVNE_TESTS_TEST_HAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lrvne/endian.h"
#include "lrvne/hal.h"
#include "lrvne/pack.h"

static const uint8_t *th_pack[LRV_PACK_COUNT];
static uint32_t th_pack_size[LRV_PACK_COUNT];
static long th_live;                    /* bytes allocated and not freed */
static int th_errors;                   /* LRV_LOG_ERROR lines logged */
static char th_last_log[512];
static uint32_t th_frames;              /* frame_wait calls */
static struct lrv_input th_next_input;  /* what the next poll returns (events and keys once) */
static int th_fails;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            th_fails++; \
        } \
    } while (0)

static void *th_pack_open(int which, uint32_t *size)
{
    if (which < 0 || which >= LRV_PACK_COUNT || !th_pack[which])
        return 0;
    *size = th_pack_size[which];
    return (void *)&th_pack[which];
}

static size_t th_pack_read(void *h, uint32_t off, void *dst, size_t n)
{
    int which = (int)((const uint8_t **)h - th_pack);
    if (off > th_pack_size[which])
        return 0;
    if (n > th_pack_size[which] - off)
        n = th_pack_size[which] - off;
    memcpy(dst, th_pack[which] + off, n);
    return n;
}

static void th_pack_close(void *h) { (void)h; }

static void *th_alloc(size_t n, unsigned flags)
{
    (void)flags;
    th_live += (long)n;
    return malloc(n);
}

static void th_free(void *p, size_t n)
{
    th_live -= (long)n;
    free(p);
}

static void th_log(int level, const char *msg)
{
    if (level >= LRV_LOG_ERROR)
        th_errors++;
    snprintf(th_last_log, sizeof th_last_log, "%s", msg);
}

static void th_fatal(const char *msg)
{
    fprintf(stderr, "fatal: %s\n", msg);
    exit(1);
}

static void th_frame_wait(void) { th_frames++; }

static void th_input_poll(struct lrv_input *in)
{
    *in = th_next_input;
    th_next_input.events = 0;
    th_next_input.nkeys = 0;
}

static void th_input_flush(void)
{
    th_next_input.events &= LRV_EV_STICKY;
    th_next_input.nkeys = 0;
}

static inline void th_hal(struct lrv_hal *hal)
{
    memset(hal, 0, sizeof *hal);
    hal->pack_open = th_pack_open; hal->pack_read = th_pack_read; hal->pack_close = th_pack_close;
    hal->mem_alloc = th_alloc; hal->mem_free = th_free; hal->log = th_log; hal->fatal = th_fatal;
    hal->frame_wait = th_frame_wait; hal->input_poll = th_input_poll;
    hal->input_flush = th_input_flush;
}

/* ---- a pack builder: the format of lrvne/pack.h, written by hand for the tests ---------- */

struct th_builder {
    uint8_t buf[8192];
    uint32_t count, data;               /* entries added, next data offset */
};

static inline void th_pack_begin(struct th_builder *b, uint32_t entries)
{
    memset(b, 0, sizeof *b);
    memcpy(b->buf, "XPAK", 4);
    lrv_wr16le(b->buf + 4, LRV_PACK_VERSION);
    lrv_wr16le(b->buf + 6, (uint16_t)entries);
    b->data = LRV_PACK_HEADER_SIZE + entries * LRV_PACK_ENTRY_SIZE;
}

static inline void th_pack_add(struct th_builder *b, const char *name, int type, int lang,
                               const void *data, uint32_t size)
{
    uint8_t *e = b->buf + LRV_PACK_HEADER_SIZE + b->count++ * LRV_PACK_ENTRY_SIZE;
    strncpy((char *)e, name, LRV_PACK_NAME_LEN);
    e[20] = (uint8_t)type;
    e[21] = (uint8_t)lang;
    lrv_wr32le(e + 24, b->data);
    lrv_wr32le(e + 28, size);
    memcpy(b->buf + b->data, data, size);
    b->data = (b->data + size + 3) & ~3u;
}

/* finishes the pack and makes it pack `which` of the HAL; returns its size */
static inline uint32_t th_pack_end(struct th_builder *b, int which)
{
    lrv_wr32le(b->buf + 8, b->data);
    th_pack[which] = b->buf;
    th_pack_size[which] = b->data;
    return b->data;
}

#endif
