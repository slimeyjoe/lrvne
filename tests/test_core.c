/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests of the core: the pack reader, the profile parser, the frame loop and input. */
#include "test_hal.h"
#include "lrvne/profile.h"
#include "lrvne/sys.h"

/* ---- pack -------------------------------------------------------------------------------- */

static void test_pack(void)
{
    static struct th_builder b;
    static uint8_t bundled[9000];
    struct lrv_hal hal;
    struct lrv_pack pk;
    const struct lrv_pack_entry *e;
    uint8_t *data, part[4];
    uint32_t size, n;

    th_hal(&hal);
    th_pack_begin(&b, 5);
    th_pack_add(&b, "langs", LRV_PT_META, 0, "1=ja\n2=en\r\n", 11);
    th_pack_add(&b, "PIC.GDT", LRV_PT_ENGINE, 0, "picture", 7);
    th_pack_add(&b, "TEXT", LRV_PT_STRINGS, 1, "nihongo", 7);
    th_pack_add(&b, "TEXT", LRV_PT_STRINGS, 2, "english", 7);
    th_pack_add(&b, "a-twenty-char-name-x", LRV_PT_BLOB, 0, "", 0);
    n = th_pack_end(&b, LRV_PACK_MAIN);

    CHECK(lrv_pack_open(&pk, &hal, LRV_PACK_MAIN) == LRV_OK);
    CHECK(pk.count == 5);
    CHECK(lrv_pack_lang_id(&pk, "JA") == 1 && lrv_pack_lang_id(&pk, "en") == 2);
    CHECK(lrv_pack_lang_id(&pk, "pl") == 0);
    CHECK(!strcmp(lrv_pack_lang_tag(&pk, 2), "en") && !lrv_pack_lang_tag(&pk, 3));

    /* names: case ignored, type respected, the full 20 characters */
    e = lrv_pack_find(&pk, "pic.gdt", LRV_PT_ENGINE);
    CHECK(e && e->size == 7);
    CHECK(!lrv_pack_find(&pk, "pic.gdt", LRV_PT_BLOB));
    CHECK(lrv_pack_find(&pk, "pic.gdt", LRV_PT_ANY) == e);
    CHECK(lrv_pack_find(&pk, "A-TWENTY-CHAR-NAME-X", LRV_PT_BLOB) != 0);
    CHECK(!lrv_pack_find(&pk, "a-twenty-char-name-xy", LRV_PT_BLOB));

    /* languages: nothing language-specific without one, then the chosen one */
    CHECK(!lrv_pack_find(&pk, "text", LRV_PT_STRINGS));
    lrv_pack_set_lang(&pk, 2);
    e = lrv_pack_find(&pk, "text", LRV_PT_STRINGS);
    data = e ? lrv_pack_load(&pk, e, &size) : 0;
    CHECK(data && size == 7 && !strcmp((char *)data, "english"));
    if (data)
        lrv_pack_free(&pk, data, size);
    CHECK(lrv_pack_find(&pk, "pic.gdt", LRV_PT_ENGINE) != 0);     /* neutral still found */

    /* partial reads stop at the entry's end */
    CHECK(lrv_pack_read(&pk, e, 5, part, 4) == 2 && part[0] == 's' && part[1] == 'h');
    CHECK(lrv_pack_read(&pk, e, 7, part, 4) == 0);
    lrv_pack_close(&pk);
    CHECK(th_live == 0);

    /* the same pack appended to an "executable" */
    memset(bundled, 0xEE, 100);
    memcpy(bundled + 100, b.buf, n);
    memcpy(bundled + 100 + n, "XPKT", 4);
    lrv_wr32le(bundled + 104 + n, n);
    lrv_wr32le(bundled + 108 + n, ~n);
    memcpy(bundled + 112 + n, "XPKT", 4);
    th_pack[LRV_PACK_MAIN] = bundled;
    th_pack_size[LRV_PACK_MAIN] = 116 + n;
    CHECK(lrv_pack_open(&pk, &hal, LRV_PACK_MAIN) == LRV_OK && pk.base == 100);
    e = lrv_pack_find(&pk, "PIC.GDT", LRV_PT_ENGINE);
    CHECK(e && lrv_pack_read(&pk, e, 0, part, 4) == 4 && !memcmp(part, "pict", 4));
    lrv_pack_close(&pk);

    /* damage is refused, with a message and without a leak */
    th_errors = 0;
    lrv_wr32le(bundled + 108 + n, n);                           /* trailer check word */
    CHECK(lrv_pack_open(&pk, &hal, LRV_PACK_MAIN) == LRV_ERR_FORMAT && th_errors == 1);
    th_pack[LRV_PACK_MAIN] = b.buf;
    th_pack_size[LRV_PACK_MAIN] = n - 8;                        /* truncated file */
    CHECK(lrv_pack_open(&pk, &hal, LRV_PACK_MAIN) == LRV_ERR_FORMAT);
    th_pack_size[LRV_PACK_MAIN] = n;
    lrv_wr32le(b.buf + LRV_PACK_HEADER_SIZE + LRV_PACK_ENTRY_SIZE + 28, n);  /* entry past the end */
    CHECK(lrv_pack_open(&pk, &hal, LRV_PACK_MAIN) == LRV_ERR_FORMAT);
    b.buf[4] = 1;                                               /* an old version */
    CHECK(lrv_pack_open(&pk, &hal, LRV_PACK_MAIN) == LRV_ERR_FORMAT);
    CHECK(th_live == 0);

    /* a voice pack that is not there is not an error to report */
    th_errors = 0;
    CHECK(lrv_pack_open(&pk, &hal, LRV_PACK_VOICE) == LRV_ERR_NOTFOUND && th_errors == 0);
}

