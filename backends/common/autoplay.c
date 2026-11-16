/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Scripted input (autoplay.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lrvne/core.h"
#include "autoplay.h"

static const struct { const char *name; int key; uint32_t event; } g_aliases[] = {
    { "ok", LRV_K_RETURN, 0 }, { "cancel", LRV_K_ESC, 0 },
    { "quit", 0, LRV_EV_QUIT }, { "emergency", 0, LRV_EV_EMERGENCY },
    { "reset", 0, LRV_EV_RESET }, { "mouse_l", 0, LRV_EV_MOUSE_L }, { "mouse_r", 0, LRV_EV_MOUSE_R }
};

int lrv_input_by_name(const char *name, size_t len, int *key, uint32_t *event)
{
    char buf[16];
    size_t i;

    *key = LRV_K_NONE;
    *event = 0;
    if (len == 0 || len >= sizeof buf)
        return 0;
    memcpy(buf, name, len);
    buf[len] = 0;
    for (i = 0; i < sizeof g_aliases / sizeof g_aliases[0]; i++)
        if (lrv_name_eq(g_aliases[i].name, buf)) {
            *key = g_aliases[i].key;
            *event = g_aliases[i].event;
            return 1;
        }
    *key = lrv_key_by_name(buf);
    return *key != LRV_K_NONE;
}

unsigned lrv_input_key_mod(int key)
{
    switch (key) {
    case LRV_K_SHIFT: return LRV_MOD_SHIFT;
    case LRV_K_CTRL:  return LRV_MOD_CTRL;
    case LRV_K_ALT:   return LRV_MOD_ALT;
    case LRV_K_CAPS:  return LRV_MOD_CAPS;
    case LRV_K_KANA:  return LRV_MOD_KANA;
    default:          return 0;
    }
}

static void press(struct lrv_input *in, int key)
{
    struct lrv_keypress *k;
    if (in->nkeys >= LRV_KEYQ)
        return;
    k = &in->keys[in->nkeys++];
    k->key = (uint8_t)key;
    k->chr = lrv_key_ascii(key, in->mods);
    k->mods = (uint8_t)in->mods;
}

static void deliver_events(struct lrv_input *in, uint32_t events)
{
    in->events |= events;
    if (events & LRV_EV_MOUSE_L)
        in->buttons |= LRV_BTN_L;
    if (events & LRV_EV_MOUSE_R)
        in->buttons |= LRV_BTN_R;
}

/* ---- parsing ------------------------------------------------------------------------------ */

struct errbuf {
    char *buf;
    size_t len;
};

static int fail(const struct errbuf *e, int rc, const char *what, const char *at)
{
    if (e->buf && e->len > 0) {
        int n = 0;
        while (at[n] && at[n] != ' ' && at[n] != '\t' && at[n] != '\n' && at[n] != '\r' && n < 40)
            n++;
        snprintf(e->buf, e->len, "autoplay: %s%s%.*s", what, n ? ": " : "", n, at);
    }
    return rc;
}

static int is_blank(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int at_end(int c)
{
    return c == 0 || is_blank(c);
}

/* grows *arr (elements of sz bytes) so that it holds n + 1; capacities 16, 32, 64, ... */
static int grow(void *arr, int n, size_t sz)
{
    void **slot = arr;
    if (n == 0 || (n >= 16 && (n & (n - 1)) == 0)) {
        void *p = realloc(*slot, (size_t)(n ? 2 * n : 16) * sz);
        if (!p)
            return LRV_ERR_NOMEM;
        *slot = p;
    }
    return LRV_OK;
}

static int cmp_at(const void *a, const void *b)
{
    const struct lrv_autoplay_at *x = a, *y = b;
    if (x->tick != y->tick)
        return x->tick < y->tick ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* at:T=KEY[+KEY..],.. ; p is after the colon */
static int parse_at(struct lrv_autoplay *ap, const char *p, const struct errbuf *e)
{
    while (!at_end((unsigned char)*p)) {
        struct lrv_autoplay_at *a;
        char *end;
        uint32_t tick = (uint32_t)strtoul(p, &end, 10);

        if (end == p || *end != '=')
            return fail(e, LRV_ERR_FORMAT, "expected T=KEY", p);
        if (grow(&ap->at, ap->at_count, sizeof *ap->at) != LRV_OK)
            return fail(e, LRV_ERR_NOMEM, "out of memory", "");
        a = &ap->at[ap->at_count];
        memset(a, 0, sizeof *a);
        a->tick = tick;
        a->seq = (uint32_t)ap->at_count;
        p = end + 1;
        for (;;) {
            size_t n = 0;
            int key;
            uint32_t event;
            while (!at_end((unsigned char)p[n]) && p[n] != '+' && p[n] != ',')
                n++;
            if (!lrv_input_by_name(p, n, &key, &event))
                return fail(e, LRV_ERR_FORMAT, "unknown key", p);
            if (event)
                a->events |= event;
            else if (a->nkeys < LRV_AUTOPLAY_CHORD)
                a->keys[a->nkeys++] = (uint8_t)key;
            else
                return fail(e, LRV_ERR_FORMAT, "too many keys at one tick", p);
            p += n;
            if (*p != '+')
                break;
            p++;
        }
        ap->at_count++;
        if (*p == ',')
            p++;
    }
    return LRV_OK;
}

/* "wait" ending at a word end or a comma */
static int is_wait(const char *p)
{
    return !strncmp(p, "wait", 4) && (at_end((unsigned char)p[4]) || p[4] == ',');
}

static int parse_snap(struct lrv_autoplay *ap, const char *p, const struct errbuf *e)
{
    while (!at_end((unsigned char)*p)) {
        char *end;
        uint32_t tick;
        if (is_wait(p)) {
            ap->snap_wait = 1;
            p += 4;
            if (*p == ',')
                p++;
            continue;
        }
        tick = (uint32_t)strtoul(p, &end, 10);
        if (end == p)
            return fail(e, LRV_ERR_FORMAT, "expected a snap tick", p);
        if (grow(&ap->snap, ap->snap_count, sizeof *ap->snap) != LRV_OK)
            return fail(e, LRV_ERR_NOMEM, "out of memory", "");
        ap->snap[ap->snap_count++] = tick;
        p = end;
        if (*p == ',')
            p++;
    }
    return LRV_OK;
}

/* KEY:N, KEY:T0-T1,.. and button_l / button_r:T0-T1,.. ; w is the word, colon at w + n */
static int parse_key_word(struct lrv_autoplay *ap, const char *w, size_t n,
                          const struct errbuf *e)
{
    const char *p = w + n + 1;
    unsigned buttons = 0;
    int key = LRV_K_NONE, ranges = 0;
    uint32_t event = 0;
    size_t i;

    if (n == 8 && !strncmp(w, "button_l", 8))
        buttons = LRV_BTN_L;
    else if (n == 8 && !strncmp(w, "button_r", 8))
        buttons = LRV_BTN_R;
    else if (!lrv_input_by_name(w, n, &key, &event) || event)
        return fail(e, LRV_ERR_FORMAT, "unknown word", w);
    if (is_wait(p) && at_end((unsigned char)p[4])) {
        if (buttons)
            return fail(e, LRV_ERR_FORMAT, "a button cannot be pressed at waits", w);
        if (ap->wait_nkeys >= LRV_AUTOPLAY_CHORD)
            return fail(e, LRV_ERR_FORMAT, "too many keys at waits", w);
        ap->wait_keys[ap->wait_nkeys++] = (uint8_t)key;
        return LRV_OK;
    }
    for (i = 0; !at_end((unsigned char)p[i]); i++)
        if (p[i] == '-')
            ranges = 1;

    if (!ranges) {
        char *end;
        unsigned long every = strtoul(p, &end, 10);
        if (end == p || !at_end((unsigned char)*end) || buttons)
            return fail(e, LRV_ERR_FORMAT, buttons ? "expected T0-T1" : "expected a period", w);
        if (every == 0)
            return LRV_OK;
        if (grow(&ap->every, ap->every_count, sizeof *ap->every) != LRV_OK)
            return fail(e, LRV_ERR_NOMEM, "out of memory", "");
        ap->every[ap->every_count].every = (uint32_t)every;
        ap->every[ap->every_count].key = (uint8_t)key;
        ap->every_count++;
        return LRV_OK;
    }
    while (!at_end((unsigned char)*p)) {
        struct lrv_autoplay_hold *h;
        char *end;
        uint32_t t0 = (uint32_t)strtoul(p, &end, 10), t1;
        if (end == p || *end != '-')
            return fail(e, LRV_ERR_FORMAT, "expected T0-T1", p);
        p = end + 1;
        t1 = (uint32_t)strtoul(p, &end, 10);
        if (end == p)
            return fail(e, LRV_ERR_FORMAT, "expected T0-T1", p);
        if (grow(&ap->hold, ap->hold_count, sizeof *ap->hold) != LRV_OK)
            return fail(e, LRV_ERR_NOMEM, "out of memory", "");
        h = &ap->hold[ap->hold_count++];
        h->t0 = t0;
        h->t1 = t1;
        h->key = (uint8_t)key;
        h->buttons = (uint8_t)buttons;
        p = end;
        if (*p == ',')
            p++;
    }
    return LRV_OK;
}

static int parse_spec(struct lrv_autoplay *ap, const char *s, const struct errbuf *e)
{
    for (;;) {
        size_t colon = 0;
        int rc;

        while (is_blank((unsigned char)*s))
            s++;
        if (!*s)
            return LRV_OK;
        while (!at_end((unsigned char)s[colon]) && s[colon] != ':')
            colon++;
        if (s[colon] != ':')
            return fail(e, LRV_ERR_FORMAT, "unknown word", s);
        if (colon == 2 && !strncmp(s, "at", 2))
            rc = parse_at(ap, s + 3, e);
        else if (colon == 4 && !strncmp(s, "snap", 4))
            rc = parse_snap(ap, s + 5, e);
        else if (colon == 4 && !strncmp(s, "rand", 4)) {
            ap->rnd = (unsigned)strtoul(s + 5, 0, 10) | 1u;
            rc = LRV_OK;
        } else if (colon == 5 && !strncmp(s, "waits", 5)) {
            char *end;
            unsigned long n = strtoul(s + 6, &end, 10);
            if (end == s + 6 || !at_end((unsigned char)*end) || n == 0)
                return fail(e, LRV_ERR_FORMAT, "expected waits:N", s);
            ap->waits_limit = (uint32_t)n;
            rc = LRV_OK;
        } else {
            rc = parse_key_word(ap, s, colon, e);
        }
        if (rc != LRV_OK)
            return rc;
        while (!at_end((unsigned char)*s))
            s++;
    }
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf = 0;
    size_t len = 0, cap = 0, n;

    if (!f)
        return 0;
    for (;;) {
        if (cap - len < 4096) {
            char *nb = realloc(buf, cap + 65536);
            if (!nb) {
                free(buf);
                fclose(f);
                return 0;
            }
            buf = nb;
            cap += 65536;
        }
        n = fread(buf + len, 1, cap - len - 1, f);
        if (n == 0)
            break;
        len += n;
    }
    fclose(f);
    buf[len] = 0;
    return buf;
}

int lrv_autoplay_parse(struct lrv_autoplay *ap, const char *spec, char *err, size_t errlen)
{
    struct errbuf e;
    int rc;

    e.buf = err;
    e.len = errlen;
    memset(ap, 0, sizeof *ap);
    if (spec[0] == '@') {
        char *text = read_file(spec + 1);
        if (!text)
            return fail(&e, LRV_ERR_IO, "cannot read the spec file", spec + 1);
        rc = parse_spec(ap, text, &e);
        free(text);
    } else {
        rc = parse_spec(ap, spec, &e);
    }
    if (rc != LRV_OK) {
        lrv_autoplay_free(ap);
        return rc;
    }
    if (ap->at_count)
        qsort(ap->at, (size_t)ap->at_count, sizeof *ap->at, cmp_at);
    if (ap->snap_count)
        qsort(ap->snap, (size_t)ap->snap_count, sizeof *ap->snap, cmp_u32);
    ap->rnd_state = ap->rnd;
    return LRV_OK;
}

void lrv_autoplay_free(struct lrv_autoplay *ap)
{
    free(ap->at);
    free(ap->hold);
    free(ap->every);
    free(ap->snap);
    memset(ap, 0, sizeof *ap);
}

/* ---- delivery ----------------------------------------------------------------------------- */

void lrv_autoplay_poll(struct lrv_autoplay *ap, uint32_t tick, struct lrv_input *in)
{
    int i, k, first = !ap->polled || tick != ap->last_tick;

    ap->polled = 1;
    ap->last_tick = tick;
    /* the levels first: a held shift is a modifier of the keys pressed at this tick */
    for (i = 0; i < ap->hold_count; i++) {
        const struct lrv_autoplay_hold *h = &ap->hold[i];
        if (tick < h->t0 || tick >= h->t1)
            continue;
        in->buttons |= h->buttons;
        if (h->key != LRV_K_NONE) {
            lrv_key_set(in, h->key, 1);
            in->mods |= lrv_input_key_mod(h->key);
        }
    }
    deliver_events(in, ap->kept);
    ap->kept = 0;
    if (ap->rnd) {
        if (first && tick % 20 == 0) {
            static const uint8_t cmds[] = {
                LRV_K_RETURN, LRV_K_RETURN, LRV_K_RETURN, LRV_K_DOWN, LRV_K_UP, LRV_K_ESC
            };
            ap->rnd_state = ap->rnd_state * 1103515245u + 12345u;
            press(in, cmds[(ap->rnd_state >> 16) % 6]);
        }
    } else if (ap->at_count || ap->hold_count) {
        for (; ap->at_next < ap->at_count && ap->at[ap->at_next].tick <= tick; ap->at_next++) {
            const struct lrv_autoplay_at *a = &ap->at[ap->at_next];
            for (k = 0; k < a->nkeys; k++)
                press(in, a->keys[k]);
            deliver_events(in, a->events);
        }
    } else if (first) {
        for (i = 0; i < ap->every_count; i++)
            if (tick % ap->every[i].every == 0)
                press(in, ap->every[i].key);
    }
    if (ap->waits_limit && ap->waits_done >= ap->waits_limit) {
        in->events |= LRV_EV_EMERGENCY;         /* the run is over: no more presses */
        ap->press_due = 0;
    } else if (ap->press_due) {
        for (k = 0; k < ap->wait_nkeys; k++)
            press(in, ap->wait_keys[k]);
        ap->press_due = 0;
    }
}

void lrv_autoplay_flush(struct lrv_autoplay *ap, uint32_t tick)
{
    while (ap->at_next < ap->at_count && ap->at[ap->at_next].tick <= tick)
        ap->kept |= ap->at[ap->at_next++].events & LRV_EV_STICKY;
    ap->press_due = 0;                          /* type-ahead, as the control protocol drops it */
}

/* ---- input waits -------------------------------------------------------------------------- */

int lrv_autoplay_uses_waits(const struct lrv_autoplay *ap)
{
    return ap->wait_nkeys || ap->snap_wait || ap->waits_limit;
}

void lrv_autoplay_wait(struct lrv_autoplay *ap, int enter)
{
    ap->in_wait = enter != 0;
    ap->wait_ticks = 0;
}

/* A wait's point is the end of the first whole tick inside it: the second tick end after the
 * enter (the first ends the tick the engine entered it in, after whatever it drew before). */
void lrv_autoplay_end_tick(struct lrv_autoplay *ap, uint32_t tick)
{
    ap->ended_tick = tick;
    ap->point = 0;
    if (ap->in_wait && ++ap->wait_ticks == 2) {
        ap->point = 1;
        ap->waits_done++;
        ap->press_due = ap->wait_nkeys > 0;
    }
}

/* the end of `tick` is (or, asked during it, will be) a wait's point */
static int wait_point(const struct lrv_autoplay *ap, uint32_t tick)
{
    if (tick == ap->ended_tick)
        return ap->point;
    return tick == ap->ended_tick + 1 && ap->in_wait && ap->wait_ticks == 1;
}

static int in_snap_list(const struct lrv_autoplay *ap, uint32_t tick)
{
    int lo = 0, hi = ap->snap_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (ap->snap[mid] == tick)
            return 1;
        if (ap->snap[mid] < tick)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

int lrv_autoplay_snap_due(const struct lrv_autoplay *ap, uint32_t tick)
{
    return in_snap_list(ap, tick) || (ap->snap_wait && wait_point(ap, tick));
}

void lrv_autoplay_snap_name(const struct lrv_autoplay *ap, uint32_t tick, char *buf, size_t len)
{
    if (in_snap_list(ap, tick) || !ap->snap_wait || !wait_point(ap, tick))
        snprintf(buf, len, "t%lu", (unsigned long)tick);
    else
        snprintf(buf, len, "w%lu", (unsigned long)(ap->waits_done + (tick != ap->ended_tick)));
}

uint32_t lrv_autoplay_last_tick(const struct lrv_autoplay *ap)
{
    uint32_t t = 0;
    int i;
    if (ap->at_count && ap->at[ap->at_count - 1].tick > t)
        t = ap->at[ap->at_count - 1].tick;
    if (ap->snap_count && ap->snap[ap->snap_count - 1] > t)
        t = ap->snap[ap->snap_count - 1];
    for (i = 0; i < ap->hold_count; i++)
        if (ap->hold[i].t1 > t)
            t = ap->hold[i].t1;
    return t;
}
