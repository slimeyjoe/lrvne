/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Where a program built on lrvne finds its packs and keeps its saves on AmigaDOS, and the pack
 * and storage functions of the HAL on dos.library files: the Amiga counterpart of
 * backends/common/paths.c. The rules are those of lrvne/app.h in AmigaDOS terms; nothing here
 * knows a program, a pack or a game by name.
 *
 *   pack      PAK=file; else the pack appended to the program file; else <program>.pak in
 *             PROGDIR: (<program> = the program file's own name, so a renamed copy of an
 *             engine finds its game), else the only *.pak there (voice packs aside); then the
 *             same in the current directory.
 *   voice     VOICEPAK=file; else <pack>_voice.pak beside the pack, <pack> being the pack's
 *             path without ".pak" (the program file's when the pack is appended to it).
 *             NOVOICE: none.
 *   storage   STORAGE=dir; else PROGDIR:saves/<game id>/; else, when that cannot be made (a
 *             CD, a write-protected disk), ENVARC:<app name>/<game id>/; else T:. A record is
 *             the file <key> there, written as <key>.tmp and renamed.
 *
 * PRELOAD: the whole main pack is read into memory at start when it fits with PRELOAD_RESERVE
 * to spare (auto), whenever it fits (yes) or never (no); otherwise each range is read from
 * the file when asked for. A voice pack is never preloaded (it is large and read in the order
 * of play). A read that continues where the last one ended skips the Seek: on FFS even a
 * short one costs milliseconds (README.md "Pack on FFS"). */
#include "os_begin.h"
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "os_end.h"
#include <stdio.h>
#include <string.h>
#include "amiga.h"
#include "lrvne/endian.h"
#include "lrvne/pack.h"
#include "flavour.h"

#define AM_PATH_MAX 256

/* Memory PRELOAD=auto leaves free after loading the pack: room for an engine's own peak
 * (its allocations, screen and audio buffers, libraries) plus a margin. */
#define PRELOAD_RESERVE (1536UL * 1024UL)

/* a pack source */
struct am_pak {
    BPTR fh;
    ULONG size;
    UBYTE *mem;                     /* the whole file in memory (PRELOAD), else 0 */
    ULONG pos;                      /* the file position after the last read, ~0 = unknown */
};

static struct am_paths_cfg g_cfg;
static struct am_pak g_pak, g_vpak;
static char g_pack[AM_PATH_MAX];            /* the main pack, once located */
static char g_storage[AM_PATH_MAX];         /* the storage directory, once opened: a prefix */

void am_paths_init(const struct am_paths_cfg *cfg)
{
    g_cfg = *cfg;
    g_pack[0] = 0;
    g_storage[0] = 0;
}

/* ---- paths -------------------------------------------------------------------------------- */

/* out = dir + name + suffix the AmigaDOS way (AddPart puts the separator); dir may be empty */
static int join(char *out, size_t cap, const char *dir, const char *name, const char *suffix)
{
    if (strlen(dir) + strlen(name) + strlen(suffix) + 2 > cap)
        return 0;
    strcpy(out, dir);
    if (!AddPart((STRPTR)out, (STRPTR)name, (ULONG)cap))
        return 0;
    strcat(out, suffix);
    return 1;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && am_strieq(s + n - m, suffix);
}

/* 1 when `name` can be a file name of its own: not empty, no path separators */
static int name_ok(const char *name)
{
    return name && *name && !strchr(name, '/') && !strchr(name, ':');
}

/* the program file's own name: from the Workbench's message, or the shell's command name */
static int program_name(char *out, size_t cap)
{
    char full[AM_PATH_MAX];
    const char *name = 0;

    if (am_wbmsg) {
        name = (const char *)am_wbmsg->sm_ArgList->wa_Name;
    } else if (GetProgramName((STRPTR)full, sizeof full)) {
        name = (const char *)FilePart((STRPTR)full);
    }
    if (!name || !*name || strlen(name) >= cap)
        return 0;
    strcpy(out, name);
    return 1;
}

static BPTR open_sized(const char *path, ULONG *size)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    LONG n;

    if (!fh)
        return 0;
    Seek(fh, 0, OFFSET_END);
    n = Seek(fh, 0, OFFSET_BEGINNING);
    if (n < 0) {
        Close(fh);
        return 0;
    }
    *size = (ULONG)n;
    return fh;
}

