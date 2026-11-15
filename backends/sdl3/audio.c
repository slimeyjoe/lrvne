/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SDL3 backend, audio: the main stream in pull mode, the auxiliary stream and the shutdown
 * watchdog.
 *
 * Pull mode (lrvne/hal.h): SDL's audio thread asks for exactly the samples the device needs
 * and the source given to audio_open renders them. The source takes the audio lock itself, the
 * one the engine takes around its own sound calls, so the thread calls it without. SDL always
 * gets its samples in time here, so the source's skip is never used. The game loop can therefore stall for as long as it likes (decoding a
 * picture, a window being dragged) without the music stuttering, as a music driver on a timer
 * interrupt never did.
 *
 * The auxiliary stream (voice) is a second stream bound to the same device: SDL mixes the
 * bound streams and converts the clips' rate to the device's. The engine pushes from its
 * frame; streams are thread-safe. Its volume is the stream's gain, so no bits are lost. */
#include <stdlib.h>
#include "sdl3.h"

#define AUX_QUEUE_MAX 65536     /* frames the auxiliary stream holds at most */

static SDL_AudioDeviceID g_dev;
static SDL_AudioStream *g_main, *g_aux;
static SDL_Mutex *g_lock;
static struct lrv_audio_source g_src;
static int16_t g_buf[4096];             /* the audio thread's alone */
static float g_aux_gain = 1.0f;
static int g_audio_inited;

/* SDL's audio thread: `additional` bytes of mono 16-bit samples are wanted now */
static void SDLCALL feed(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    int frames = (additional + 1) / 2;
    (void)userdata;
    (void)total;
    while (frames > 0) {
        int max = (int)(sizeof g_buf / sizeof g_buf[0]), n = frames > max ? max : frames;
        g_src.render(g_src.ctx, g_buf, n);
        SDL_PutAudioStreamData(stream, g_buf, n * 2);
        frames -= n;
    }
}

int lrv_sdl3_audio_open(int rate, const struct lrv_audio_source *src)
{
    SDL_AudioSpec spec;

    if (lrv_sdl3.control || rate <= 0 || !src || !src->render || g_main)
        return 0;
    if (!g_audio_inited && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("audio: %s", SDL_GetError());
        return 0;
    }
    g_audio_inited = 1;
    g_src = *src;
    g_lock = SDL_CreateMutex();
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = rate;
    /* the device is opened by itself (not with SDL_OpenAudioDeviceStream): a device opened
     * that way takes no second stream, and the auxiliary one binds its own */
    g_dev = g_lock ? SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL) : 0;
    g_main = g_dev ? SDL_CreateAudioStream(&spec, NULL) : NULL;
    if (!g_main || !SDL_SetAudioStreamGetCallback(g_main, feed, NULL) ||
        !SDL_BindAudioStream(g_dev, g_main)) {
        SDL_Log("audio: %s", SDL_GetError());
        lrv_sdl3_audio_close();
        return 0;
    }
    return rate;
}

void lrv_sdl3_audio_close(void)
{
    /* SDL's audio backends can hang for good when the default output changes under them (a
     * hotplug thread waiting for the new device while holding the device lock), and then
     * destroying a stream blocks on it. An engine closes its audio when it is leaving and its
     * saves are written, so if this does not return in time the process just ends. */
    lrv_sdl3_watchdog_arm();
    lrv_sdl3_aux_close();
    if (g_main)
        SDL_DestroyAudioStream(g_main); /* unbinds; stops the callbacks before returning */
    g_main = 0;
    if (g_dev)
        SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
    if (g_lock)
        SDL_DestroyMutex(g_lock);
    g_lock = 0;
    lrv_sdl3_watchdog_disarm();
}

void lrv_sdl3_audio_lock(void)
{
    if (g_lock)
        SDL_LockMutex(g_lock);
}

void lrv_sdl3_audio_unlock(void)
{
    if (g_lock)
        SDL_UnlockMutex(g_lock);
}

/* the push model is not this backend's: the entries exist so that the HAL table is whole */
int lrv_sdl3_audio_space(void)
{
    return 0;
}

void lrv_sdl3_audio_push(const int16_t *pcm, int frames)
{
    (void)pcm;
    (void)frames;
}

/* ---- the auxiliary stream ----------------------------------------------------------------- */

int lrv_sdl3_aux_open(int rate)
{
    SDL_AudioSpec spec;

    if (!g_main || !g_dev || rate <= 0 || g_aux)
        return 0;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = rate;
    g_aux = SDL_CreateAudioStream(&spec, NULL);
    if (!g_aux)
        return 0;
    SDL_SetAudioStreamGain(g_aux, g_aux_gain);
    if (!SDL_BindAudioStream(g_dev, g_aux)) {
        SDL_Log("audio: %s", SDL_GetError());
        SDL_DestroyAudioStream(g_aux);
        g_aux = 0;
        return 0;
    }
    return rate;
}

void lrv_sdl3_aux_close(void)
{
    if (g_aux)
        SDL_DestroyAudioStream(g_aux);
    g_aux = 0;
}

int lrv_sdl3_aux_queued(void)
{
    int bytes = g_aux ? SDL_GetAudioStreamQueued(g_aux) : 0;
    return bytes > 0 ? bytes / 2 : 0;
}

int lrv_sdl3_aux_space(void)
{
    int queued = lrv_sdl3_aux_queued();
    return g_aux && queued < AUX_QUEUE_MAX ? AUX_QUEUE_MAX - queued : 0;
}

void lrv_sdl3_aux_push(const int16_t *pcm, int frames)
{
    if (!g_aux)
        return;
    if (frames > 0)
        SDL_PutAudioStreamData(g_aux, pcm, frames * 2);
    else
        SDL_FlushAudioStream(g_aux);    /* end of a clip: the frames the resampler held back
                                         * too, or the queue would never drain to zero */
}

void lrv_sdl3_aux_flush(void)
{
    if (g_aux)
        SDL_ClearAudioStream(g_aux);
}

int lrv_sdl3_set_volume(int which, int percent)
{
    if (which != LRV_VOL_AUX)
        return 0;                       /* the main stream is scaled by its renderer */
    g_aux_gain = (float)percent / 100.0f;
    if (g_aux)
        SDL_SetAudioStreamGain(g_aux, g_aux_gain);
    return 1;
}

/* ---- the shutdown watchdog ---------------------------------------------------------------- */

#define SHUTDOWN_GRACE_MS 2000

static SDL_AtomicInt g_watch;           /* changes whenever the watchdog is armed or disarmed */

static int SDLCALL watchdog(void *arg)
{
    int armed_as = (int)(intptr_t)arg;
    SDL_Delay(SHUTDOWN_GRACE_MS);
    if (SDL_GetAtomicInt(&g_watch) == armed_as) {
        SDL_Log("%s: shutdown still blocked after %d ms (audio device), exiting",
                lrv_sdl3.app->name, SHUTDOWN_GRACE_MS);
        _Exit(0);
    }
    return 0;
}

void lrv_sdl3_watchdog_arm(void)
{
    int armed_as = SDL_AddAtomicInt(&g_watch, 1) + 1;
    SDL_Thread *t = SDL_CreateThread(watchdog, "shutdown-watchdog", (void *)(intptr_t)armed_as);
    if (t)
        SDL_DetachThread(t);
}

void lrv_sdl3_watchdog_disarm(void)
{
    SDL_AddAtomicInt(&g_watch, 1);
}
