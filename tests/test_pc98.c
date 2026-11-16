/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests of the PC-98 module: the video mode and the translation of neutral keys into
 * PC-98 scan codes, BIOS characters and the key-down bitmap. The expected values are a table
 * written out key by key, independent of the tables in the module. */
#include "test_hal.h"
#include "lrvne/machine.h"
#include "lrvne/pc98.h"

#define NONE LRV_PC98_K_NONE

/* every neutral key: its scan code, and the character of a press unshifted and shifted (no
 * locks). Keys not listed are not PC-98 keys. */
static const struct expect {
    int key;
    uint8_t scan, chr, chr_shift;
} expect[] = {
    { LRV_K_A, 0x1D, 'a', 'A' }, { LRV_K_B, 0x2D, 'b', 'B' }, { LRV_K_C, 0x2B, 'c', 'C' },
    { LRV_K_D, 0x1F, 'd', 'D' }, { LRV_K_E, 0x12, 'e', 'E' }, { LRV_K_F, 0x20, 'f', 'F' },
    { LRV_K_G, 0x21, 'g', 'G' }, { LRV_K_H, 0x22, 'h', 'H' }, { LRV_K_I, 0x17, 'i', 'I' },
    { LRV_K_J, 0x23, 'j', 'J' }, { LRV_K_K, 0x24, 'k', 'K' }, { LRV_K_L, 0x25, 'l', 'L' },
    { LRV_K_M, 0x2F, 'm', 'M' }, { LRV_K_N, 0x2E, 'n', 'N' }, { LRV_K_O, 0x18, 'o', 'O' },
    { LRV_K_P, 0x19, 'p', 'P' }, { LRV_K_Q, 0x10, 'q', 'Q' }, { LRV_K_R, 0x13, 'r', 'R' },
    { LRV_K_S, 0x1E, 's', 'S' }, { LRV_K_T, 0x14, 't', 'T' }, { LRV_K_U, 0x16, 'u', 'U' },
    { LRV_K_V, 0x2C, 'v', 'V' }, { LRV_K_W, 0x11, 'w', 'W' }, { LRV_K_X, 0x2A, 'x', 'X' },
    { LRV_K_Y, 0x15, 'y', 'Y' }, { LRV_K_Z, 0x29, 'z', 'Z' },
    { LRV_K_1, 0x01, '1', 0 }, { LRV_K_2, 0x02, '2', 0 }, { LRV_K_3, 0x03, '3', 0 },
    { LRV_K_4, 0x04, '4', 0 }, { LRV_K_5, 0x05, '5', 0 }, { LRV_K_6, 0x06, '6', 0 },
    { LRV_K_7, 0x07, '7', 0 }, { LRV_K_8, 0x08, '8', 0 }, { LRV_K_9, 0x09, '9', 0 },
    { LRV_K_0, 0x0A, '0', 0 },
    { LRV_K_RETURN, 0x1C, 0x0D, 0x0D }, { LRV_K_ESC, 0x00, 0x1B, 0x1B },
    { LRV_K_BS, 0x0E, 0x08, 0x08 }, { LRV_K_TAB, 0x0F, 0x09, 0x09 },
    { LRV_K_SPACE, 0x34, 0x20, 0x20 },
    { LRV_K_MINUS, 0x0B, '-', '-' }, { LRV_K_EQUALS, 0x0C, 0, 0 },
    { LRV_K_LBRACKET, 0x1A, 0, 0 }, { LRV_K_RBRACKET, 0x1B, 0, 0 },
    { LRV_K_BACKSLASH, 0x28, 0, 0 }, { LRV_K_SEMICOLON, 0x26, 0, 0 },
    { LRV_K_APOSTROPHE, 0x27, 0, 0 }, { LRV_K_GRAVE, NONE, 0, 0 },
    { LRV_K_COMMA, 0x30, ',', ',' }, { LRV_K_PERIOD, 0x31, '.', '.' },
    { LRV_K_SLASH, 0x32, '/', '/' }, { LRV_K_YEN, 0x0D, 0, 0 }, { LRV_K_RO, 0x33, 0, 0 },
    { LRV_K_F1, 0x62, 0, 0 }, { LRV_K_F2, 0x63, 0, 0 }, { LRV_K_F3, 0x64, 0, 0 },
    { LRV_K_F4, 0x65, 0, 0 }, { LRV_K_F5, 0x66, 0, 0 }, { LRV_K_F6, 0x67, 0, 0 },
    { LRV_K_F7, 0x68, 0, 0 }, { LRV_K_F8, 0x69, 0, 0 }, { LRV_K_F9, 0x6A, 0, 0 },
    { LRV_K_F10, 0x6B, 0, 0 }, { LRV_K_F11, NONE, 0, 0 }, { LRV_K_F12, NONE, 0, 0 },
    { LRV_K_F13, NONE, 0, 0 }, { LRV_K_F14, NONE, 0, 0 }, { LRV_K_F15, NONE, 0, 0 },
    { LRV_K_INS, 0x38, 0, 0 }, { LRV_K_DEL, 0x39, 0x7F, 0x7F }, { LRV_K_HOME, 0x3E, 0, 0 },
    { LRV_K_END, 0x3F, 0, 0 }, { LRV_K_PGUP, 0x37, 0, 0 }, { LRV_K_PGDN, 0x36, 0, 0 },
    { LRV_K_UP, 0x3A, 0, 0 }, { LRV_K_DOWN, 0x3D, 0, 0 }, { LRV_K_LEFT, 0x3B, 0, 0 },
    { LRV_K_RIGHT, 0x3C, 0, 0 }, { LRV_K_PAUSE, 0x60, 0, 0 }, { LRV_K_HELP, 0x3F, 0, 0 },
    { LRV_K_KP0, 0x4E, '0', '0' }, { LRV_K_KP1, 0x4A, '1', '1' }, { LRV_K_KP2, 0x4B, '2', '2' },
    { LRV_K_KP3, 0x4C, '3', '3' }, { LRV_K_KP4, 0x46, '4', '4' }, { LRV_K_KP5, 0x47, '5', '5' },
    { LRV_K_KP6, 0x48, '6', '6' }, { LRV_K_KP7, 0x42, '7', '7' }, { LRV_K_KP8, 0x43, '8', '8' },
    { LRV_K_KP9, 0x44, '9', '9' }, { LRV_K_KP_ENTER, 0x1C, 0x0D, 0x0D },
    { LRV_K_KP_PLUS, NONE, 0, 0 }, { LRV_K_KP_MINUS, NONE, 0, 0 }, { LRV_K_KP_MUL, NONE, 0, 0 },
    { LRV_K_KP_DIV, NONE, 0, 0 }, { LRV_K_KP_PERIOD, NONE, 0, 0 },
    { LRV_K_SHIFT, 0x70, 0, 0 }, { LRV_K_CTRL, 0x74, 0, 0 }, { LRV_K_ALT, 0x73, 0, 0 },
    { LRV_K_CAPS, 0x71, 0, 0 }, { LRV_K_KANA, 0x72, 0, 0 }
};
#define NEXPECT ((int)(sizeof expect / sizeof expect[0]))

