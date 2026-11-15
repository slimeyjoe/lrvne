/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SDL3 backend: what its files share. main.c assembles the HAL from the functions below;
 * video.c owns the window, input.c the events, audio.c the sound device. */
#ifndef LRVNE_BACKENDS_SDL3_H
#define LRVNE_BACKENDS_SDL3_H

#include <SDL3/SDL.h>
#include <stdint.h>
#include "lrvne/app.h"
#include "lrvne/hal.h"
#include "autoplay.h"
#include "control.h"

/* The state more than one file reads. */
struct lrv_sdl3 {
    const struct lrv_app *app;
    int control;                    /* --control: offscreen, paced by the driver, no audio */
    int vsync;                      /* --tick vsync: a frame is the display's refresh */
    int win_w, win_h;               /* --window, 0 = twice the video mode */
    struct lrv_video_mode mode;     /* the mode of the open display; mode.w == 0: none */
    struct lrv_autoplay autoplay;   /* --autoplay */
    int have_autoplay;
    uint32_t frames;                /* frame_wait calls */
};

extern struct lrv_sdl3 lrv_sdl3;

/* ---- video.c ------------------------------------------------------------------------------ */

int  lrv_sdl3_video_open(const struct lrv_video_cfg *cfg);
void lrv_sdl3_video_close(void);
void lrv_sdl3_video_present(const struct lrv_frame *frame);
int  lrv_sdl3_readback(struct lrv_ctl_readback *rb);
SDL_Window *lrv_sdl3_window(void);
/* A window or renderer event that is not input. */
void lrv_sdl3_video_event(const SDL_Event *e);
void lrv_sdl3_toggle_fullscreen(void);
void lrv_sdl3_cycle_scale(void);
/* Window coordinates of an event -> pixel of the video mode, clamped to the picture. Returns 0
 * when there is no display. */
int  lrv_sdl3_window_to_mode(float wx, float wy, int *x, int *y);
/* A distance in window coordinates -> pixels of the video mode (not rounded). */
void lrv_sdl3_window_to_mode_rel(float wdx, float wdy, float *dx, float *dy);
/* The centre of a pixel of the video mode -> window coordinates. */
int  lrv_sdl3_mode_to_window(int x, int y, float *wx, float *wy);

/* ---- input.c ------------------------------------------------------------------------------ */

void lrv_sdl3_input_poll(struct lrv_input *in);
void lrv_sdl3_input_flush(void);
void lrv_sdl3_mouse_warp(int x, int y);

/* ---- audio.c ------------------------------------------------------------------------------ */

int  lrv_sdl3_audio_open(int rate, const struct lrv_audio_source *src);
void lrv_sdl3_audio_close(void);
int  lrv_sdl3_audio_space(void);
void lrv_sdl3_audio_push(const int16_t *pcm, int frames);
void lrv_sdl3_audio_lock(void);
void lrv_sdl3_audio_unlock(void);
int  lrv_sdl3_aux_open(int rate);
void lrv_sdl3_aux_close(void);
int  lrv_sdl3_aux_space(void);
void lrv_sdl3_aux_push(const int16_t *pcm, int frames);
int  lrv_sdl3_aux_queued(void);
void lrv_sdl3_aux_flush(void);
int  lrv_sdl3_set_volume(int which, int percent);
/* Ends the process if the calls that follow have not let it end by itself in time; disarm
 * when they returned. */
void lrv_sdl3_watchdog_arm(void);
void lrv_sdl3_watchdog_disarm(void);

#endif
