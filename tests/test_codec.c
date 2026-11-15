/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests of the decoders: LZSS on a stream written by hand, and the voice clip codecs on
 * clips the test encodes itself. */
#include "test_hal.h"
#include "lrvne/lzss.h"
#include "lrvne/vdec.h"

/* ---- LZSS ----------------------------------------------------------------------------------- */

static void test_lzss(void)
{
    /* flag bytes are read LSB first: 1 = a literal, 0 = a match (position, length - 3). The
     * ring is written from 0xFEE on. */
    static const uint8_t stream[] = {
        0x0F, 'a', 'b', 'c', 'd',       /* four literals: ring[0xFEE..0xFF1] */
        0xEE, 0xF1,                     /* 4 bytes from 0xFEE: "abcd" again */
        0xF5, 0xF3,                     /* 6 from 0xFF5, the byte just before the write
                                         * position: the copy overlaps itself, a run of 'd' */
        0x00, 0x00,                     /* 3 from 0x000: the ring starts as zeros */
        0xEE, 0xFF,                     /* 18 (the longest) from 0xFEE: the write position
                                         * wraps to 0, the last byte read is the first written */
        0x01, 'Z'                       /* a literal; the match the flags promise is not there */
    };
    static const uint8_t want[] = "abcdabcddddddd\0\0\0abcdabcddddddd\0\0\0aZ";
    static uint8_t ring[LRV_LZSS_RING];
    uint8_t out[64];
    size_t n;

    memset(out, 0xEE, sizeof out);
    n = lrv_lzss_decode(stream, sizeof stream, out, sizeof out, ring);
    CHECK(n == 36 && !memcmp(out, want, 36) && out[36] == 0xEE);

    /* a full buffer: the rest is dropped, nothing is written past it */
    memset(out, 0xEE, sizeof out);
    n = lrv_lzss_decode(stream, sizeof stream, out, 10, ring);
    CHECK(n == 10 && !memcmp(out, want, 10) && out[10] == 0xEE);

    /* the input ending inside an item ends the output before it */
    CHECK(lrv_lzss_decode(stream, 6, out, sizeof out, ring) == 4);      /* half a match */
    CHECK(lrv_lzss_decode(stream, 3, out, sizeof out, ring) == 2);
    CHECK(lrv_lzss_decode(stream, 1, out, sizeof out, ring) == 0);      /* flags only */
    CHECK(lrv_lzss_decode(stream, 0, out, sizeof out, ring) == 0);
}

/* ---- a voice pack with one large clip ----------------------------------------------------- */

#define CLIP_MAX 150000
static uint8_t big_pack[LRV_PACK_HEADER_SIZE + LRV_PACK_ENTRY_SIZE + CLIP_MAX + 4];
static uint8_t clip[CLIP_MAX];
static int pack_reads;                  /* reads the backend was asked for */
static int fail_window;                 /* the read-ahead window cannot be allocated */

static size_t counting_read(void *h, uint32_t off, void *dst, size_t n)
{
    pack_reads++;
    return th_pack_read(h, off, dst, n);
}

static void *failing_alloc(size_t n, unsigned flags)
{
    if (fail_window && n == LRV_VDEC_READAHEAD)
        return 0;
    return th_alloc(n, flags);
}

static void clip_hal(struct lrv_hal *hal)
{
    th_hal(hal);
    hal->pack_read = counting_read;
    hal->mem_alloc = failing_alloc;
}

/* makes clip[0 .. size) the one entry of the voice pack and opens it */
static const struct lrv_pack_entry *clip_pack(struct lrv_pack *pk, const struct lrv_hal *hal,
                                              uint32_t size)
{
    uint32_t data = LRV_PACK_HEADER_SIZE + LRV_PACK_ENTRY_SIZE;
    uint8_t *e = big_pack + LRV_PACK_HEADER_SIZE;
    memset(big_pack, 0, data);
    memcpy(big_pack, "XPAK", 4);
    lrv_wr16le(big_pack + 4, LRV_PACK_VERSION);
    lrv_wr16le(big_pack + 6, 1);
    lrv_wr32le(big_pack + 8, data + size);
    memcpy(e, "CLIP", 4);
    e[20] = LRV_PT_VOICE;
    lrv_wr32le(e + 24, data);
    lrv_wr32le(e + 28, size);
    memcpy(big_pack + data, clip, size);
    th_pack[LRV_PACK_VOICE] = big_pack;
    th_pack_size[LRV_PACK_VOICE] = data + size;
    if (lrv_pack_open(pk, hal, LRV_PACK_VOICE) != LRV_OK)
        return 0;
    return lrv_pack_find(pk, "clip", LRV_PT_VOICE);
}

