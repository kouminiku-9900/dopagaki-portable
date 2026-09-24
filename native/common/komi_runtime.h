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

typedef enum {
    KOMI_LANDSCAPE,
    /* Draw on a 272x480 canvas in RAM and rotate all of it into the back
       buffer on present (the first version; two extra full-screen passes). */
    KOMI_PORTRAIT_CANVAS,
    /* Draw straight into the back buffer through the rotation: canvas
       (x, y) is back buffer [x * KOMI_VRAM_STRIDE + 479 - y]. The video is
       scaled and rotated in one tiled pass. */
    KOMI_PORTRAIT_DIRECT
} KomiOrientation;

/* Direct portrait draws the video with the graphics engine unless this is
   turned off (or sceGuInit failed); then with the CPU, in strips. */
void komi_set_portrait_ge(bool enabled);
bool komi_portrait_ge_active(void);
void komi_portrait_ge_counts(unsigned *draws, uint64_t *wait_us,
                             uint64_t *wait_max_us);
void komi_portrait_ge_stage_counts(uint64_t *stage_us,
                                   uint64_t *stage_max_us);

/* Switch presentation (before drawing anything). */
void komi_set_orientation(KomiOrientation orientation);
KomiOrientation komi_orientation(void);
/* How canvas (x, y) maps into the surface komi_canvas() returns:
   surface + origin + x * step_x + y * step_y. */
void komi_canvas_steps(long *step_x, long *step_y, long *origin);
/* The surface programs draw on: the portrait canvas, or the back buffer
   (landscape and direct portrait). NULL when there is no back buffer. */
uint16_t *komi_canvas(void);
/* Canvas pixel (x, y) of a surface komi_canvas() returned. */
uint16_t komi_canvas_read(const uint16_t *surface, int x, int y);
/* Rotate the canvas into the back buffer (canvas portrait) and show it. */
void komi_canvas_present(void);

typedef struct {
    uint64_t last_us;
    uint64_t identity;
    unsigned presented;
    /* Time per drawn frame, for the log: the whole draw, and each stage
       (KOMI_STAGE_*) as a total and a maximum. */
    uint64_t draw_us;
    unsigned draws;
    uint64_t stage_us[6];
    uint64_t stage_max_us[6];
    /* Frame loop gaps of 50 ms or more while playing, and what the frame
       before each did. */
    unsigned gaps;
    uint64_t last_advance_us;
    uint64_t advance_max_us;
    uint64_t last_draw_us;
    /* Audio output totals when the video opened (see komi_playback_audio). */
    uint32_t audio_blocks_at_open;
    uint32_t audio_starves_at_open;
    uint32_t audio_gaps_at_open;
    uint32_t audio_gap_us_at_open;
} KomiPlayback;

/* Remember the audio output totals at open; later, the blocks played and
   the times the audio queue ran dry since then. */
void komi_playback_audio_mark(KomiPlayback *state);
void komi_playback_audio(const KomiPlayback *state, unsigned *blocks,
                         unsigned *starves);
/* "audio-blocks=.. starves=.. gaps=N gap-ms=.. gap-max=..us" since the
   mark: gaps are output calls the DAC had to wait for (heard as crackle). */
void komi_playback_audio_text(const KomiPlayback *state, char *out,
                              size_t size);

enum {
    KOMI_STAGE_CLEAR,
    KOMI_STAGE_PICTURE,
    KOMI_STAGE_OVERLAY,
    KOMI_STAGE_FEED,
    KOMI_STAGE_ROTATE,
    KOMI_STAGE_PUBLISH
};

/* "clear=a/b picture=..." (mean/max microseconds per drawn frame). */
void komi_playback_stages(const KomiPlayback *state, char *out, size_t size);

/* One frame of playback: advance the session, draw a new picture (or, when
   redraw is set, the current one again) with the overlay, feed, flip.
   Without a picture to draw it waits for the vertical blank instead. */
void komi_playback_frame(KomiPlayback *state, KomiOverlay overlay,
                         void *context, bool redraw);
/* Feed the codec worker (at most every 2 ms) from inside long drawing
   work; komi_playback_frame does this itself. Programs may call it from
   their overlay. On by default; off only for comparisons. */
void komi_feed_tick(void);
void komi_set_feed_ticks(bool enabled);
void komi_feed_tick_counts(unsigned *ticks, unsigned *submits);

/* Close the current video and wait (bounded) until its pipeline is gone. */
void komi_playback_close(KomiPlayback *state);

#endif
