/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Asset pack ("XPAK") reader.
 *
 * Everything an engine needs of a game (pictures, songs, scripts, fonts, text of each
 * language, the game profile) is in one pack built by tools/lrvne/xpak.py. The pack is a file
 * of its own or is appended to the executable, where a 16-byte trailer finds it.
 *
 * Layout (little-endian):
 *   0x00  4   "XPAK"
 *   0x04  u16 format version (2)
 *   0x06  u16 entry count
 *   0x08  u32 total pack size (bytes, including this header)
 *   0x0C  4   reserved (0)
 *   0x10  entries, 32 bytes each, sorted by (lang, name):
 *         name[20] (NUL padded; a game's file keeps its original name, a-z upper-cased;
 *                   entries of the layer are lower case: "profile", "langs", "font16"),
 *         u8 type, u8 lang, u16 reserved, u32 offset, u32 size
 *   data, each entry 4-byte aligned, offsets relative to the pack start.
 *
 * Trailer after a pack bundled into an executable:
 *   "XPKT" + u32 pack size + u32 ~pack size + "XPKT"  (16 bytes)
 *
 * Types below LRV_PT_ENGINE mean the same in every pack; an engine numbers its own formats
 * from LRV_PT_ENGINE.
 *
 * Languages: an entry is either neutral (lang 0) or belongs to one language id. The pack's
 * "langs" entry (LRV_PT_META, lines "<id>=<tag>", e.g. "1=ja") names the ids, so no language
 * is compiled into the reader. A lookup tries the current language, then the neutral set.
 *
 * Ranges are read through hal->pack_read, so a backend may memory-map, preload or stream from
 * a slow disk. The entry table is read once. */
#ifndef LRVNE_PACK_H
#define LRVNE_PACK_H

#include <stddef.h>
#include <stdint.h>
#include "lrvne/core.h"

#define LRV_PACK_VERSION      2
#define LRV_PACK_HEADER_SIZE  16
#define LRV_PACK_ENTRY_SIZE   32
#define LRV_PACK_NAME_LEN     20
#define LRV_PACK_TRAILER_SIZE 16
#define LRV_PACK_LANGS_MAX    15
#define LRV_PACK_TAG_LEN      8

enum lrv_pack_type {
    LRV_PT_ANY      = -1,   /* lookups only: any type */
    LRV_PT_META     = 1,    /* key=value text: "profile", "langs", per-language settings */
    LRV_PT_FONT     = 2,    /* glyph table (lrvne/font.h) */
    LRV_PT_STRINGS  = 3,    /* language string table */
    LRV_PT_BLOB     = 4,    /* anything else */
    LRV_PT_VOICECUE = 5,    /* the voice cue table */
    LRV_PT_VOICE    = 6,    /* one voice clip, in the voice pack */
    LRV_PT_ENGINE   = 32    /* first type an engine defines */
};

#define LRV_LANG_ANY 0      /* language-neutral */

struct lrv_pack_entry {
    char     name[LRV_PACK_NAME_LEN + 1];   /* NUL terminated */
    uint8_t  type;
    uint8_t  lang;
    uint32_t offset;
    uint32_t size;
};

struct lrv_hal;

struct lrv_pack {
    const struct lrv_hal *hal;
    void *src;                      /* the backend's handle */
    uint32_t base;                  /* offset of the pack in the source (inside an executable) */
    uint32_t total;
    uint16_t count;
    struct lrv_pack_entry *entries;
    uint8_t lang;                   /* current language id, LRV_LANG_ANY = neutral only */
    char tags[LRV_PACK_LANGS_MAX + 1][LRV_PACK_TAG_LEN];    /* tag of each language id */
};

/* Opens pack `which` (enum lrv_pack_id) through the backend. Failures are logged, except a
 * voice pack the backend does not have (LRV_ERR_NOTFOUND). */
int lrv_pack_open(struct lrv_pack *pk, const struct lrv_hal *hal, int which);
void lrv_pack_close(struct lrv_pack *pk);

/* The id of a language tag ("ja", any case) in this pack, 0 when it has no such language; the
 * tag of an id, NULL when the pack does not name it. */
int lrv_pack_lang_id(const struct lrv_pack *pk, const char *tag);
const char *lrv_pack_lang_tag(const struct lrv_pack *pk, int id);
/* Selects the language of the lookups. */
void lrv_pack_set_lang(struct lrv_pack *pk, int id);

/* Finds an entry by name (ASCII case ignored) and type (LRV_PT_ANY: any) in the current
 * language, then in the neutral set. NULL if absent. */
const struct lrv_pack_entry *lrv_pack_find(const struct lrv_pack *pk, const char *name, int type);

/* Reads a whole entry into memory from the HAL (lrv_pack_free releases it); NULL on failure.
 * One more byte than the entry is allocated and it is zero, so text entries can be read as
 * strings. */
uint8_t *lrv_pack_load(const struct lrv_pack *pk, const struct lrv_pack_entry *e, uint32_t *size);
void lrv_pack_free(const struct lrv_pack *pk, uint8_t *data, uint32_t size);

/* Reads part of an entry. Returns the bytes read (less than n at the end of the entry). */
size_t lrv_pack_read(const struct lrv_pack *pk, const struct lrv_pack_entry *e, uint32_t off,
                     void *dst, size_t n);

#endif