static uint32_t rng_state;

static uint32_t rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* reads the whole clip in pieces of uneven sizes; returns the number of samples */
static int read_all(struct lrv_vdec *d, int16_t *out, int cap)
{
    static const int sizes[] = { 1, 255, 256, 257, 3, 1000, 5000, 2 };
    int n = 0, i = 0, got;
    for (;;) {
        int want = sizes[i++ % 8];
        if (want > cap - n)
            want = cap - n;
        got = lrv_vdec_read(d, out + n, want);
        if (got <= 0)
            break;
        n += got;
    }
    return n;
}

static int16_t pcm_out[CLIP_MAX + 16], pcm_want[CLIP_MAX + 16];

/* ---- pcm8 ----------------------------------------------------------------------------------- */

static void test_pcm8(void)
{
    enum { N = 40000 };
    struct lrv_hal hal;
    struct lrv_pack pk;
    struct lrv_vdec d;
    const struct lrv_pack_entry *e;
    int i, n, pass;

    clip_hal(&hal);
    memcpy(clip, "pcm8", 4);
    lrv_wr32le(clip + 4, 11025);
    lrv_wr32le(clip + 8, N);
    rng_state = 1;
    for (i = 0; i < N; i++) {
        clip[LRV_VDEC_PCM8_HEADER + i] = (uint8_t)rng();
        pcm_want[i] = (int16_t)((int8_t)clip[LRV_VDEC_PCM8_HEADER + i] * 256);
    }
    clip[LRV_VDEC_PCM8_HEADER] = 0x80;          /* the extremes */
    clip[LRV_VDEC_PCM8_HEADER + 1] = 0x7F;
    pcm_want[0] = -32768;
    pcm_want[1] = 32512;

    /* with the read-ahead window, and without it when there is no memory for one */
    for (pass = 0; pass < 2; pass++) {
        fail_window = pass;
        e = clip_pack(&pk, &hal, LRV_VDEC_PCM8_HEADER + N);
        CHECK(e != 0);
        lrv_vdec_init(&d);
        pack_reads = 0;
        CHECK(lrv_vdec_open(&d, &pk, e) == LRV_OK && d.kind == LRV_VDEC_PCM8 && d.rate == 11025);
        n = read_all(&d, pcm_out, N + 16);
        CHECK(n == N && !memcmp(pcm_out, pcm_want, sizeof pcm_out[0] * N));
        CHECK(lrv_vdec_read(&d, pcm_out, 10) == 0);
        if (pass == 0)
            CHECK(pack_reads == 3);             /* 40012 bytes in windows of 16384 */
        else
            CHECK(pack_reads > 100);
        /* the same decoder again, for another clip: the window is not that of the first */
        clip[LRV_VDEC_PCM8_HEADER] = 0x01;
        lrv_pack_close(&pk);
        e = clip_pack(&pk, &hal, LRV_VDEC_PCM8_HEADER + N);
        CHECK(lrv_vdec_open(&d, &pk, e) == LRV_OK);
        CHECK(lrv_vdec_read(&d, pcm_out, 2) == 2 && pcm_out[0] == 256 && pcm_out[1] == 32512);
        clip[LRV_VDEC_PCM8_HEADER] = 0x80;
        lrv_vdec_close(&d);
        lrv_pack_close(&pk);
        CHECK(th_live == 0);
    }
    fail_window = 0;

    /* fewer samples than the entry holds: the count rules; more: not a clip */
    lrv_wr32le(clip + 8, 100);
    e = clip_pack(&pk, &hal, LRV_VDEC_PCM8_HEADER + N);
    lrv_vdec_init(&d);
    CHECK(lrv_vdec_open(&d, &pk, e) == LRV_OK && read_all(&d, pcm_out, N) == 100);
    lrv_pack_close(&pk);
    lrv_wr32le(clip + 8, N + 1);
    e = clip_pack(&pk, &hal, LRV_VDEC_PCM8_HEADER + N);
    CHECK(lrv_vdec_open(&d, &pk, e) == LRV_ERR_FORMAT && d.kind == LRV_VDEC_NONE);
    CHECK(lrv_vdec_read(&d, pcm_out, 10) == 0);
    lrv_pack_close(&pk);
    lrv_wr32le(clip + 8, N);
    lrv_wr32le(clip + 4, 0);                    /* no rate */
    e = clip_pack(&pk, &hal, LRV_VDEC_PCM8_HEADER + N);
    CHECK(lrv_vdec_open(&d, &pk, e) == LRV_ERR_FORMAT);
    lrv_pack_close(&pk);
    /* what is no clip at all */
    memcpy(clip, "RIFF", 4);
    e = clip_pack(&pk, &hal, 64);
    CHECK(lrv_vdec_open(&d, &pk, e) == LRV_ERR_FORMAT);
    lrv_pack_close(&pk);
    e = clip_pack(&pk, &hal, 3);
    CHECK(lrv_vdec_open(&d, &pk, e) == LRV_ERR_FORMAT);
    lrv_pack_close(&pk);
    lrv_vdec_close(&d);
    CHECK(th_live == 0);

    CHECK(lrv_vdec_supports("pcm8") && lrv_vdec_supports("ldp4") && lrv_vdec_supports("ldp6"));
    CHECK(!lrv_vdec_supports("flac") && !lrv_vdec_supports(""));
#ifdef LRV_HAVE_OPUS
    CHECK(lrv_vdec_supports("opus"));
#else
    CHECK(!lrv_vdec_supports("opus"));
#endif
}

