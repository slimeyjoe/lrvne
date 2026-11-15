/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Game profiles: what makes a generic engine run one particular game.
 *
 * A profile is the pack's "profile" entry (LRV_PT_META): UTF-8 text, one "key=value" per
 * line, blank lines and lines starting with '#' ignored. An engine describes the keys it
 * understands as a table of fields over its own profile structure; the parser fills the
 * structure and reports every line it cannot use, so a misspelled key is found when the pack
 * is first run instead of being silently ignored.
 *
 * The caller sets the defaults in the structure before parsing: a key that is absent leaves
 * its field alone.
 *
 * Keys every engine has are in struct lrv_profile_base, which an engine's profile structure
 * embeds (LRV_PROFILE_BASE_FIELDS adds them to its table). */
#ifndef LRVNE_PROFILE_H
#define LRVNE_PROFILE_H

#include <stddef.h>
#include <stdint.h>
#include "lrvne/core.h"

enum lrv_pfield_type {
    LRV_PF_T_STR,       /* char[cap], NUL terminated; longer values are an error */
    LRV_PF_T_INT,       /* int within [min, max]; decimal, or hexadecimal with 0x */
    LRV_PF_T_BOOL,      /* int: 0/1, no/yes, false/true, off/on */
    LRV_PF_T_INTS       /* int[cap], comma separated, each within [min, max]; the count is
                           stored in the int at count_off */
};

struct lrv_pfield {
    const char *key;
    int type;           /* enum lrv_pfield_type */
    size_t off;         /* of the field in the profile structure */
    size_t cap;         /* STR: bytes; INTS: elements */
    long min, max;      /* INT, INTS */
    size_t count_off;   /* INTS: of the int that receives the number of elements */
};

#define LRV_PF_STR(key, type, member) \
    { key, LRV_PF_T_STR, offsetof(type, member), sizeof(((type *)0)->member), 0, 0, 0 }
#define LRV_PF_INT(key, type, member, min, max) \
    { key, LRV_PF_T_INT, offsetof(type, member), 0, min, max, 0 }
#define LRV_PF_BOOL(key, type, member) \
    { key, LRV_PF_T_BOOL, offsetof(type, member), 0, 0, 1, 0 }
#define LRV_PF_INTS(key, type, member, count_member, min, max) \
    { key, LRV_PF_T_INTS, offsetof(type, member), \
      sizeof(((type *)0)->member) / sizeof(int), min, max, offsetof(type, count_member) }

/* What every game states. */
struct lrv_profile_base {
    char id[24];            /* the game's short name: names its saves' directory */
    char title[96];         /* window title, UTF-8 */
    char lang[8];           /* the default language's tag */
    char modules[160];      /* comma-separated native modules the game enables */
};

/* (the offset in two steps: not every compiler's offsetof takes a member of a member) */
#define LRV_PF_BASE_STR(key, type, member, field) \
    { key, LRV_PF_T_STR, offsetof(type, member) + offsetof(struct lrv_profile_base, field), \
      sizeof(((struct lrv_profile_base *)0)->field), 0, 0, 0 }
#define LRV_PROFILE_BASE_FIELDS(type, member) \
    LRV_PF_BASE_STR("id", type, member, id), \
    LRV_PF_BASE_STR("title", type, member, title), \
    LRV_PF_BASE_STR("lang", type, member, lang), \
    LRV_PF_BASE_STR("modules", type, member, modules)

/* Called for each line that cannot be used: line number from 1, the key (NUL terminated) and
 * what is wrong with it. */
typedef void lrv_profile_err_fn(void *ctx, int line, const char *key, const char *what);

/* Parses `text` into the structure at `dst`. Returns the number of lines reported (0 = the
 * whole profile was understood). */
int lrv_profile_parse(const uint8_t *text, size_t len, const struct lrv_pfield *fields,
                      int nfields, void *dst, lrv_profile_err_fn *err, void *ctx);

struct lrv_pack;

/* Loads and parses the "profile" entry of a pack, logging what is wrong with it. Returns
 * LRV_OK, LRV_ERR_NOTFOUND when the pack has no profile, LRV_ERR_FORMAT when lines were
 * reported (the fields that could be read are set). */
int lrv_profile_load(const struct lrv_pack *pk, const struct lrv_pfield *fields, int nfields,
                     void *dst);

/* 1 when `name` is an element of the comma-separated list (spaces around elements ignored). */
int lrv_list_has(const char *list, const char *name);

#endif
