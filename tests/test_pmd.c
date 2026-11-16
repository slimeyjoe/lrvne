/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The PMD module (lrvne/pmd.h) against the real driver: tests/data/pmd holds songs of our own
 * and what PMD.COM 4.7g did with them, interrupt by interrupt (README.md there). Each log is
 * replayed: the same installation, the same INT 60h calls between the same interrupts, every
 * interrupt at its own master clock; the register writes must be the same, in the same order,
 * at the same clocks, and so must the driver's status after each interrupt. Then the frame-
 * driven path: the same song advanced in output samples puts each write on the sample of its
 * clock. Run from the repository's root (make test does). */
#ifdef LRV_HAVE_PMD
#include "test_hal.h"
#include "lrvne/pmd.h"

#define DATA "tests/data/pmd/"
#define MAXW (1u << 20)

/* what the driver wrote, with the clock it wrote at */
static struct wr { uint64_t clock; uint8_t reg, val; } *got;
static size_t ngot;
static struct lrv_pmd pmd;

static void on_write(void *ctx, uint8_t reg, uint8_t val)
{
    (void)ctx;
    if (ngot < MAXW) {
        got[ngot].clock = pmd.clock;
        got[ngot].reg = reg;
        got[ngot].val = val;
    }
    ngot++;
}

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f) {
        fprintf(stderr, "test_pmd: cannot open %s (run from the repository's root)\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "test_pmd: cannot read %s\n", path);
        exit(1);
    }
    fclose(f);
    buf[n] = 0;
    *size = (size_t)n;
    return buf;
}

/* the "r=v r=v ..." tail of a log line, from s on, against got[from..ngot) */
static int same_writes(const char *s, size_t from, uint64_t clock, int check_clock,
                       const char *what, int line)
{
    size_t i = from;
    unsigned r, v;
    int n, ok = 1;
    while (sscanf(s, " %2x=%2x%n", &r, &v, &n) == 2) {
        if (i >= ngot || got[i].reg != r || got[i].val != v ||
            (check_clock && got[i].clock != clock)) {
            ok = 0;
            break;
        }
        i++;
        s += n;
    }
    if (ok && i != ngot)
        ok = 0;
    if (!ok) {
        size_t k;
        fprintf(stderr, "%s line %d: the writes differ at write %zu\n  real: %s\n  ours:", what,
                line, i - from, s);
        for (k = i; k < ngot && k < i + 12; k++)
            fprintf(stderr, " %02X=%02X@%llu", got[k].reg, got[k].val,
                    (unsigned long long)got[k].clock);
        fprintf(stderr, "%s\n", ngot > i + 12 ? " ..." : "");
    }
    return ok;
}

static void int60(unsigned ax)
{
    switch (ax >> 8) {
    case 0x00: lrv_pmd_mstart(&pmd); break;
    case 0x01: lrv_pmd_mstop(&pmd); break;
    case 0x02: lrv_pmd_fade(&pmd, (uint8_t)ax); break;
    case 0x03: lrv_pmd_ssg_effect_start(&pmd, ax & 0xFF, 0); break;
    case 0x04: lrv_pmd_ssg_effect_stop(&pmd); break;
    case 0x19: lrv_pmd_set_fadevol(&pmd, (uint8_t)ax); break;
    default:
        fprintf(stderr, "test_pmd: INT 60h AX=%04X is not replayed\n", ax);
        th_fails++;
    }
}

/* Replays one log; returns the number of interrupts compared. */
static int replay(const char *log, const char *song, const char *table)
{
    static uint8_t music[0x4000];
    size_t size, tsize = 0, msize;
    char *text = (char *)load(log, &size), *line, *next;
    uint8_t *m = load(song, &msize), *tab = table ? load(table, &tsize) : 0;
    int lineno = 0, irqs = 0, bad = 0;

    memset(music, 0, sizeof music);
    memcpy(music, m, msize);
    ngot = 0;
    lrv_pmd_init(&pmd, on_write, 0, 44100);
    for (line = text; line && *line && bad < 3; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        lineno++;
        if (line[0] == '#') {
            if (strstr(line, "options") && !strstr(line, "/F"))
                pmd.stop_after_fade = 1;
            else if (strstr(line, "options"))
                pmd.stop_after_fade = 0;
            continue;
        }
        if (line[0] == 'I') {
            bad += !same_writes(line + 1, 0, 0, 0, log, lineno);
            if (tab)
                lrv_pmd_set_ssgtab(&pmd, tab, (uint32_t)tsize);
            /* the engines copy the whole file into PMD's buffer: data from byte 1 */
            lrv_pmd_load_music(&pmd, music + 1, (uint32_t)msize - 1);
        } else if (line[0] == 'S') {
            unsigned ax;
            int n;
            size_t from = ngot;
            if (sscanf(line + 1, " %4x%n", &ax, &n) != 1)
                break;
            int60(ax);
            bad += !same_writes(line + 1 + n, from, 0, 0, log, lineno);
        } else if (line[0] == 'T') {
            unsigned long long clock;
            char kind[3];
            unsigned s2, meas, fade, s1;
            uint8_t ps1, ps2;
            int n;
            size_t from = ngot;
            if (sscanf(line + 1, " %llu %2s s2=%2x m=%4x f=%2x s1=%2x%n", &clock, kind, &s2,
                       &meas, &fade, &s1, &n) != 6)
                break;
            lrv_pmd_run_until(&pmd, clock);
            bad += !same_writes(line + 1 + n, from, clock, 1, log, lineno);
            lrv_pmd_get_status(&pmd, &ps1, &ps2);
            if (ps1 != s1 || ps2 != s2 || lrv_pmd_get_fadevol(&pmd) != fade) {
                fprintf(stderr, "%s line %d: status %02X/%02X fade %02X, real %02X/%02X %02X\n",
                        log, lineno, ps1, ps2, lrv_pmd_get_fadevol(&pmd), s1, s2, fade);
                bad++;
            }
            (void)meas;
            irqs++;
        }
    }
    CHECK(bad == 0 && irqs > 0);
    free(text);
    free(m);
    free(tab);
    return irqs;
}