static void test_mode(void)
{
    const struct lrv_video_mode *m = &lrv_pc98_video_mode;
    CHECK(m->format == LRV_FMT_PLANAR4 && m->w == 640 && m->h == 400);
    CHECK(m->aspect_w == 4 && m->aspect_h == 3 && m->frame_ns == LRV_PC98_FRAME_NS);
    {
        /* the refresh is 56.42 Hz = 5642 / 100: 5642 periods are 100 s to within half a
         * nanosecond a period, so sound paced by the frames keeps the machine's time */
        int64_t off = (int64_t)m->frame_ns * 5642 - (int64_t)100 * 1000000000;
        CHECK(off > -2821 && off < 2821);
    }
    CHECK(m->colours == 16 && m->rgb_bits == 4);
    CHECK(LRV_PC98_STRIDE * 8 == LRV_PC98_W);
    /* by name, for a backend without an engine */
    CHECK(lrv_video_mode_by_name("pc98") == m && lrv_video_mode_by_name("PC98") == m);
    CHECK(!lrv_video_mode_by_name("pc9") && !lrv_video_mode_by_name("pc988"));
    CHECK(!lrv_video_mode_by_name("") && !strcmp(lrv_video_mode_names, "pc98"));
}

static void test_scan(void)
{
    uint8_t seen[LRV_K_COUNT];
    int i;

    /* the table names every neutral key exactly once */
    memset(seen, 0, sizeof seen);
    for (i = 0; i < NEXPECT; i++)
        seen[expect[i].key]++;
    for (i = LRV_K_NONE + 1; i < LRV_K_COUNT; i++)
        CHECK(seen[i] == 1);

    for (i = 0; i < NEXPECT; i++) {
        const struct expect *e = &expect[i];
        struct lrv_keypress k;
        struct lrv_pc98_key out;
        int is_press = e->scan != NONE && e->scan < LRV_PC98_K_SHIFT;

        CHECK(lrv_pc98_scan(e->key, 0) == e->scan);
        CHECK(lrv_pc98_char(e->key, 0) == e->chr);
        CHECK(lrv_pc98_char(e->key, LRV_MOD_SHIFT) == e->chr_shift);

        /* a press: the pair, or nothing for what is not a key or is a shift key. The US
         * layout's character in the neutral press plays no part */
        memset(&k, 0, sizeof k);
        memset(&out, 0xEE, sizeof out);
        k.key = (uint8_t)e->key;
        k.chr = '?';
        CHECK(lrv_pc98_keypress(&k, 0, &out) == is_press);
        if (is_press)
            CHECK(out.scan == e->scan && out.chr == e->chr);
        else
            CHECK(out.scan == 0xEE && out.chr == 0xEE);
        k.mods = LRV_MOD_SHIFT;
        if (lrv_pc98_keypress(&k, 0, &out))
            CHECK(out.scan == e->scan && out.chr == e->chr_shift);
    }
    CHECK(lrv_pc98_scan(LRV_K_NONE, 0) == NONE && lrv_pc98_scan(LRV_K_COUNT, 0) == NONE);
    CHECK(lrv_pc98_scan(-1, 0) == NONE && lrv_pc98_scan(1000, 0) == NONE);

    /* letters: shift and caps lock cancel; the other modifiers change nothing */
    CHECK(lrv_pc98_char(LRV_K_Q, LRV_MOD_CAPS) == 'Q');
    CHECK(lrv_pc98_char(LRV_K_Q, LRV_MOD_CAPS | LRV_MOD_SHIFT) == 'q');
    CHECK(lrv_pc98_char(LRV_K_Q, LRV_MOD_CTRL | LRV_MOD_ALT) == 'q');
    CHECK(lrv_pc98_char(LRV_K_5, LRV_MOD_CAPS) == '5');
}

