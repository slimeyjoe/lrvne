/* SPDX-License-Identifier: GPL-3.0-or-later */
/* XPAK reader (lrvne/pack.h). */
#include <string.h>
#include "lrvne/pack.h"
#include "lrvne/endian.h"
#include "lrvne/hal.h"

static int read_header(struct lrv_pack *pk, uint32_t base, uint32_t avail)
{
    uint8_t hdr[LRV_PACK_HEADER_SIZE];
    if (avail < LRV_PACK_HEADER_SIZE)
        return LRV_ERR_FORMAT;
    if (pk->hal->pack_read(pk->src, base, hdr, sizeof hdr) != sizeof hdr)
        return LRV_ERR_IO;
    if (memcmp(hdr, "XPAK", 4) != 0 || lrv_rd16le(hdr + 4) != LRV_PACK_VERSION)
        return LRV_ERR_FORMAT;
    pk->count = lrv_rd16le(hdr + 6);
    pk->total = lrv_rd32le(hdr + 8);
    pk->base = base;
    if (pk->total > avail ||
        (uint32_t)pk->count * LRV_PACK_ENTRY_SIZE > pk->total - LRV_PACK_HEADER_SIZE)
        return LRV_ERR_FORMAT;
    return LRV_OK;
}

/* A bundled pack ends with a trailer that says how long it is. Sets *base; LRV_ERR_FORMAT
 * when there is a trailer and it is damaged. */
static int find_base(struct lrv_pack *pk, uint32_t size, uint32_t *base)
{
    uint8_t tr[LRV_PACK_TRAILER_SIZE];
    uint32_t psize;
    *base = 0;
    if (size < sizeof tr ||
        pk->hal->pack_read(pk->src, size - (uint32_t)sizeof tr, tr, sizeof tr) != sizeof tr ||
        memcmp(tr, "XPKT", 4) != 0 || memcmp(tr + 12, "XPKT", 4) != 0)
        return LRV_OK;
    psize = lrv_rd32le(tr + 4);
    if ((lrv_rd32le(tr + 8) ^ psize) != 0xFFFFFFFFu || psize > size - sizeof tr)
        return LRV_ERR_FORMAT;
    *base = size - (uint32_t)sizeof tr - psize;
    return LRV_OK;
}

static int read_entries(struct lrv_pack *pk)
{
    const struct lrv_hal *hal = pk->hal;
    size_t tbl = (size_t)pk->count * LRV_PACK_ENTRY_SIZE;
    uint8_t *raw = lrv_alloc(hal, tbl);
    uint16_t i;

    pk->entries = lrv_alloc(hal, sizeof *pk->entries * pk->count);
    if (!raw || !pk->entries) {
        lrv_free(hal, raw, tbl);
        return LRV_ERR_NOMEM;
    }
    if (hal->pack_read(pk->src, pk->base + LRV_PACK_HEADER_SIZE, raw, tbl) != tbl) {
        lrv_free(hal, raw, tbl);
        return LRV_ERR_IO;
    }
    for (i = 0; i < pk->count; i++) {
        const uint8_t *e = raw + (size_t)i * LRV_PACK_ENTRY_SIZE;
        struct lrv_pack_entry *o = &pk->entries[i];
        memcpy(o->name, e, LRV_PACK_NAME_LEN);
        o->name[LRV_PACK_NAME_LEN] = 0;
        o->type = e[20];
        o->lang = e[21];
        o->offset = lrv_rd32le(e + 24);
        o->size = lrv_rd32le(e + 28);
        if (o->offset > pk->total || o->size > pk->total - o->offset) {
            lrv_free(hal, raw, tbl);
            return LRV_ERR_FORMAT;
        }
    }
    lrv_free(hal, raw, tbl);
    return LRV_OK;
}

/* the "langs" entry: lines "<id>=<tag>" */
static void read_langs(struct lrv_pack *pk)
{
    const struct lrv_pack_entry *e = lrv_pack_find(pk, "langs", LRV_PT_META);
    uint8_t *text;
    uint32_t size, i = 0;

    if (!e || !(text = lrv_pack_load(pk, e, &size)))
        return;
    while (i < size) {
        unsigned id = 0;
        size_t n = 0;
        while (i < size && text[i] >= '0' && text[i] <= '9')
            id = id * 10 + (unsigned)(text[i++] - '0');
        if (i < size && text[i] == '=' && id >= 1 && id <= LRV_PACK_LANGS_MAX) {
            for (i++; i < size && text[i] != '\n' && text[i] != '\r'; i++)
                if (n < LRV_PACK_TAG_LEN - 1)
                    pk->tags[id][n++] = (char)text[i];
            pk->tags[id][n] = 0;
        }
        while (i < size && text[i] != '\n')
            i++;
        i++;
    }
    lrv_pack_free(pk, text, size);
}

