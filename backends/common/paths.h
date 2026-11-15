/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Where a program built on lrvne finds its packs and keeps its saves on a POSIX host, and the
 * pack and storage functions of the HAL on plain files. The rules are those of lrvne/app.h;
 * nothing here knows a program, a pack or a game by name.
 *
 * The first half are pure functions over paths, which the tests exercise in a scratch
 * directory. The second half is the process's one instance, configured once by the backend
 * and then called through the HAL. */
#ifndef LRVNE_BACKENDS_PATHS_H
#define LRVNE_BACKENDS_PATHS_H

#include <stddef.h>
#include <stdint.h>

#define LRV_PATH_MAX 1024

/* ---- pure helpers ----------------------------------------------------------------------- */

/* The executable's own file, symbolic links resolved. argv0 is used where the system cannot
 * tell. Returns 1, or 0 when it is not known. */
int lrv_paths_exe(char *out, size_t cap, const char *argv0);

/* 1 when the file ends with the trailer of a bundled pack (lrvne/pack.h). */
int lrv_paths_has_pack(const char *path);

/* The main pack of the program whose executable is `exe` (may be NULL), looking in the
 * executable's directory and then in `cwd`: the executable itself when a pack is appended to
 * it; else, in each directory in turn, <program>.pak, or the only *.pak there (voice packs,
 * *_voice.pak, do not count). Returns 1 with the path in out, else 0. */
int lrv_paths_find_pack(char *out, size_t cap, const char *exe, const char *cwd);

/* The voice pack that belongs to a main pack: <pack>_voice.pak beside it, <pack> being the
 * pack's file name without ".pak" (the executable's name when the pack is bundled). Returns 0
 * when the path does not fit. */
int lrv_paths_voice_pack(char *out, size_t cap, const char *pack);

/* The storage directory: `arg` (--storage) when given, else
 * <data>/<app_name>/<game_id> with <data> = $XDG_DATA_HOME, or $HOME/.local/share; the two
 * variables are passed in (NULL or empty = not set). Returns 0, or LRV_ERR_RANGE when the id is
 * not a file name or the path does not fit. */
int lrv_paths_storage_dir(char *out, size_t cap, const char *arg, const char *app_name,
                          const char *game_id, const char *xdg_data_home, const char *home);

/* 1 when `name` can be a file name of its own: not empty, no '/', not "." or "..". */
int lrv_paths_name_ok(const char *name);

/* mkdir -p. Returns 0 or LRV_ERR_IO. */
int lrv_paths_mkdirs(const char *dir);

/* Reads / writes a record as the file <dir>/<key>; writing goes through <key>.tmp and a
 * rename, so a record is never left half written. 0, LRV_ERR_NOTFOUND, LRV_ERR_IO,
 * LRV_ERR_RANGE (a key that is not a file name). */
int lrv_paths_record_read(const char *dir, const char *key, void *buf, size_t cap, size_t *len);
int lrv_paths_record_write(const char *dir, const char *key, const void *buf, size_t len);

/* ---- the process's instance: HAL functions ------------------------------------------------ */

struct lrv_paths_cfg {
    const char *app_name;       /* app->name */
    const char *argv0;
    const char *pak;            /* --pak or NULL */
    const char *voice_pak;      /* --voice-pak or NULL */
    int no_voice;               /* --no-voice */
    int voice_explicit;         /* 1: a voice pack only with --voice-pak (no search) */
    const char *storage;        /* --storage or NULL */
    const char *storage_default;/* the directory used without --storage instead of the
                                 * user's data directory, or NULL */
};

/* Sets the instance up; the strings must stay valid. */
void lrv_paths_init(const struct lrv_paths_cfg *cfg);

void  *lrv_paths_pack_open(int which, uint32_t *size);
size_t lrv_paths_pack_read(void *h, uint32_t off, void *dst, size_t n);
void   lrv_paths_pack_close(void *h);
int    lrv_paths_storage_open(const char *game_id);
int    lrv_paths_storage_read(const char *key, void *buf, size_t cap, size_t *len);
int    lrv_paths_storage_write(const char *key, const void *buf, size_t len);

#endif