/* ---- ldp: an encoder for the test ----------------------------------------------------------- */

/* the decoder's step tables, written out again: the test must not share them with it */
static const int16_t steps4[16] = {
    0, 8, 26, 86, 283, 927, 3043, 9986, -32768, -9986, -3043, -927, -283, -86, -26, -8
};
static const int16_t steps6[64] = {
    0, 1, 2, 3, 4, 5, 6, 7, 10, 15, 20, 29, 40, 56, 78, 109,
    153, 214, 299, 419, 586, 819, 1145, 1601, 2240, 3132, 4380, 6126, 8567, 11980, 16754, 23431,
    -32768, -23431, -16754, -11980, -8567, -6126, -4380, -3132,
    -2240, -1601, -1145, -819, -586, -419, -299, -214,
    -153, -109, -78, -56, -40, -29, -20, -15, -10, -7, -6, -5, -4, -3, -2, -1
};

static struct {
    uint32_t pos;                       /* bytes of clip[] written */
    uint32_t acc;                       /* bits not yet written, MSB first */
    int nacc;
} bw;

static void put_bits(unsigned v, int bits)
{
    bw.acc = (bw.acc << bits) | v;
    bw.nacc += bits;
    while (bw.nacc >= 8) {
        bw.nacc -= 8;
        clip[bw.pos++] = (uint8_t)(bw.acc >> bw.nacc);
    }
}

static void flush_bits(void)
{
    if (bw.nacc)
        put_bits(0, 8 - bw.nacc);
    bw.acc = 0;
    bw.nacc = 0;
}

/* Writes a clip of `blocks` blocks of random codes and the samples they stand for. Every
 * third block announces fewer samples than its codes make (the rest of the block is skipped),
 * every fourth carries bytes after its codes. Returns the sample count. */