/* ---- profile ------------------------------------------------------------------------------ */

struct game_profile {
    struct lrv_profile_base base;
    char start[16];
    int speed;
    int voice;
    int rows[3];
    int nrows;
};

static const struct lrv_pfield fields[] = {
    LRV_PROFILE_BASE_FIELDS(struct game_profile, base),
    LRV_PF_STR("start", struct game_profile, start),
    LRV_PF_INT("speed", struct game_profile, speed, 1, 100000),
    LRV_PF_BOOL("voice", struct game_profile, voice),
    LRV_PF_INTS("rows", struct game_profile, rows, nrows, 0, 399)
};
#define NFIELDS ((int)(sizeof fields / sizeof fields[0]))

static int perr_count, perr_line;
static char perr_key[64], perr_what[64];

static void perr(void *ctx, int line, const char *key, const char *what)
{
    (void)ctx;
    perr_count++;
    perr_line = line;
    snprintf(perr_key, sizeof perr_key, "%s", key);
    snprintf(perr_what, sizeof perr_what, "%s", what);
}

static int parse(struct game_profile *p, const char *text)
{
    perr_count = 0;
    return lrv_profile_parse((const uint8_t *)text, strlen(text), fields, NFIELDS, p, perr, 0);
}

static void test_profile(void)
{
    struct game_profile p;

    memset(&p, 0, sizeof p);
    p.speed = 7;
    CHECK(parse(&p, "# a game\n"
                    "id=demo\r\n"
                    "title = A Title = with an equals sign \n"
                    "\n"
                    "start=RUN\n"
                    "voice=Yes\n"
                    "rows=176, 224,0x110\n"
                    "modules=logo, title ,credits") == 0);
    CHECK(!strcmp(p.base.id, "demo"));
    CHECK(!strcmp(p.base.title, "A Title = with an equals sign"));
    CHECK(!strcmp(p.start, "RUN") && p.voice == 1 && p.speed == 7);
    CHECK(p.nrows == 3 && p.rows[0] == 176 && p.rows[1] == 224 && p.rows[2] == 272);
    CHECK(lrv_list_has(p.base.modules, "logo") && lrv_list_has(p.base.modules, "title"));
    CHECK(lrv_list_has(p.base.modules, "credits") && !lrv_list_has(p.base.modules, "cred"));
    CHECK(!lrv_list_has("", "x") && !lrv_list_has("a,,b", ""));

    /* every unusable line is reported and leaves its field alone */
    CHECK(parse(&p, "speed=fast\n") == 1 && !strcmp(perr_what, "not a number") && p.speed == 7);
    CHECK(parse(&p, "speed=0\n") == 1 && !strcmp(perr_what, "number out of range"));
    CHECK(parse(&p, "\nspead=3\n") == 1 && perr_line == 2 && !strcmp(perr_key, "spead"));
    CHECK(parse(&p, "start=A-NAME-THAT-IS-TOO-LONG\n") == 1 && !strcmp(p.start, "RUN"));
    CHECK(parse(&p, "voice=maybe\n") == 1);
    CHECK(parse(&p, "rows=1,2,3,4\n") == 1 && p.nrows == 3);
    CHECK(parse(&p, "rows=1,,3\n") == 1);
    CHECK(parse(&p, "just a word\n") == 1 && !strcmp(perr_what, "no '='"));
    CHECK(parse(&p, "speed=-0x10\n") == 1);
    CHECK(parse(&p, "speed=0x10\nrows=\n") == 0 && p.speed == 16 && p.nrows == 0);
}