int lrv_pack_open(struct lrv_pack *pk, const struct lrv_hal *hal, int which)
{
    uint32_t size = 0, base;
    int rc;

    memset(pk, 0, sizeof *pk);
    pk->hal = hal;
    pk->src = hal->pack_open(which, &size);
    if (!pk->src) {
        if (which == LRV_PACK_MAIN)
            lrv_log(hal, LRV_LOG_ERROR, "pack: no asset pack found");
        return LRV_ERR_NOTFOUND;
    }
    rc = find_base(pk, size, &base);
    if (rc == LRV_OK)
        rc = read_header(pk, base, size - base);
    if (rc == LRV_OK)
        rc = read_entries(pk);
    if (rc != LRV_OK) {
        lrv_log(hal, LRV_LOG_ERROR, "pack: %s", lrv_strerror(rc));
        lrv_pack_close(pk);
        return rc;
    }
    read_langs(pk);
    return LRV_OK;
}

void lrv_pack_close(struct lrv_pack *pk)
{
    if (!pk->hal)
        return;
    lrv_free(pk->hal, pk->entries, sizeof *pk->entries * pk->count);
    pk->entries = 0;
    pk->count = 0;
    if (pk->src)
        pk->hal->pack_close(pk->src);
    pk->src = 0;
}

int lrv_pack_lang_id(const struct lrv_pack *pk, const char *tag)
{
    int id;
    for (id = 1; id <= LRV_PACK_LANGS_MAX; id++)
        if (pk->tags[id][0] && lrv_name_eq(pk->tags[id], tag))
            return id;
    return 0;
}

const char *lrv_pack_lang_tag(const struct lrv_pack *pk, int id)
{
    return id >= 1 && id <= LRV_PACK_LANGS_MAX && pk->tags[id][0] ? pk->tags[id] : 0;
}

void lrv_pack_set_lang(struct lrv_pack *pk, int id)
{
    pk->lang = (uint8_t)(id >= 0 && id <= LRV_PACK_LANGS_MAX ? id : LRV_LANG_ANY);
}

static const struct lrv_pack_entry *find_in(const struct lrv_pack *pk, const char *name,
                                            int type, int lang)
{
    uint16_t i;
    /* Linear: a few hundred entries, looked up at scene changes only. */
    for (i = 0; i < pk->count; i++) {
        const struct lrv_pack_entry *e = &pk->entries[i];
        if (e->lang == lang && (type == LRV_PT_ANY || e->type == type) &&
            lrv_name_eq(e->name, name))
            return e;
    }
    return 0;
}

const struct lrv_pack_entry *lrv_pack_find(const struct lrv_pack *pk, const char *name, int type)
{
    const struct lrv_pack_entry *e = 0;
    if (pk->lang != LRV_LANG_ANY)
        e = find_in(pk, name, type, pk->lang);
    if (!e)
        e = find_in(pk, name, type, LRV_LANG_ANY);
    return e;
}

uint8_t *lrv_pack_load(const struct lrv_pack *pk, const struct lrv_pack_entry *e, uint32_t *size)
{
    uint8_t *buf = lrv_alloc(pk->hal, (size_t)e->size + 1);
    if (!buf)
        return 0;
    if (pk->hal->pack_read(pk->src, pk->base + e->offset, buf, e->size) != e->size) {
        lrv_free(pk->hal, buf, (size_t)e->size + 1);
        return 0;
    }
    *size = e->size;
    return buf;
}

void lrv_pack_free(const struct lrv_pack *pk, uint8_t *data, uint32_t size)
{
    lrv_free(pk->hal, data, (size_t)size + 1);
}

size_t lrv_pack_read(const struct lrv_pack *pk, const struct lrv_pack_entry *e, uint32_t off,
                     void *dst, size_t n)
{
    if (off >= e->size)
        return 0;
    if (n > e->size - off)
        n = e->size - off;
    return pk->hal->pack_read(pk->src, pk->base + e->offset + off, dst, n);
}
