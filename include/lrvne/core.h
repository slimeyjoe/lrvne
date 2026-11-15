/* SPDX-License-Identifier: GPL-3.0-or-later */
/* lrvne - common definitions: version, error codes and the helpers every module uses on top of
 * the HAL (formatted logging, checked allocation).
 *
 * Conventions of the whole library:
 *   - Functions that can fail return 0 (LRV_OK) or a negative enum lrv_err; functions that
 *     return a pointer return NULL on failure. lrv_fatal() does not return.
 *   - No module keeps hidden global state: state lives in a structure the caller owns and
 *     passes in. The one process-wide object is the HAL, which a backend provides.
 *   - All file formats are little-endian and are read through lrvne/endian.h; the library runs
 *     on big-endian hosts.
 *   - Memory comes from the HAL and is released with its size (hosts whose allocator wants
 *     it). */
#ifndef LRVNE_CORE_H
#define LRVNE_CORE_H

#include <stddef.h>
#include <stdint.h>

#define LRV_VERSION_MAJOR 0
#define LRV_VERSION_MINOR 1
#define LRV_VERSION_PATCH 0
#define LRV_VERSION       "0.1.0"

enum lrv_err {
    LRV_OK           = 0,
    LRV_ERR          = -1,    /* unspecified failure */
    LRV_ERR_NOMEM    = -2,
    LRV_ERR_IO       = -3,    /* the backend could not read or write */
    LRV_ERR_FORMAT   = -4,    /* data is not what its format says */
    LRV_ERR_NOTFOUND = -5,
    LRV_ERR_RANGE    = -6     /* a value or a size is outside what the caller allows */
};

/* A short constant description of an error code, for messages. */
const char *lrv_strerror(int err);

#if defined(__GNUC__) || defined(__clang__)
#define LRV_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#define LRV_NORETURN          __attribute__((noreturn))
#else
#define LRV_PRINTF(fmt, args)
#define LRV_NORETURN
#endif

struct lrv_hal;

/* Formatted log line (at most 511 bytes) through hal->log. */
void lrv_log(const struct lrv_hal *hal, int level, const char *fmt, ...) LRV_PRINTF(3, 4);

/* Formatted message through hal->fatal, which shows it and ends the program. */
void lrv_fatal(const struct lrv_hal *hal, const char *fmt, ...) LRV_PRINTF(2, 3) LRV_NORETURN;

/* Zeroed memory from the HAL; NULL when there is none. */
void *lrv_alloc(const struct lrv_hal *hal, size_t n);
/* The same, but running out of memory is fatal: never returns NULL. */
void *lrv_xalloc(const struct lrv_hal *hal, size_t n);
/* Releases what lrv_alloc/lrv_xalloc returned for the same n; p may be NULL. */
void lrv_free(const struct lrv_hal *hal, void *p, size_t n);

/* Compares two names without regard to ASCII case. Returns 1 when equal. */
int lrv_name_eq(const char *a, const char *b);

#endif