static void test_profile_pack(void)
{
    static struct th_builder b;
    struct lrv_hal hal;
    struct lrv_pack pk;
    struct game_profile p;

    th_hal(&hal);
    memset(&p, 0, sizeof p);
    th_pack_begin(&b, 1);
    th_pack_add(&b, "profile", LRV_PT_META, 0, "id=demo\nspeed=9\n", 16);
    th_pack_end(&b, LRV_PACK_MAIN);
    CHECK(lrv_pack_open(&pk, &hal, LRV_PACK_MAIN) == LRV_OK);
    CHECK(lrv_profile_load(&pk, fields, NFIELDS, &p) == LRV_OK && p.speed == 9);
    lrv_pack_close(&pk);

    th_pack_begin(&b, 1);
    th_pack_add(&b, "profile", LRV_PT_META, 0, "id=demo\nsped=9\n", 15);
    th_pack_end(&b, LRV_PACK_MAIN);
    th_errors = 0;
    CHECK(lrv_pack_open(&pk, &hal, LRV_PACK_MAIN) == LRV_OK);
    CHECK(lrv_profile_load(&pk, fields, NFIELDS, &p) == LRV_ERR_FORMAT && th_errors == 1);
    CHECK(strstr(th_last_log, "line 2") && strstr(th_last_log, "sped"));
    lrv_pack_close(&pk);

    th_pack_begin(&b, 0);
    th_pack_end(&b, LRV_PACK_MAIN);
    CHECK(lrv_pack_open(&pk, &hal, LRV_PACK_MAIN) == LRV_OK);
    CHECK(lrv_profile_load(&pk, fields, NFIELDS, &p) == LRV_ERR_NOTFOUND);
    lrv_pack_close(&pk);
    CHECK(th_live == 0);
}

/* ---- the frame loop and input ------------------------------------------------------------- */

static char order[64];

static void hook_a(void *ctx) { strcat(order, (const char *)ctx); }
static void do_present(void *ctx) { (void)ctx; strcat(order, "P"); }