/* The frame-driven path: advanced in output samples, the driver fires each interrupt at the
 * first sample at or after its clock, and renders the samples in between. */
static long rendered;
static void count_render(void *ctx, int n)
{
    (void)ctx;
    rendered += n;
}

static struct wr *clocked;
static size_t nclocked;

static void test_frames(void)
{
    static uint8_t music[0x4000];
    size_t msize, i;
    uint8_t *m = load(DATA "SYNTH1.M", &msize);
    const uint32_t rate = 55466;
    long total = 0;

    /* the reference: the same song run by clocks */
    memset(music, 0, sizeof music);
    memcpy(music, m, msize);
    ngot = 0;
    lrv_pmd_init(&pmd, on_write, 0, rate);
    lrv_pmd_load_music(&pmd, music + 1, (uint32_t)msize - 1);
    lrv_pmd_mstart(&pmd);
    lrv_pmd_run_until(&pmd, (uint64_t)LRV_OPN_CLOCK_PC98 * 20);
    nclocked = ngot;
    clocked = malloc(sizeof *clocked * nclocked);
    memcpy(clocked, got, sizeof *clocked * nclocked);

    /* the same in blocks of samples of different sizes */
    memset(music, 0, sizeof music);
    memcpy(music, m, msize);
    ngot = 0;
    rendered = 0;
    lrv_pmd_init(&pmd, on_write, 0, rate);
    lrv_pmd_load_music(&pmd, music + 1, (uint32_t)msize - 1);
    lrv_pmd_mstart(&pmd);
    while (total < (long)rate * 20) {
        int n = 1 + (int)(total % 997);
        lrv_pmd_advance(&pmd, n, count_render, 0);
        total += n;
    }
    CHECK(rendered == total);
    CHECK(ngot >= nclocked && nclocked > 1000);
    for (i = 0; i < nclocked && i < ngot; i++) {
        /* the sample the write is made at: the first one whose clock is not before it */
        uint64_t want = (clocked[i].clock * rate + LRV_OPN_CLOCK_PC98 - 1) / LRV_OPN_CLOCK_PC98;
        uint64_t at = (got[i].clock * rate + LRV_OPN_CLOCK_PC98 - 1) / LRV_OPN_CLOCK_PC98;
        if (got[i].reg != clocked[i].reg || got[i].val != clocked[i].val ||
            (at > want + 1 || at + 1 < want)) {
            fprintf(stderr, "frames: write %zu %02X=%02X at sample %llu, by clocks %02X=%02X "
                    "at %llu\n", i, got[i].reg, got[i].val, (unsigned long long)at,
                    clocked[i].reg, clocked[i].val, (unsigned long long)want);
            th_fails++;
            break;
        }
    }
    free(clocked);
    free(m);
}

int main(void)
{
    int n = 0;
    got = malloc(sizeof *got * MAXW);
    n += replay(DATA "synth1.log", DATA "SYNTH1.M", 0);
    n += replay(DATA "synth1-fade.log", DATA "SYNTH1.M", 0);
    n += replay(DATA "synth2.log", DATA "SYNTH2.M", 0);
    test_frames();
    free(got);
    if (th_fails) {
        fprintf(stderr, "test_pmd: %d check(s) failed\n", th_fails);
        return 1;
    }
    printf("test_pmd: ok (%d interrupts of the real driver)\n", n);
    return 0;
}

#else
#include <stdio.h>

int main(void)
{
    printf("test_pmd: skipped (built without LRVNE_PMD)\n");
    return 0;
}

#endif