static void test_cursor_option(void)
{
    static const int keys[4] = { LRV_K_UP, LRV_K_LEFT, LRV_K_RIGHT, LRV_K_DOWN };
    static const uint8_t own[4] = { 0x3A, 0x3B, 0x3C, 0x3D }, pad[4] = { 0x43, 0x46, 0x48, 0x4B };
    struct lrv_keypress k;
    struct lrv_pc98_key out;
    int i;

    for (i = 0; i < 4; i++) {
        CHECK(lrv_pc98_scan(keys[i], 0) == own[i]);
        CHECK(lrv_pc98_scan(keys[i], LRV_PC98_CURSOR_KEYPAD) == pad[i]);
        memset(&k, 0, sizeof k);
        k.key = (uint8_t)keys[i];
        CHECK(lrv_pc98_keypress(&k, LRV_PC98_CURSOR_KEYPAD, &out) && out.scan == pad[i] &&
              out.chr == 0);                    /* the keypad's scan code, not its digit */
    }
    /* nothing else is touched by the option */
    for (i = 0; i < NEXPECT; i++)
        if (expect[i].key != LRV_K_UP && expect[i].key != LRV_K_LEFT &&
            expect[i].key != LRV_K_RIGHT && expect[i].key != LRV_K_DOWN)
            CHECK(lrv_pc98_scan(expect[i].key, LRV_PC98_CURSOR_KEYPAD) == expect[i].scan);
}

static int press(int key, unsigned mods, struct lrv_pc98_key *out)
{
    struct lrv_keypress k;
    memset(&k, 0, sizeof k);
    k.key = (uint8_t)key;
    k.mods = (uint8_t)mods;
    return lrv_pc98_keypress(&k, 0, out);
}

static void test_kana(void)
{
    struct lrv_pc98_key out;

    /* the JIS kana layout by position, unshifted and shifted */
    CHECK(press(LRV_K_A, LRV_MOD_KANA, &out) && out.scan == 0x1D && out.chr == 0xC1);   /* chi */
    CHECK(press(LRV_K_Q, LRV_MOD_KANA, &out) && out.chr == 0xC0);                       /* ta */
    CHECK(press(LRV_K_3, LRV_MOD_KANA, &out) && out.chr == 0xB1);                       /* a */
    CHECK(press(LRV_K_3, LRV_MOD_KANA | LRV_MOD_SHIFT, &out) && out.chr == 0xA7);  /* small a */
    CHECK(press(LRV_K_0, LRV_MOD_KANA, &out) && out.chr == 0xDC);                       /* wa */
    CHECK(press(LRV_K_0, LRV_MOD_KANA | LRV_MOD_SHIFT, &out) && out.chr == 0xA6);       /* wo */
    CHECK(press(LRV_K_LBRACKET, LRV_MOD_KANA, &out) && out.chr == 0xDE);    /* voiced mark */
    CHECK(press(LRV_K_RBRACKET, LRV_MOD_KANA, &out) && out.chr == 0xDF);
    CHECK(press(LRV_K_RBRACKET, LRV_MOD_KANA | LRV_MOD_SHIFT, &out) && out.chr == 0xA2);
    CHECK(press(LRV_K_YEN, LRV_MOD_KANA, &out) && out.scan == 0x0D && out.chr == 0xB0);
    CHECK(press(LRV_K_RO, LRV_MOD_KANA, &out) && out.scan == 0x33 && out.chr == 0xDB);
    CHECK(press(LRV_K_SLASH, LRV_MOD_KANA | LRV_MOD_SHIFT, &out) && out.chr == 0xA5);
    /* keys the layout has nothing for keep their own character */
    CHECK(press(LRV_K_ESC, LRV_MOD_KANA, &out) && out.chr == 0x1B);
    CHECK(press(LRV_K_BS, LRV_MOD_KANA, &out) && out.chr == 0x08);
    CHECK(press(LRV_K_TAB, LRV_MOD_KANA, &out) && out.chr == 0x09);
    CHECK(press(LRV_K_RETURN, LRV_MOD_KANA, &out) && out.chr == 0x0D);
    CHECK(press(LRV_K_SPACE, LRV_MOD_KANA, &out) && out.chr == 0x20);
    CHECK(press(LRV_K_KP5, LRV_MOD_KANA, &out) && out.chr == '5');
    /* and without the lock nothing is kana */
    CHECK(press(LRV_K_A, 0, &out) && out.chr == 'a');
}