static int readable(const char *path)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return 0;
    Close(fh);
    return 1;
}

/* 1 when the file ends with the trailer of a bundled pack (lrvne/pack.h) */
static int has_pack(const char *path)
{
    UBYTE tr[LRV_PACK_TRAILER_SIZE];
    ULONG size;
    BPTR fh = open_sized(path, &size);
    int ok;

    if (!fh)
        return 0;
    ok = size >= sizeof tr && Seek(fh, (LONG)(size - sizeof tr), OFFSET_BEGINNING) >= 0 &&
         Read(fh, tr, sizeof tr) == (LONG)sizeof tr &&
         !memcmp(tr, "XPKT", 4) && !memcmp(tr + 12, "XPKT", 4) &&
         (lrv_rd32le(tr + 4) ^ lrv_rd32le(tr + 8)) == 0xFFFFFFFFUL;
    Close(fh);
    return ok;
}

/* <dir><program>.pak, or the only main pack in dir ("" = the current directory) */
static int find_in_dir(char *out, size_t cap, const char *dir, const char *program)
{
    char only[108];
    struct FileInfoBlock *fib;
    BPTR lock;
    int count = 0;

    if (program && *program && join(out, cap, dir, program, ".pak") && readable(out))
        return 1;
    lock = Lock((STRPTR)dir, SHARED_LOCK);
    if (!lock)
        return 0;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(lock, fib)) {
        while (ExNext(lock, fib)) {
            const char *name = (const char *)fib->fib_FileName;
            if (fib->fib_DirEntryType >= 0 || !ends_with(name, ".pak") ||
                ends_with(name, "_voice.pak") || strlen(name) >= sizeof only)
                continue;
            strcpy(only, name);
            count++;
        }
    }
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return count == 1 && join(out, cap, dir, only, "") && readable(out);
}

static int locate_main(void)
{
    char program[108];
    int have;

    if (g_pack[0])
        return 1;
    if (g_cfg.pak) {
        if (strlen(g_cfg.pak) >= sizeof g_pack)
            return 0;
        strcpy(g_pack, g_cfg.pak);
        return 1;
    }
    have = program_name(program, sizeof program);
    if (have && join(g_pack, sizeof g_pack, "PROGDIR:", program, "") && has_pack(g_pack))
        return 1;
    if (find_in_dir(g_pack, sizeof g_pack, "PROGDIR:", have ? program : 0) ||
        find_in_dir(g_pack, sizeof g_pack, "", have ? program : 0))
        return 1;
    g_pack[0] = 0;
    return 0;
}

/* the voice pack that belongs to a main pack: <pack>_voice.pak beside it */
static int voice_pack(char *out, size_t cap, const char *pack)
{
    size_t n = strlen(pack);
    if (ends_with(pack, ".pak"))
        n -= 4;
    if (n + sizeof "_voice.pak" > cap)
        return 0;
    memcpy(out, pack, n);
    strcpy(out + n, "_voice.pak");
    return 1;
}

/* ---- packs -------------------------------------------------------------------------------- */

