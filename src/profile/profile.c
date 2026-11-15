/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Game profile parser (lrvne/profile.h). */
#include <string.h>
#include "lrvne/profile.h"
#include "lrvne/hal.h"
#include "lrvne/pack.h"

#define KEY_MAX 48

/* a number at v[0..n): decimal, or hexadecimal with 0x; an optional sign. 0 = not a number */
static int parse_long(const uint8_t *v, size_t n, long *out)
{
    long x = 0;
    int neg = 0, base = 10;
    size_t i = 0;
    if (i < n && (v[i] == '-' || v[i] == '+'))
        neg = v[i++] == '-';
    if (i + 1 < n && v[i] == '0' && (v[i + 1] | 0x20) == 'x') {
        base = 16;
        i += 2;
    }
    if (i >= n)
        return 0;
    for (; i < n; i++) {
        int d = v[i] >= '0' && v[i] <= '9' ? v[i] - '0' :
                (v[i] | 0x20) >= 'a' && (v[i] | 0x20) <= 'f' ? (v[i] | 0x20) - 'a' + 10 : 99;
        if (d >= base || x > 0x7FFFFFF)
            return 0;
        x = x * base + d;
    }
    *out = neg ? -x : x;
    return 1;
}

static int word_is(const uint8_t *v, size_t n, const char *w)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!w[i] || (v[i] | 0x20) != w[i])
            return 0;
    return w[n] == 0;
}

static void trim(const uint8_t **v, size_t *n)
{
    while (*n && ((*v)[0] == ' ' || (*v)[0] == '\t')) {
        (*v)++;
        (*n)--;
    }
    while (*n && ((*v)[*n - 1] == ' ' || (*v)[*n - 1] == '\t' || (*v)[*n - 1] == '\r'))
        (*n)--;
}

/* stores one value; returns NULL or what is wrong with it */
static const char *store(const struct lrv_pfield *f, void *dst, const uint8_t *v, size_t n)
{
    char *base = (char *)dst;
    long x;

    switch (f->type) {
    case LRV_PF_T_STR:
        if (n >= f->cap)
            return "value too long";
        memcpy(base + f->off, v, n);
        base[f->off + n] = 0;
        return 0;
    case LRV_PF_T_INT:
        if (!parse_long(v, n, &x))
            return "not a number";
        if (x < f->min || x > f->max)
            return "number out of range";
        *(int *)(void *)(base + f->off) = (int)x;
        return 0;
    case LRV_PF_T_BOOL:
        if (word_is(v, n, "1") || word_is(v, n, "yes") || word_is(v, n, "true") || word_is(v, n, "on"))
            x = 1;
        else if (word_is(v, n, "0") || word_is(v, n, "no") || word_is(v, n, "false") || word_is(v, n, "off"))
            x = 0;
        else
            return "not yes or no";
        *(int *)(void *)(base + f->off) = (int)x;
        return 0;
    case LRV_PF_T_INTS: {
        int *out = (int *)(void *)(base + f->off);
        size_t count = 0, i = 0;
        while (n) {                             /* an empty value is an empty list */
            const uint8_t *e = v + i;
            size_t en = 0;
            while (i + en < n && v[i + en] != ',')
                en++;
            i += en + 1;
            trim(&e, &en);
            if (count >= f->cap)
                return "too many numbers";
            if (!parse_long(e, en, &x))
                return "not a list of numbers";
            if (x < f->min || x > f->max)
                return "number out of range";
            out[count++] = (int)x;
            if (i > n)
                break;
        }
        *(int *)(void *)(base + f->count_off) = (int)count;
        return 0;
    }
    default:
        return "unknown field type";
    }
}

int lrv_profile_parse(const uint8_t *text, size_t len, const struct lrv_pfield *fields,
                      int nfields, void *dst, lrv_profile_err_fn *err, void *ctx)
{
    size_t i = 0;
    int line = 0, bad = 0;

    while (i < len) {
        const uint8_t *s = text + i, *k, *v;
        size_t n = 0, kn, vn;
        char key[KEY_MAX];
        const char *what = 0;
        int f, has_eq;

        while (i + n < len && s[n] != '\n')
            n++;
        i += n + 1;
        line++;
        k = s;
        kn = 0;
        while (kn < n && s[kn] != '=')
            kn++;
        v = s + kn + 1;
        has_eq = kn < n;
        vn = has_eq ? n - kn - 1 : 0;
        trim(&k, &kn);
        if (kn == 0 && vn == 0)
            continue;                           /* blank */
        if (k[0] == '#')
            continue;
        memcpy(key, k, kn < KEY_MAX - 1 ? kn : KEY_MAX - 1);
        key[kn < KEY_MAX - 1 ? kn : KEY_MAX - 1] = 0;
        if (!has_eq) {
            what = "no '='";
        } else {
            trim(&v, &vn);
            for (f = 0; f < nfields; f++)
                if (strlen(fields[f].key) == kn && !memcmp(fields[f].key, k, kn))
                    break;
            what = f < nfields ? store(&fields[f], dst, v, vn) : "unknown key";
        }
        if (what) {
            bad++;
            if (err)
                err(ctx, line, key, what);
        }
    }
    return bad;
}

static void log_err(void *ctx, int line, const char *key, const char *what)
{
    lrv_log((const struct lrv_hal *)ctx, LRV_LOG_ERROR, "profile: line %d, '%s': %s", line, key,
            what);
}

int lrv_profile_load(const struct lrv_pack *pk, const struct lrv_pfield *fields, int nfields,
                     void *dst)
{
    const struct lrv_pack_entry *e = lrv_pack_find(pk, "profile", LRV_PT_META);
    uint8_t *text;
    uint32_t size;
    int bad;

    if (!e)
        return LRV_ERR_NOTFOUND;
    text = lrv_pack_load(pk, e, &size);
    if (!text)
        return LRV_ERR_IO;
    bad = lrv_profile_parse(text, size, fields, nfields, dst, log_err, (void *)pk->hal);
    lrv_pack_free(pk, text, size);
    return bad ? LRV_ERR_FORMAT : LRV_OK;
}

int lrv_list_has(const char *list, const char *name)
{
    size_t nn = strlen(name);
    while (*list) {
        const char *e;
        size_t n = 0;
        while (*list == ' ' || *list == ',')
            list++;
        e = list;
        while (list[n] && list[n] != ',')
            n++;
        list += n;
        while (n && e[n - 1] == ' ')
            n--;
        if (n == nn && !memcmp(e, name, n))
            return 1;
    }
    return 0;
}