static int ldp_encode(int bits, int blocks)
{
    const int16_t *steps = bits == 4 ? steps4 : steps6;
    unsigned mask = (1u << bits) - 1;
    int total = 0, b;

    memcpy(clip, bits == 4 ? "ldp4" : "ldp6", 4);
    clip[4] = 0; clip[5] = 0; clip[6] = 0x5D; clip[7] = 0xC0;      /* 24000 Hz, big-endian */
    bw.pos = 12;
    bw.acc = 0;
    bw.nacc = 0;
    for (b = 0; b < blocks; b++) {
        uint32_t head = bw.pos, bytes;
        uint16_t acc = 0;
        int codes = 100 + (int)(rng() % 900), made = 0, said, k, i;
        bw.pos += 4;
        for (k = 0; k < codes; k++) {
            unsigned c = rng() % 10 == 0 ? 0 : 1 + rng() % mask;
            int run = 1;
            put_bits(c, bits);
            if (c) {
                acc = (uint16_t)(acc + (uint16_t)steps[c]);
            } else {
                unsigned n = rng() & mask;
                put_bits(n, bits);
                run = (int)n + 1;
            }
            for (i = 0; i < run; i++)
                pcm_want[total + made++] = (int16_t)acc;
        }
        flush_bits();
        if (b % 4 == 3) {
            clip[bw.pos++] = 0xFF;              /* not codes: the block's byte count skips them */
            clip[bw.pos++] = 0xFF;
            clip[bw.pos++] = 0xFF;
        }
        said = b % 3 == 2 ? made - made / 3 : made;
        bytes = bw.pos - head - 4;
        clip[head] = (uint8_t)(bytes >> 8);
        clip[head + 1] = (uint8_t)bytes;
        clip[head + 2] = (uint8_t)(said >> 8);
        clip[head + 3] = (uint8_t)said;
        total += said;
    }
    clip[8] = (uint8_t)(total >> 24);
    clip[9] = (uint8_t)(total >> 16);
    clip[10] = (uint8_t)(total >> 8);
    clip[11] = (uint8_t)total;
    return total;
}

static void test_ldp(void)
{
    struct lrv_hal hal;
    struct lrv_pack pk;
    struct lrv_vdec d;
    const struct lrv_pack_entry *e;
    int bits, total, n;

    clip_hal(&hal);
    lrv_vdec_init(&d);
    for (bits = 4; bits <= 6; bits += 2) {
        rng_state = (uint32_t)bits;
        total = ldp_encode(bits, bits == 4 ? 60 : 40);
        CHECK(total > 5000 && total < CLIP_MAX && bw.pos > LRV_VDEC_READAHEAD);
        e = clip_pack(&pk, &hal, bw.pos);
        CHECK(e != 0);
        CHECK(lrv_vdec_open(&d, &pk, e) == LRV_OK && d.kind == LRV_VDEC_LDP && d.rate == 24000);
        n = read_all(&d, pcm_out, CLIP_MAX);
        CHECK(n == total && !memcmp(pcm_out, pcm_want, sizeof pcm_out[0] * (size_t)total));
        CHECK(lrv_vdec_read(&d, pcm_out, 10) == 0);

        /* a sample at a time gives the same */
        CHECK(lrv_vdec_open(&d, &pk, e) == LRV_OK);
        for (n = 0; n < total && lrv_vdec_read(&d, pcm_out + n, 1) == 1; n++)
            ;
        CHECK(n == total && !memcmp(pcm_out, pcm_want, sizeof pcm_out[0] * (size_t)total));
        lrv_pack_close(&pk);

        /* the header's count ends the clip before its blocks do ... */
        clip[10] = 0x01; clip[11] = 0x00; clip[8] = clip[9] = 0;
        e = clip_pack(&pk, &hal, bw.pos);
        CHECK(lrv_vdec_open(&d, &pk, e) == LRV_OK && read_all(&d, pcm_out, CLIP_MAX) == 256);
        CHECK(!memcmp(pcm_out, pcm_want, sizeof pcm_out[0] * 256));
        lrv_pack_close(&pk);
        /* ... and a clip cut short ends with its data, without reading past it */
        clip[9] = 0x10;                         /* more than there is */
        e = clip_pack(&pk, &hal, bw.pos - bw.pos / 3);
        CHECK(lrv_vdec_open(&d, &pk, e) == LRV_OK);
        n = read_all(&d, pcm_out, CLIP_MAX);
        CHECK(n > 256 && n < total);
        lrv_pack_close(&pk);
    }
    /* another code width, no rate */
    memcpy(clip, "ldp5", 4);
    e = clip_pack(&pk, &hal, 64);
    CHECK(lrv_vdec_open(&d, &pk, e) == LRV_ERR_FORMAT);
    lrv_pack_close(&pk);
    memcpy(clip, "ldp6\0\0\0\0", 8);
    e = clip_pack(&pk, &hal, 64);
    CHECK(lrv_vdec_open(&d, &pk, e) == LRV_ERR_FORMAT);
    lrv_pack_close(&pk);
    lrv_vdec_close(&d);
    CHECK(th_live == 0);
}