static void preload(struct am_pak *pk)
{
    const char *o = am_opts.preload;
    int force = o && am_strieq(o, "yes");
    ULONG avail, big, got = 0, t0;

    if (o && am_strieq(o, "no"))
        return;
    avail = AvailMem(am_flavour.preload_memf);
    big = AvailMem(am_flavour.preload_memf | MEMF_LARGEST);
    if (big < pk->size || avail < pk->size + (force ? 0 : PRELOAD_RESERVE)) {
        am_log("pack: read from disk when needed (%lu KB free, %lu KB to preload)\n",
               (unsigned long)(avail >> 10),
               (unsigned long)((pk->size + (force ? 0 : PRELOAD_RESERVE)) >> 10));
        return;
    }
    pk->mem = AllocMem(pk->size, am_flavour.preload_memf);
    if (!pk->mem)
        return;
    t0 = am_prof_now();
    if (Seek(pk->fh, 0, OFFSET_BEGINNING) >= 0) {
        while (got < pk->size) {
            LONG n = Read(pk->fh, pk->mem + got, (LONG)(pk->size - got));
            if (n <= 0)
                break;
            got += (ULONG)n;
        }
    }
    if (got != pk->size) {
        am_log("pack: preload failed after %lu bytes, reading from disk\n", (unsigned long)got);
        FreeMem(pk->mem, pk->size);
        pk->mem = 0;
        pk->pos = ~(ULONG)0;
        return;
    }
    Close(pk->fh);
    pk->fh = 0;
    am_log("pack: %lu KB in memory, loaded in %lu ms\n", (unsigned long)(pk->size >> 10),
           (unsigned long)am_prof_ms(am_prof_now() - t0));
}

void *am_pack_open(int which, uint32_t *size)
{
    char path[AM_PATH_MAX];
    struct am_pak *pk;
    const char *name = 0;

    if (which == LRV_PACK_MAIN) {
        pk = &g_pak;
        if (locate_main())
            name = g_pack;
    } else if (which == LRV_PACK_VOICE && !g_cfg.no_voice) {
        pk = &g_vpak;
        if (g_cfg.voice_pak)
            name = g_cfg.voice_pak;
        else if (locate_main() && voice_pack(path, sizeof path, g_pack))
            name = path;
    } else {
        return 0;
    }
    if (!name)
        return 0;
    memset(pk, 0, sizeof *pk);
    pk->fh = open_sized(name, &pk->size);
    if (!pk->fh) {
        /* a file the options named and that cannot be opened deserves a word; a pack that
         * is simply not there is the caller's to report */
        if (which == LRV_PACK_MAIN ? g_cfg.pak != 0 : g_cfg.voice_pak != 0)
            am_log("%s: cannot open %s\n", am_app->name, name);
        return 0;
    }
    am_log("%s: %s\n", which == LRV_PACK_MAIN ? "pack" : "voice pack", name);
    pk->pos = 0;
    *size = pk->size;
    if (which == LRV_PACK_MAIN)
        preload(pk);
    return pk;
}

size_t am_pack_read(void *h, uint32_t off, void *dst, size_t n)
{
    struct am_pak *pk = h;
    LONG got;
    ULONG ts, t0 = am_prof_now();

    if (pk->mem) {
        if (off >= pk->size)
            return 0;
        if (n > pk->size - off)
            n = pk->size - off;
        memcpy(dst, pk->mem + off, n);
        am_prof_pack(t0, t0, (ULONG)off, (ULONG)n);
        return n;
    }
    if (!pk->fh || off > 0x7FFFFFFFUL)
        return 0;
    if (off != pk->pos && Seek(pk->fh, (LONG)off, OFFSET_BEGINNING) < 0) {
        pk->pos = ~(ULONG)0;
        return 0;
    }
    ts = am_prof_now();
    got = Read(pk->fh, dst, (LONG)n);
    pk->pos = got < 0 ? ~(ULONG)0 : off + (ULONG)got;
    am_prof_pack(t0, ts, (ULONG)off, got < 0 ? 0 : (ULONG)got);
    return got < 0 ? 0 : (size_t)got;
}

void am_pack_close(void *h)
{
    struct am_pak *pk = h;

    if (!pk)
        return;
    if (pk->mem)
        FreeMem(pk->mem, pk->size);
    else if (pk->fh)
        Close(pk->fh);
    pk->mem = 0;
    pk->fh = 0;
}

