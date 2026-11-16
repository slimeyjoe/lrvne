/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The machines lrvne models, by name: what a backend needs to open one's video mode without an
 * engine (the presentation test of backends/common/ptest.h, given a mode on its command
 * line). An engine names its mode in code (lrvne/pc98.h) and does not need this. */
#ifndef LRVNE_MACHINE_H
#define LRVNE_MACHINE_H

#include "lrvne/hal.h"

/* The video mode of machine `name` ("pc98"; any case), NULL when there is none. */
const struct lrv_video_mode *lrv_video_mode_by_name(const char *name);

/* The names lrv_video_mode_by_name knows, separated by '|' ("pc98"), for usage texts. */
extern const char lrv_video_mode_names[];

#endif