static void test_keymap(void)
{
    struct lrv_input in;
    uint8_t map[LRV_PC98_KEYMAP_BYTES];
    int i, n;

    memset(&in, 0, sizeof in);
    memset(map, 0xEE, sizeof map);
    lrv_pc98_keymap(&in, 0, map);
    for (i = 0; i < LRV_PC98_KEYMAP_BYTES; i++)
        CHECK(map[i] == 0);

    /* held keys at their scan codes; keys the PC-98 does not have leave no bit */
    lrv_key_set(&in, LRV_K_Z, 1);
    lrv_key_set(&in, LRV_K_ESC, 1);
    lrv_key_set(&in, LRV_K_SHIFT, 1);
    lrv_key_set(&in, LRV_K_UP, 1);
    lrv_key_set(&in, LRV_K_F12, 1);
    lrv_key_set(&in, LRV_K_GRAVE, 1);
    lrv_key_set(&in, LRV_K_KANA, 1);
    lrv_pc98_keymap(&in, 0, map);
    CHECK(lrv_pc98_key_down(map, LRV_PC98_K_Z) && lrv_pc98_key_down(map, LRV_PC98_K_ESC));
    CHECK(lrv_pc98_key_down(map, LRV_PC98_K_SHIFT) && lrv_pc98_key_down(map, LRV_PC98_K_UP));
    CHECK(!lrv_pc98_key_down(map, LRV_PC98_K_KP8) && lrv_pc98_key_down(map, LRV_PC98_K_KANA));
    for (i = 0, n = 0; i < 128; i++)
        n += lrv_pc98_key_down(map, i);
    CHECK(n == 5);

    /* the option moves the cursor keys in the bitmap too */
    lrv_pc98_keymap(&in, LRV_PC98_CURSOR_KEYPAD, map);
    CHECK(lrv_pc98_key_down(map, LRV_PC98_K_KP8) && !lrv_pc98_key_down(map, LRV_PC98_K_UP));

    /* CAPS and KANA are locks: down while on, whether or not a key is held */
    memset(&in, 0, sizeof in);
    in.mods = LRV_MOD_CAPS | LRV_MOD_KANA | LRV_MOD_SHIFT;
    lrv_pc98_keymap(&in, 0, map);
    CHECK(lrv_pc98_key_down(map, LRV_PC98_K_CAPS) && lrv_pc98_key_down(map, LRV_PC98_K_KANA));
    CHECK(!lrv_pc98_key_down(map, LRV_PC98_K_SHIFT));   /* a level, not a held key */
    in.mods = 0;
    lrv_key_set(&in, LRV_K_CAPS, 1);                    /* the lock as the bitmap has it */
    lrv_pc98_keymap(&in, 0, map);
    CHECK(lrv_pc98_key_down(map, LRV_PC98_K_CAPS) && !lrv_pc98_key_down(map, LRV_PC98_K_KANA));

    /* several host keys of one PC-98 key */
    memset(&in, 0, sizeof in);
    lrv_key_set(&in, LRV_K_KP_ENTER, 1);
    lrv_key_set(&in, LRV_K_END, 1);
    lrv_pc98_keymap(&in, 0, map);
    CHECK(lrv_pc98_key_down(map, LRV_PC98_K_RETURN) && lrv_pc98_key_down(map, LRV_PC98_K_HELP));
}

int main(void)
{
    test_mode();
    test_scan();
    test_cursor_option();
    test_kana();
    test_keymap();
    if (th_fails) {
        fprintf(stderr, "test_pc98: %d failed\n", th_fails);
        return 1;
    }
    printf("test_pc98: ok\n");
    return 0;
}