static void test_sys(void)
{
    struct lrv_hal hal;
    struct lrv_sys s;
    struct lrv_keypress k;
    int i;

    th_hal(&hal);
    memset(&th_next_input, 0, sizeof th_next_input);
    th_frames = 0;
    lrv_sys_init(&s, &hal, 640, 400);
    CHECK(s.mouse_x == 320 && s.mouse_y == 200);
    lrv_sys_set_present(&s, do_present, 0);
    CHECK(lrv_sys_add_hook(&s, LRV_HOOK_PUMP, hook_a, "a") == LRV_OK);
    CHECK(lrv_sys_add_hook(&s, LRV_HOOK_PUMP, hook_a, "b") == LRV_OK);
    CHECK(lrv_sys_add_hook(&s, LRV_HOOK_TICK, hook_a, "t") == LRV_OK);
    CHECK(lrv_sys_add_hook(&s, 7, hook_a, "x") == LRV_ERR_RANGE);

    /* one frame: pump hooks in order, present, wait, tick hooks */
    lrv_sys_frame_wait(&s);
    CHECK(!strcmp(order, "abPt") && s.frame == 1 && th_frames == 1);

    /* a press shorter than a poll: queued, and its key is down for this poll */
    th_next_input.nkeys = 2;
    th_next_input.keys[0].key = LRV_K_RETURN;
    th_next_input.keys[0].chr = '\r';
    th_next_input.keys[1].key = LRV_K_A;
    th_next_input.events = LRV_EV_MOUSE_L;
    th_next_input.mouse_dx = 1000;
    th_next_input.mouse_dy = -1000;
    lrv_sys_frame_wait(&s);
    CHECK(lrv_sys_key_sense(&s) && lrv_sys_key_down(&s, LRV_K_RETURN));
    CHECK(lrv_sys_key_read(&s, &k) && k.key == LRV_K_RETURN && k.chr == '\r');
    CHECK(lrv_sys_key_read(&s, &k) && k.key == LRV_K_A && !lrv_sys_key_read(&s, &k));
    CHECK(s.mouse_x == 639 && s.mouse_y == 0);                      /* clamped */
    CHECK((lrv_sys_mouse_buttons(&s) & LRV_BTN_L) != 0);
    CHECK(lrv_sys_mouse_pressed(&s, LRV_EV_MOUSE_L) && !lrv_sys_mouse_pressed(&s, LRV_EV_MOUSE_L));
    th_next_input.mouse_dx = th_next_input.mouse_dy = 0;
    lrv_sys_frame_wait(&s);
    CHECK(!lrv_sys_key_down(&s, LRV_K_RETURN) && !lrv_sys_mouse_buttons(&s));

    /* an absolute pointer wins only when it moved */
    th_next_input.mouse_abs = 1;
    th_next_input.mouse_x = 10;
    th_next_input.mouse_y = 20;
    lrv_sys_frame_wait(&s);
    CHECK(s.mouse_x == 10 && s.mouse_y == 20);
    lrv_sys_mouse_set(&s, 100, 100);
    lrv_sys_frame_wait(&s);
    CHECK(s.mouse_x == 100 && s.mouse_y == 100);

    /* the queue holds LRV_KEYQ presses and drops the rest; a flush empties it */
    for (i = 0; i < 3; i++) {
        th_next_input.nkeys = 8;
        lrv_sys_frame_wait(&s);
    }
    for (i = 0; lrv_sys_key_read(&s, &k); i++)
        ;
    CHECK(i == LRV_KEYQ);
    th_next_input.nkeys = 1;
    lrv_sys_frame_wait(&s);
    lrv_sys_key_flush(&s);
    CHECK(!lrv_sys_key_sense(&s));

    /* the CPU budget: 2.5 frames of work pass 2 frames and keep the rest */
    s.cpu_per_frame = 1000;
    th_frames = 0;
    lrv_sys_cpu(&s, 2500);
    CHECK(th_frames == 2 && s.cpu == 500);
    lrv_sys_frame_wait(&s);
    CHECK(s.cpu == 0);

    /* a reset unwinds like a quit, without blocking or presenting, and can be cleared */
    th_next_input.events = LRV_EV_RESET;
    lrv_sys_frame_wait(&s);
    CHECK(lrv_sys_quitting(&s) && lrv_sys_resetting(&s));
    th_frames = 0;
    order[0] = 0;
    lrv_sys_frame_wait(&s);
    lrv_sys_cpu(&s, 5000);
    CHECK(th_frames == 0 && !strcmp(order, "t"));
    lrv_sys_reset_done(&s);
    CHECK(!lrv_sys_quitting(&s));
    lrv_sys_request_quit(&s);
    CHECK(lrv_sys_quitting(&s) && !lrv_sys_resetting(&s));
    s.quit_requested = 0;
    th_next_input.events = LRV_EV_QUIT;
    lrv_sys_frame_wait(&s);
    CHECK(lrv_sys_quitting(&s) && !lrv_sys_resetting(&s));
}

static void test_keys(void)
{
    int k;
    for (k = 1; k < LRV_K_COUNT; k++) {
        const char *n = lrv_key_name(k);
        CHECK(n && lrv_key_by_name(n) == k);
    }
    CHECK(LRV_K_COUNT <= LRV_KEYMAP_BYTES * 8);
    CHECK(lrv_key_by_name("RETURN") == LRV_K_RETURN && lrv_key_by_name("nokey") == LRV_K_NONE);
    CHECK(!lrv_key_name(0) && !lrv_key_name(LRV_K_COUNT));
    CHECK(lrv_key_ascii(LRV_K_A, 0) == 'a' && lrv_key_ascii(LRV_K_A, LRV_MOD_SHIFT) == 'A');
    CHECK(lrv_key_ascii(LRV_K_Z, LRV_MOD_SHIFT | LRV_MOD_CAPS) == 'z');
    CHECK(lrv_key_ascii(LRV_K_1, 0) == '1' && lrv_key_ascii(LRV_K_0, 0) == '0');
    CHECK(lrv_key_ascii(LRV_K_2, LRV_MOD_SHIFT) == '@' && lrv_key_ascii(LRV_K_SLASH, 0) == '/');
    CHECK(lrv_key_ascii(LRV_K_SLASH, LRV_MOD_SHIFT) == '?' && lrv_key_ascii(LRV_K_KP7, 0) == '7');
    CHECK(lrv_key_ascii(LRV_K_F1, 0) == 0 && lrv_key_ascii(LRV_K_YEN, 0) == 0);
}

int main(void)
{
    test_pack();
    test_profile();
    test_profile_pack();
    test_sys();
    test_keys();
    if (th_fails) {
        fprintf(stderr, "test_core: %d failed\n", th_fails);
        return 1;
    }
    printf("test_core: ok\n");
    return 0;
}