/* ---- opus ----------------------------------------------------------------------------------- */

#ifdef LRV_HAVE_OPUS
/* Without an encoder the test cannot make speech, but it can make packets: a packet of one
 * byte is a frame without data, which the decoder turns into its frame's length of silence
 * (10 ms for the byte 0x00: 80 samples at 8 kHz). That exercises the container: the pre-skip,
 * the sample count, the packet lengths, the end of the entry. */
static uint32_t opus_clip(uint32_t rate, uint32_t count, unsigned skip, int packets)
{
    uint32_t pos = 16;
    int i;
    memset(clip, 0, 16);
    memcpy(clip, "opus", 4);
    clip[4] = (uint8_t)(rate >> 24); clip[5] = (uint8_t)(rate >> 16);
    clip[6] = (uint8_t)(rate >> 8); clip[7] = (uint8_t)rate;
    clip[8] = (uint8_t)(count >> 24); clip[9] = (uint8_t)(count >> 16);
    clip[10] = (uint8_t)(count >> 8); clip[11] = (uint8_t)count;
    clip[12] = (uint8_t)(skip >> 8); clip[13] = (uint8_t)skip;
    for (i = 0; i < packets; i++) {
        clip[pos++] = 0;
        clip[pos++] = 1;
        clip[pos++] = 0x00;
    }
    return pos;
}

static void test_opus(void)
{
    struct lrv_hal hal;
    struct lrv_pack pk;
    struct lrv_vdec d;
    const struct lrv_pack_entry *e;
    uint32_t size;
    int n, i, ok;

    clip_hal(&hal);
    lrv_vdec_init(&d);
    /* 5 packets of 80 samples, 30 skipped: 370 are there, the clip says 300 */
    size = opus_clip(8000, 300, 30, 5);
    e = clip_pack(&pk, &hal, size);
    CHECK(lrv_vdec_open(&d, &pk, e) == LRV_OK && d.kind == LRV_VDEC_OPUS && d.rate == 8000);
    memset(pcm_out, 0x55, sizeof pcm_out);
    n = read_all(&d, pcm_out, CLIP_MAX);
    for (i = 0, ok = n == 300; i < 300 && ok; i++)
        ok = pcm_out[i] == 0;
    CHECK(ok && pcm_out[300] == 0x5555);
    lrv_pack_close(&pk);
    /* the clip says more than its packets hold: it ends with them; the decoder's state is
     * the same one, at another rate */
    size = opus_clip(16000, 5000, 100, 4);      /* 4 x 160 - 100 */
    e = clip_pack(&pk, &hal, size);
    CHECK(lrv_vdec_open(&d, &pk, e) == LRV_OK && d.rate == 16000);
    CHECK(read_all(&d, pcm_out, CLIP_MAX) == 540);
    lrv_pack_close(&pk);
    /* a packet that is longer than the entry, a rate Opus does not have, one too high */
    size = opus_clip(8000, 300, 0, 2);
    clip[size - 2] = 9;
    e = clip_pack(&pk, &hal, size);
    CHECK(lrv_vdec_open(&d, &pk, e) == LRV_OK && read_all(&d, pcm_out, CLIP_MAX) == 80);
    lrv_pack_close(&pk);
    size = opus_clip(11025, 300, 0, 2);
    e = clip_pack(&pk, &hal, size);
    CHECK(lrv_vdec_open(&d, &pk, e) == LRV_ERR_FORMAT);
    lrv_pack_close(&pk);
    size = opus_clip(48000, 300, 0, 2);
    e = clip_pack(&pk, &hal, size);
    CHECK(lrv_vdec_open(&d, &pk, e) == LRV_ERR_FORMAT);
    lrv_pack_close(&pk);
    lrv_vdec_close(&d);
    CHECK(th_live == 0);
}
#endif

int main(void)
{
    test_lzss();
    test_pcm8();
    test_ldp();
#ifdef LRV_HAVE_OPUS
    test_opus();
#endif
    if (th_fails) {
        fprintf(stderr, "test_codec: %d checks failed\n", th_fails);
        return 1;
    }
    printf("test_codec: ok\n");
    return 0;
}
