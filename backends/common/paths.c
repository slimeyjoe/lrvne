/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pack discovery, the storage directory and file-backed pack and storage I/O for POSIX hosts
 * (paths.h). This file is the only place of the hosted backends that names a directory. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE        /* realpath and _NSGetExecutablePath under strict POSIX flags */
#endif
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>        /* macOS has no /proc/self/exe */
#endif
#include "lrvne/core.h"
#include "lrvne/endian.h"
#include "lrvne/hal.h"
#include "lrvne/pack.h"
#include "paths.h"

/* ---- pure helpers ------------------------------------------------------------------------- */

/* out = dir "/" name suffix; dir may be empty or end with a slash. Returns 1 when it fits. */
static int join(char *out, size_t cap, const char *dir, const char *name, const char *suffix)
{
    size_t dl = strlen(dir);
    const char *sep = dl && dir[dl - 1] != '/' ? "/" : "";
    int n = snprintf(out, cap, "%s%s%s%s", dir, sep, name, suffix);
    return n > 0 && (size_t)n < cap;
}

static int readable(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && lrv_name_eq(s + n - m, suffix);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int lrv_paths_exe(char *out, size_t cap, const char *argv0)
{
#ifdef __APPLE__
    char raw[4096], real[4096];
    uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) == 0 && realpath(raw, real) && strlen(real) < cap) {
        strcpy(out, real);
        return 1;
    }
#else
    ssize_t n = cap ? readlink("/proc/self/exe", out, cap - 1) : -1;
    if (n > 0) {
        out[n] = 0;
        return 1;
    }
#endif
    /* a path the program was started by; a bare name found on the PATH tells nothing */
    if (argv0 && strchr(argv0, '/') && strlen(argv0) < cap) {
        strcpy(out, argv0);
        return 1;
    }
    return 0;
}

int lrv_paths_has_pack(const char *path)
{
    uint8_t tr[LRV_PACK_TRAILER_SIZE];
    FILE *f = fopen(path, "rb");
    int ok;

    if (!f)
        return 0;
    ok = fseek(f, -(long)sizeof tr, SEEK_END) == 0 && fread(tr, 1, sizeof tr, f) == sizeof tr &&
         !memcmp(tr, "XPKT", 4) && !memcmp(tr + 12, "XPKT", 4) &&
         (lrv_rd32le(tr + 4) ^ lrv_rd32le(tr + 8)) == 0xFFFFFFFFu;
    fclose(f);
    return ok;
}

/* <dir>/<program>.pak, or the only main pack in dir */
static int find_in_dir(char *out, size_t cap, const char *dir, const char *program)
{
    char only[256];
    struct dirent *de;
    DIR *d;
    int count = 0;

    if (program && *program && join(out, cap, dir, program, ".pak") && readable(out))
        return 1;
    d = opendir(*dir ? dir : ".");
    if (!d)
        return 0;
    while ((de = readdir(d)) != 0) {
        if (!ends_with(de->d_name, ".pak") || ends_with(de->d_name, "_voice.pak") ||
            strlen(de->d_name) >= sizeof only)
            continue;
        strcpy(only, de->d_name);
        count++;
    }
    closedir(d);
    return count == 1 && join(out, cap, dir, only, "") && readable(out);
}

int lrv_paths_find_pack(char *out, size_t cap, const char *exe, const char *cwd)
{
    const char *program = exe ? base_name(exe) : 0;

    if (exe) {
        char dir[LRV_PATH_MAX];
        size_t n = (size_t)(base_name(exe) - exe);
        if (lrv_paths_has_pack(exe) && strlen(exe) < cap) {
            strcpy(out, exe);
            return 1;
        }
        if (n < sizeof dir) {
            memcpy(dir, exe, n);
            dir[n] = 0;
            if (find_in_dir(out, cap, n ? dir : ".", program))
                return 1;
        }
    }
    return cwd && find_in_dir(out, cap, cwd, program);
}

int lrv_paths_voice_pack(char *out, size_t cap, const char *pack)
{
    size_t n = strlen(pack);
    int w;
    if (ends_with(pack, ".pak"))
        n -= 4;
    w = snprintf(out, cap, "%.*s_voice.pak", (int)n, pack);
    return w > 0 && (size_t)w < cap;
}

int lrv_paths_name_ok(const char *name)
{
    return name && *name && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..");
}

int lrv_paths_storage_dir(char *out, size_t cap, const char *arg, const char *app_name,
                          const char *game_id, const char *xdg_data_home, const char *home)
{
    int n;
    if (arg && *arg)
        n = snprintf(out, cap, "%s", arg);
    else if (!lrv_paths_name_ok(app_name) || !lrv_paths_name_ok(game_id))
        return LRV_ERR_RANGE;
    else if (xdg_data_home && *xdg_data_home)
        n = snprintf(out, cap, "%s/%s/%s", xdg_data_home, app_name, game_id);
    else
        n = snprintf(out, cap, "%s/.local/share/%s/%s", home && *home ? home : ".", app_name,
                     game_id);
    return n > 0 && (size_t)n < cap ? LRV_OK : LRV_ERR_RANGE;
}

