/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The 68k assembly render loop of the light OPN core (src/snd/opn_lite_68k.s) against the C
 * loop it replaces, on a 68040 (tests/opn68k.sh builds this program and runs it under
 * qemu-m68k).
 *
 * Two copies of src/snd/opn_lite.c are linked: one built with LRV_OPN_ASM_68K, whose render
 * is the assembly loop, and one without, with its external names prefixed ref_. Both get the
 * same register writes, a pseudo-random song of key events, operator and frequency changes,
 * SSG and FM3 writes and plain noise, and render the same blocks of odd lengths at several
 * output rates. Every sample must be equal. The program prints the number of samples
 * compared, how many were not silence and their hash, and exits with 0 when all were equal.
 *
 * A freestanding m68k Linux program: no C library is needed, only a cross compiler. The
 * assembly is written for vbcc's register parameters (a0, a1, d0) and its underscore names;
 * a trampoline below takes the arguments from the stack, and the linker gives the C symbols
 * their underscored aliases. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "lrvne/opn.h"

/* ---- startup and the two system calls ---------------------------------------------------- */

__asm__(
    "   .text\n"
    "   .globl _start\n"
    "_start:\n"
    "   jsr     main\n"
    "   move.l  %d0,%d1\n"
    "   moveq   #1,%d0\n"                   /* exit */
    "   trap    #0\n"
    "   .globl sys_write\n"
    "sys_write:\n"
    "   movem.l %d2-%d3,-(%sp)\n"
    "   move.l  12(%sp),%d1\n"
    "   move.l  16(%sp),%d2\n"
    "   move.l  20(%sp),%d3\n"
    "   moveq   #4,%d0\n"                   /* write */
    "   trap    #0\n"
    "   movem.l (%sp)+,%d2-%d3\n"
    "   rts\n"
    /* void lrv_opn_lite_render_68k(struct lrv_opn *c, int16_t *mono, int frames) as the C
     * side calls it here: the arguments into the registers the assembly takes them in */
    "   .globl lrv_opn_lite_render_68k\n"
    "lrv_opn_lite_render_68k:\n"
    "   move.l  4(%sp),%a0\n"
    "   move.l  8(%sp),%a1\n"
    "   move.l  12(%sp),%d0\n"
    "   jmp     _lrv_opn_lite_render_68k\n");

long sys_write(int fd, const void *buf, unsigned long len);

/* ---- what the core needs of a C library and of lrvne ------------------------------------- */

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

static unsigned char g_arena[16384];
static size_t g_used;

void *lrv_alloc(const struct lrv_hal *hal, size_t n)
{
    void *p = g_arena + g_used;
    (void)hal;
    n = (n + 3) & ~(size_t)3;
    if (g_used + n > sizeof g_arena)
        return 0;
    g_used += n;
    return memset(p, 0, n);
}

void lrv_free(const struct lrv_hal *hal, void *p, size_t n)
{
    (void)hal;
    (void)p;
    (void)n;
}

/* ---- output ------------------------------------------------------------------------------ */

static void put(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    sys_write(1, s, n);
}

static void put_num(uint32_t v, unsigned base, int width)
{
    char buf[12];
    int i = (int)sizeof buf;
    buf[--i] = 0;
    do {
        buf[--i] = "0123456789abcdef"[v % base];
        v /= base;
        width--;
    } while (v || width > 0);
    put(buf + i);
}

/* ---- the comparison ---------------------------------------------------------------------- */

extern const struct lrv_opn_ops ref_opn_lite_ops;       /* the C loop */

static struct lrv_opn *g_asm, *g_ref;
static uint32_t g_rnd = 0x2545F491u;

static uint32_t rnd(void)
{
    g_rnd ^= g_rnd << 13;
    g_rnd ^= g_rnd >> 17;
    g_rnd ^= g_rnd << 5;
    return g_rnd;
}

static void wr(unsigned reg, unsigned val)
{
    lrv_opn_lite_ops.write(g_asm, (uint8_t)reg, (uint8_t)val);
    ref_opn_lite_ops.write(g_ref, (uint8_t)reg, (uint8_t)val);
}