/* ---- storage ------------------------------------------------------------------------------ */

/* 1 when `path` is a directory that exists or could be made */
static int dir_usable(const char *path)
{
    BPTR lock = Lock((STRPTR)path, SHARED_LOCK);
    if (!lock)
        lock = CreateDir((STRPTR)path);
    if (!lock)
        return 0;
    UnLock(lock);
    return 1;
}

/* <parent><a>/<b>/ as a prefix in out, making both directories; 0 when it cannot be had */
static int storage_under(char *out, size_t cap, const char *parent, const char *a, const char *b)
{
    char dir[AM_PATH_MAX];

    if (!join(dir, sizeof dir, parent, a, "") || !dir_usable(dir) ||
        !join(out, cap, dir, b, "") || !dir_usable(out) || strlen(out) + 2 > cap)
        return 0;
    strcat(out, "/");
    return 1;
}

int am_storage_open(const char *game_id)
{
    g_storage[0] = 0;
    if (g_cfg.storage && *g_cfg.storage) {
        size_t n = strlen(g_cfg.storage);
        char last = g_cfg.storage[n - 1];
        if (n + 2 > sizeof g_storage)
            return LRV_ERR_RANGE;
        strcpy(g_storage, g_cfg.storage);
        /* a volume or an assign ("RAM:") is a prefix as it is; a directory gets its slash */
        if (last != ':' && last != '/') {
            if (!dir_usable(g_storage)) {
                g_storage[0] = 0;
                return LRV_ERR_IO;
            }
            strcat(g_storage, "/");
        }
    } else if (!name_ok(game_id) || !name_ok(am_app->name)) {
        return LRV_ERR_RANGE;
    } else if (!storage_under(g_storage, sizeof g_storage, "PROGDIR:", "saves", game_id) &&
               !storage_under(g_storage, sizeof g_storage, "ENVARC:", am_app->name, game_id)) {
        strcpy(g_storage, "T:");
    }
    am_log("storage: %s\n", g_storage);
    return LRV_OK;
}

int am_storage_read(const char *key, void *buf, size_t cap, size_t *len)
{
    char path[AM_PATH_MAX];
    BPTR fh;
    LONG n;

    if (!g_storage[0])
        return LRV_ERR;
    if (!name_ok(key) || strlen(g_storage) + strlen(key) + 1 > sizeof path)
        return LRV_ERR_RANGE;
    sprintf(path, "%s%s", g_storage, key);
    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return IoErr() == ERROR_OBJECT_NOT_FOUND ? LRV_ERR_NOTFOUND : LRV_ERR_IO;
    n = Read(fh, buf, (LONG)cap);
    Close(fh);
    if (n < 0)
        return LRV_ERR_IO;
    *len = (size_t)n;
    return LRV_OK;
}

int am_storage_write(const char *key, const void *buf, size_t len)
{
    char path[AM_PATH_MAX], tmp[AM_PATH_MAX];
    BPTR fh;
    LONG n;

    if (!g_storage[0])
        return LRV_ERR;
    if (!name_ok(key) || strlen(g_storage) + strlen(key) + 5 > sizeof path)
        return LRV_ERR_RANGE;
    sprintf(path, "%s%s", g_storage, key);
    sprintf(tmp, "%s%s.tmp", g_storage, key);
    fh = Open((STRPTR)tmp, MODE_NEWFILE);
    if (!fh)
        return LRV_ERR_IO;
    n = Write(fh, (APTR)buf, (LONG)len);
    Close(fh);
    if (n != (LONG)len) {
        DeleteFile((STRPTR)tmp);
        return LRV_ERR_IO;
    }
    /* Rename does not replace an existing file: the old record goes first */
    DeleteFile((STRPTR)path);
    if (!Rename((STRPTR)tmp, (STRPTR)path)) {
        DeleteFile((STRPTR)tmp);
        return LRV_ERR_IO;
    }
    return LRV_OK;
}