int lrv_paths_mkdirs(const char *dir)
{
    char tmp[LRV_PATH_MAX];
    struct stat st;
    char *p;

    if (strlen(dir) >= sizeof tmp)
        return LRV_ERR_IO;
    strcpy(tmp, dir);
    for (p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0700);               /* the last mkdir tells whether it worked */
            *p = '/';
        }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return LRV_ERR_IO;
    return stat(tmp, &st) == 0 && S_ISDIR(st.st_mode) ? LRV_OK : LRV_ERR_IO;
}

int lrv_paths_record_read(const char *dir, const char *key, void *buf, size_t cap, size_t *len)
{
    char path[LRV_PATH_MAX];
    FILE *f;

    if (!lrv_paths_name_ok(key) || !join(path, sizeof path, dir, key, ""))
        return LRV_ERR_RANGE;
    f = fopen(path, "rb");
    if (!f)
        return errno == ENOENT ? LRV_ERR_NOTFOUND : LRV_ERR_IO;
    *len = fread(buf, 1, cap, f);
    if (ferror(f)) {
        fclose(f);
        return LRV_ERR_IO;
    }
    fclose(f);
    return LRV_OK;
}

int lrv_paths_record_write(const char *dir, const char *key, const void *buf, size_t len)
{
    char path[LRV_PATH_MAX], tmp[LRV_PATH_MAX];
    FILE *f;
    int ok;

    if (!lrv_paths_name_ok(key) || !join(path, sizeof path, dir, key, "") ||
        !join(tmp, sizeof tmp, dir, key, ".tmp"))
        return LRV_ERR_RANGE;
    f = fopen(tmp, "wb");
    if (!f)
        return LRV_ERR_IO;
    ok = fwrite(buf, 1, len, f) == len;
    if (fclose(f) != 0)
        ok = 0;
    if (ok && rename(tmp, path) == 0)
        return LRV_OK;
    remove(tmp);
    return LRV_ERR_IO;
}

/* ---- the process's instance --------------------------------------------------------------- */

static struct lrv_paths_cfg g_cfg;
static char g_pack[LRV_PATH_MAX];           /* the main pack, once located */
static char g_storage[LRV_PATH_MAX];        /* the storage directory, once opened */

void lrv_paths_init(const struct lrv_paths_cfg *cfg)
{
    g_cfg = *cfg;
    g_pack[0] = 0;
    g_storage[0] = 0;
}

static FILE *open_sized(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;

    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || (unsigned long)n > 0xFFFFFFFFul) {
        fclose(f);
        return 0;
    }
    *size = (uint32_t)n;
    return f;
}

static int locate_main(void)
{
    char exe[LRV_PATH_MAX];
    int have_exe;

    if (g_pack[0])
        return 1;
    if (g_cfg.pak) {
        if (strlen(g_cfg.pak) >= sizeof g_pack)
            return 0;
        strcpy(g_pack, g_cfg.pak);
        return 1;
    }
    have_exe = lrv_paths_exe(exe, sizeof exe, g_cfg.argv0);
    return lrv_paths_find_pack(g_pack, sizeof g_pack, have_exe ? exe : 0, ".");
}

void *lrv_paths_pack_open(int which, uint32_t *size)
{
    char path[LRV_PATH_MAX];
    const char *name = 0;
    FILE *f;

    if (which == LRV_PACK_MAIN) {
        if (locate_main())
            name = g_pack;
    } else if (which == LRV_PACK_VOICE && !g_cfg.no_voice) {
        if (g_cfg.voice_pak)
            name = g_cfg.voice_pak;
        else if (!g_cfg.voice_explicit && locate_main() &&
                 lrv_paths_voice_pack(path, sizeof path, g_pack))
            name = path;
    }
    if (!name)
        return 0;
    f = open_sized(name, size);
    /* a file the command line named and that cannot be opened deserves a word; a pack that
     * is simply not there is the caller's to report */
    if (!f && (which == LRV_PACK_MAIN ? g_cfg.pak : g_cfg.voice_pak))
        fprintf(stderr, "%s: cannot open %s\n", g_cfg.app_name, name);
    return f;
}

size_t lrv_paths_pack_read(void *h, uint32_t off, void *dst, size_t n)
{
    if (off > 0x7FFFFFFFu || fseek((FILE *)h, (long)off, SEEK_SET) != 0)
        return 0;
    return fread(dst, 1, n, (FILE *)h);
}

void lrv_paths_pack_close(void *h)
{
    if (h)
        fclose((FILE *)h);
}

int lrv_paths_storage_open(const char *game_id)
{
    int rc;

    if (!g_cfg.storage && g_cfg.storage_default)
        rc = lrv_paths_storage_dir(g_storage, sizeof g_storage, g_cfg.storage_default, 0, 0, 0, 0);
    else
        rc = lrv_paths_storage_dir(g_storage, sizeof g_storage, g_cfg.storage, g_cfg.app_name,
                                   game_id, getenv("XDG_DATA_HOME"), getenv("HOME"));
    if (rc == LRV_OK)
        rc = lrv_paths_mkdirs(g_storage);
    if (rc != LRV_OK)
        g_storage[0] = 0;
    return rc;
}

int lrv_paths_storage_read(const char *key, void *buf, size_t cap, size_t *len)
{
    if (!g_storage[0])
        return LRV_ERR;
    return lrv_paths_record_read(g_storage, key, buf, cap, len);
}

int lrv_paths_storage_write(const char *key, const void *buf, size_t len)
{
    if (!g_storage[0])
        return LRV_ERR;
    return lrv_paths_record_write(g_storage, key, buf, len);
}
