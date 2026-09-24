/*
 * What every komi-tube native program needs from tilefinch, set up without
 * the browser: logging beside the program, a watchdog, platform services,
 * the Media Engine pool, the display, Wi-Fi without the firmware dialog, a
 * bounded HTTP session, and the media session with a software presenter.
 *
 * Shared by native/player (the unattended playback test) and native/app
 * (the client). One process has one runtime; the state is global.
 */
#ifndef KOMI_RUNTIME_H
#define KOMI_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/browser_profile.h"
#include "tilefinch/budget.h"
#include "tilefinch/install_paths.h"
#include "tilefinch/psp_display.h"
#include "tilefinch/psp_media_session.h"
#include "tilefinch/psp_network.h"
#include "tilefinch/session.h"

#define KOMI_SCREEN_WIDTH 480
#define KOMI_SCREEN_HEIGHT 272
#define KOMI_VRAM_STRIDE 512

typedef struct {
    const char *argv0;
    PspDisplay display;
    PspMediaSession media;
    Budget budget;
    BrowserSession session;
    BrowserProfile *profile;
    TilefinchInstallPaths install_paths;
    PspNetwork network;
    bool network_ready;
} KomiRuntime;

extern KomiRuntime komi;

uint64_t komi_now_us(void);
unsigned komi_heap_used(void);
unsigned komi_heap_free(void);

/* <directory of argv0>/<name>. */
void komi_sibling_path(char *out, size_t size, const char *name);

/* Open <log_name> beside the program (truncating) and start the tilefinch
   log (tilefinch-validation.txt) and the watchdog. */
void komi_log_open(const char *argv0, const char *log_name);
void komi_log_close(const char *outcome);
/* One line to the result log (flushed) and the tilefinch log. */
void komi_result(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
/* Note progress for the watchdog; stage is a string literal. */
void komi_progress(const char *stage);
/* The watchdog ends the program after this long without progress. */
void komi_set_watchdog_seconds(unsigned seconds);

/* Clock, platform services, Media Engine pool, wide (360p) program, display.
   me_high fills the heap below 0x0A000000 first (extended-bank test). */
void komi_platform_init(bool me_high);
/* CA bundle, Wi-Fi (saved profiles, last good first), HTTP session, profile,
   media session. Returns false with a result line on failure. */
bool komi_services_init(void);

/* Black screen, published. */
void komi_clear_screen(void);

/* Called with the drawing surface (the back buffer, or the portrait canvas)
   after the video is drawn and before it is shown; may be NULL. */
typedef void (*KomiOverlay)(uint16_t *vram, void *context);

/* Portrait mode (dopagaki-portable): the PSP is held with its left edge
   (the analog stick) at the bottom. Programs draw on a 272x480 canvas, and
   presenting rotates it into the landscape page: canvas (x, y) lands on the
   panel at (479 - y, x). */
#define KOMI_PORTRAIT_WIDTH KOMI_SCREEN_HEIGHT
#define KOMI_PORTRAIT_HEIGHT KOMI_SCREEN_WIDTH
#define KOMI_PORTRAIT_STRIDE KOMI_PORTRAIT_WIDTH

/* Switch presentation to the portrait canvas (before drawing anything). */
void komi_set_portrait(bool portrait);
/* The surface programs draw on: the portrait canvas, or the back buffer in
   landscape. NULL when there is no back buffer. */
uint16_t *komi_canvas(void);
/* Rotate the canvas into the back buffer (portrait) and show it. */
void komi_canvas_present(void);

typedef struct {
    uint64_t last_us;
    uint64_t identity;
    unsigned presented;
    /* Portrait only: time spent scaling + rotating, for the log. */
    uint64_t draw_us;
    unsigned draws;
} KomiPlayback;

/* One frame of playback: advance the session, draw a new picture (or, when
   redraw is set, the current one again) with the overlay, feed, flip.
   Without a picture to draw it waits for the vertical blank instead. */
void komi_playback_frame(KomiPlayback *state, KomiOverlay overlay,
                         void *context, bool redraw);
/* Close the current video and wait (bounded) until its pipeline is gone. */
void komi_playback_close(KomiPlayback *state);

#endif