/* a patch that sounds on every channel, so the song starts from sound and not from silence */
static void patch(void)
{
    unsigned ch, op;
    for (ch = 0; ch < 3; ch++) {
        for (op = 0; op < 4; op++) {
            unsigned o = ch + op * 4;
            wr(0x30 + o, 0x01 + op);            /* detune / multiple */
            wr(0x40 + o, op == 3 ? 0x00 : 0x18);/* total level */
            wr(0x50 + o, 0x1F);                 /* attack */
            wr(0x60 + o, 0x05);                 /* decay */
            wr(0x70 + o, 0x02);                 /* sustain rate */
            wr(0x80 + o, 0x27);                 /* sustain level / release */
        }
        wr(0xB0 + ch, (rnd() & 7) << 3 | ch * 3);
        wr(0xA4 + ch, 0x22 + ch * 4);
        wr(0xA0 + ch, 0x69 + ch * 40);
        wr(0x28, 0xF0 | ch);
    }
    wr(0x07, 0x38);                             /* SSG: tones on */
    wr(0x00, 0xFE); wr(0x01, 0x00); wr(0x08, 0x0C);
    wr(0x02, 0x7F); wr(0x03, 0x01); wr(0x09, 0x10);
    wr(0x0B, 0x40); wr(0x0C, 0x02); wr(0x0D, 0x0E);
}

/* one event of the song */
static void event(void)
{
    static const uint8_t base[7] = { 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90 };
    unsigned r = rnd() % 100, ch = rnd() % 3, v = rnd() & 0xFF;
    if (r < 30) {
        wr(0x28, ((rnd() & 3) ? 0xF0 : 0x00) | ch);
    } else if (r < 55) {
        unsigned b = base[rnd() % 7];
        if (b == 0x40)
            v &= (rnd() & 7) ? 0x1F : 0x7F;     /* mostly audible levels */
        wr(b + ch + (rnd() & 3) * 4, v);
    } else if (r < 70) {
        wr(0xA4 + ch, v & 0x3F);
        wr(0xA0 + ch, rnd() & 0xFF);
    } else if (r < 80) {
        wr(0xB0 + ch, v & 0x3F);
    } else if (r < 90) {
        wr(rnd() % 14, v);
    } else if (r < 95) {
        wr(0x27, v);                            /* the FM3 mode bits and its slot frequencies */
        wr(0xAC + ch, rnd() & 0x3F);
        wr(0xA8 + ch, rnd() & 0xFF);
    } else {
        wr(rnd() & 0xFF, v);
    }
}

#define BLOCK_MAX 300
static int16_t g_a[BLOCK_MAX], g_r[BLOCK_MAX];

int main(void)
{
    static const uint32_t rates[] = { 11025, 22050, 44100, 55466 };
    uint32_t hash = 2166136261u, total = 0, sounding = 0;
    unsigned ri, step, i;

    for (ri = 0; ri < sizeof rates / sizeof rates[0]; ri++) {
        uint32_t ra = 0, rr = 0;
        g_used = 0;
        g_asm = lrv_opn_lite_ops.create(0, LRV_OPN_CLOCK_PC98, rates[ri], 0, &ra);
        g_ref = ref_opn_lite_ops.create(0, LRV_OPN_CLOCK_PC98, rates[ri], 0, &rr);
        if (!g_asm || !g_ref || ra != rr) {
            put("opn68k: cannot create the cores\n");
            return 2;
        }
        patch();
        for (step = 0; step < 600; step++) {
            int n = 1 + (int)(rnd() % BLOCK_MAX), events = (int)(rnd() % 4);
            /* now and then nothing sounds for a while: the paths that skip work */
            if (step % 97 == 96) {
                wr(0x28, 0x00); wr(0x28, 0x01); wr(0x28, 0x02);
                wr(0x08, 0); wr(0x09, 0); wr(0x0A, 0);
                events = 0;
            }
            if (step % 97 == 20)
                patch();
            while (events-- > 0)
                event();
            lrv_opn_lite_ops.render(g_asm, g_a, n);
            ref_opn_lite_ops.render(g_ref, g_r, n);
            for (i = 0; i < (unsigned)n; i++) {
                if (g_a[i] != g_r[i]) {
                    put("opn68k: MISMATCH at rate ");
                    put_num(ra, 10, 0);
                    put(", block ");
                    put_num(step, 10, 0);
                    put(", sample ");
                    put_num(i, 10, 0);
                    put(": assembly ");
                    put_num((uint16_t)g_a[i], 16, 4);
                    put(", C ");
                    put_num((uint16_t)g_r[i], 16, 4);
                    put("\n");
                    return 1;
                }
                hash = (hash ^ (uint8_t)g_a[i]) * 16777619u;
                hash = (hash ^ (uint8_t)((uint16_t)g_a[i] >> 8)) * 16777619u;
                sounding += g_a[i] != 0;
            }
            total += (uint32_t)n;
        }
    }
    put("opn68k: ");
    put_num(total, 10, 0);
    put(" samples at 4 rates, ");
    put_num(sounding, 10, 0);
    put(" not silent, hash ");
    put_num(hash, 16, 8);
    put(": the assembly loop equals the C loop\n");
    return sounding > total / 4 ? 0 : 3;
}
