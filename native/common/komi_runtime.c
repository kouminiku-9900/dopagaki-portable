#include "komi_runtime.h"

#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tilefinch/fetch.h"
#include "tilefinch/media_backend.h"
#include "tilefinch/platform.h"
#include "tilefinch/psp_log.h"
#include "tilefinch/psp_media_present.h"
#include "tilefinch/psp_media_scale.h"

#define BUDGET_BYTES (32u * 1024u * 1024u)
#define SESSION_CACHE_BYTES (1024u * 1024u)
#define NETWORK_TIMEOUT_US (30u * 1000u * 1000u)
#define CLOSE_TIMEOUT_US (10u * 1000u * 1000u)
#define ME_VISIBLE_LIMIT UINT32_C(0x0A000000)

KomiRuntime komi;

static FILE *result_log;
static volatile uint64_t last_progress_us;
static volatile const char *progress_stage = "boot";
static volatile uint64_t watchdog_us = 120u * 1000u * 1000u;
static PspMediaScaleMap scale_map;

uint64_t komi_now_us(void)
{
    return (uint64_t) sceKernelGetSystemTimeWide();
}

/* PSP_HEAP_SIZE_KB(-1) hands the whole partition to newlib at start, so the
   kernel's free counters only see what is outside the heap. What the
   program actually holds is the heap's in-use bytes. */
unsigned komi_heap_used(void)
{
    struct mallinfo info = mallinfo();
    return (unsigned) info.uordblks;
}

unsigned komi_heap_free(void)
{
    struct mallinfo info = mallinfo();
    return (unsigned) info.fordblks;
}

void komi_sibling_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, "%s", komi.argv0 != NULL ? komi.argv0 : "ms0:/");
    char *slash = strrchr(out, '/');
    size_t prefix = slash == NULL ? 0 : (size_t) (slash + 1 - out);
    snprintf(out + prefix, size - prefix, "%s", name);
}

void komi_result(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (result_log != NULL) {
        fprintf(result_log, "%s\n", line);
        fflush(result_log);
    }
    psp_log_printf("komi: %s\n", line);
}

/* tilefinch_core reports through plain printf; send it to the same
   validation log as the media session (linked with --wrap=printf). */
int __wrap_printf(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    psp_log_printf("%s", line);
    return length;
}

/* Every aligned block of 4 KiB or more, for the first few dozen: the Media
   Engine pool (reserved at boot) and any decoder buffer that falls back to
   the heap are memalign calls, so this shows exactly where they landed
   (linked with --wrap=memalign). */
void *__real_memalign(size_t alignment, size_t size);
static unsigned memalign_reports;

void *__wrap_memalign(size_t alignment, size_t size)
{
    void *block = __real_memalign(alignment, size);
    if (size >= 4096u && memalign_reports < 40u) {
        memalign_reports++;
        komi_result("memalign bytes=%u align=%u at=0x%08x side=%s",
                    (unsigned) size, (unsigned) alignment,
                    (unsigned) (uintptr_t) block,
                    (uintptr_t) block >= ME_VISIBLE_LIMIT ? "high" : "low");
    }
    return block;
}

void komi_progress(const char *stage)
{
    progress_stage = stage;
    last_progress_us = komi_now_us();
    psp_log_heartbeat();
}

void komi_set_watchdog_seconds(unsigned seconds)
{
    watchdog_us = (uint64_t) seconds * 1000000u;
    last_progress_us = komi_now_us();
}

static int watchdog_thread(SceSize args, void *argp)
{
    (void) args;
    (void) argp;
    for (;;) {
        sceKernelDelayThread(1000 * 1000);
        uint64_t idle = komi_now_us() - last_progress_us;
        if (idle > watchdog_us) {
            komi_result("WATCHDOG stage=%s stalled=%llus exiting",
                        (const char *) progress_stage,
                        (unsigned long long) (idle / 1000000u));
            psp_log_finish("watchdog");
            sceKernelExitGame();
        }
    }
    return 0;
}

void komi_log_open(const char *argv0, const char *log_name)
{
    komi.argv0 = argv0;
    last_progress_us = komi_now_us();
    char path[256];
    komi_sibling_path(path, sizeof path, log_name);
    result_log = fopen(path, "w");
    (void) psp_log_start(argv0);
    SceUID watchdog = sceKernelCreateThread(
        "komi_watchdog", watchdog_thread, 0x11, 16 * 1024,
        PSP_THREAD_ATTR_USER, NULL);
    if (watchdog >= 0) sceKernelStartThread(watchdog, 0, NULL);
}

void komi_log_close(const char *outcome)
{
    psp_log_finish(outcome);
    if (result_log != NULL) fclose(result_log);
    result_log = NULL;
}

/* --- platform services the media session and transport call back into --- */

static uint64_t platform_now_us(void *context)
{
    (void) context;
    return komi_now_us();
}

static uint64_t platform_now_ns(void *context)
{
    (void) context;
    return komi_now_us() * 1000u;
}

static uint64_t platform_wall_ns(void *context)
{
    (void) context;
    /* src/psp_time.c supplies the RTC-backed time() the TLS stack needs. */
    time_t seconds = time(NULL);
    return seconds <= 0 ? 0 : (uint64_t) seconds * UINT64_C(1000000000);
}

static const char *platform_language(void *context)
{
    (void) context;
    return "ja";
}

static bool platform_cooperate(void *context, const char *phase,
                               size_t completed_work_units)
{
    (void) context;
    (void) phase;
    (void) completed_work_units;
    last_progress_us = komi_now_us();
    return true;
}

static bool media_cancel_requested(void *context)
{
    (void) context;
    return false;
}

static size_t media_free_memory(void *context)
{
    (void) context;
    return (size_t) sceKernelTotalFreeMemSize();
}

static size_t media_largest_block(void *context)
{
    (void) context;
    return (size_t) sceKernelMaxFreeMemSize();
}

static bool media_resolve_offline(void *context, const char *url,
                                  PspMediaOfflineSource *source)
{
    (void) context;
    (void) url;
    (void) source;
    return false;
}

static void media_profile_changed(void *context, uint64_t at_us)
{
    (void) context;
    (void) at_us;
}

static bool media_failure_report(void *context, const char *stage,
                                 const char *detail, const char *url,
                                 long http_status, int native_result)
{
    (void) context;
    (void) url;
    komi_result("failure stage=%s http=%ld native=0x%08x detail=%s",
                stage == NULL ? "?" : stage, http_status,
                (unsigned) native_result, detail == NULL ? "" : detail);
    return true;
}

/* Newlib's heap grows upward from the program image, so one allocation that
   reaches just past the limit leaves every later large block above it. */
static void fill_low_heap(void)
{
    void *probe = malloc(64);
    uintptr_t at = (uintptr_t) probe;
    free(probe);
    if (at >= ME_VISIBLE_LIMIT) return;
    size_t size = (size_t) (ME_VISIBLE_LIMIT - at) + 256u * 1024u;
    void *ballast = malloc(size);
    komi_result("me-high ballast=0x%08x..0x%08x bytes=%u",
                (unsigned) (uintptr_t) ballast,
                (unsigned) ((uintptr_t) ballast + size), (unsigned) size);
    /* Held for the life of the process on purpose. */
}

void komi_platform_init(bool me_high)
{
    (void) scePowerSetClockFrequency(333, 333, 166);
    static const TilefinchPlatformServices services = {
        .wall_time_ns = platform_wall_ns,
        .monotonic_time_ns = platform_now_ns,
        .monotonic_time_us = platform_now_us,
        .preferred_language = platform_language,
        .cooperate = platform_cooperate,
    };
    tilefinch_platform_set_services(&services);
    if (me_high) fill_low_heap();
    /* Before any other large allocation, as the browser does. */
    media_psp_backend_reserve_pool();
    /* An empty name selects the hardware-qualified wide program, as the
       browser's default boot.cfg does; without this call the backend stays
       on the 240p compatibility program. */
    media_psp_backend_set_wide_program("");
    if (!psp_display_begin(&komi.display, psp_display_system_backend()))
        komi_result("display begin failed");
    komi_clear_screen();
    if (!tilefinch_install_paths_derive(komi.argv0, &komi.install_paths))
        memset(&komi.install_paths, 0, sizeof komi.install_paths);
}

/* --- Wi-Fi --- */

static bool try_profile(PspNetwork *network, int profile)
{
    memset(network, 0, sizeof *network);
    uint64_t started = komi_now_us();
    if (!psp_network_begin(network, profile)) {
        komi_result("network FAIL begin profile=%d", profile);
        return false;
    }
    PspNetworkStatus status = network->status;
    while (status != PSP_NETWORK_READY && status != PSP_NETWORK_FAILED
           && status != PSP_NETWORK_CANCELLED) {
        status = psp_network_pump(network, NETWORK_TIMEOUT_US);
        last_progress_us = komi_now_us();
        sceKernelDelayThread(10 * 1000);
    }
    komi_result("network %s profile=%d elapsed=%llums status=%s phase=%s "
                "native=0x%08x apctl=%d wlan-switch=%d heap-used=%u",
                status == PSP_NETWORK_READY ? "READY" : "FAIL", profile,
                (unsigned long long) ((komi_now_us() - started) / 1000u),
                psp_network_status_name(status),
                psp_network_status_name(network->failure_phase),
                (unsigned) network->native_result, network->apctl_state,
                network->wlan_switch_state, komi_heap_used());
    return status == PSP_NETWORK_READY;
}

/* No dialog: the firmware Wi-Fi screen would need a button press. Every
   saved connection is listed, then tried in order until one gets an address,
   starting with the one that worked last time (komi-wifi.txt). */
static bool connect_network(void)
{
    komi_progress("network");
    char memo_path[256];
    komi_sibling_path(memo_path, sizeof memo_path, "komi-wifi.txt");
    int remembered = 0;
    FILE *memo = fopen(memo_path, "r");
    if (memo != NULL) {
        if (fscanf(memo, "%d", &remembered) != 1) remembered = 0;
        fclose(memo);
    }
    int profiles[10];
    size_t count = 0;
    for (int profile = 1; profile <= 10; profile++) {
        if (!psp_network_profile_is_saved(profile)) continue;
        /* Only the number: logs get shared, access point names should not. */
        komi_result("wifi-profile %d saved", profile);
        if (profile == remembered && count > 0) {
            profiles[count++] = profiles[0];
            profiles[0] = profile;
        } else {
            profiles[count++] = profile;
        }
    }
    if (count == 0) {
        komi_result("network FAIL no saved Wi-Fi profile");
        return false;
    }
    for (size_t at = 0; at < count; at++) {
        if (try_profile(&komi.network, profiles[at])) {
            if (profiles[at] != remembered
                && (memo = fopen(memo_path, "w")) != NULL) {
                fprintf(memo, "%d\n", profiles[at]);
                fclose(memo);
            }
            /* The browser opens this when its network lifecycle reports
               ready; until then every transport request is refused. */
            fetch_background_transport_set_admission(true);
            return true;
        }
        PspNetworkShutdownReport report;
        psp_network_shutdown(&komi.network, &report);
    }
    return false;
}

bool komi_services_init(void)
{
    char ca_path[256];
    komi_sibling_path(ca_path, sizeof ca_path, "roots.pem");
    if (!fetch_set_ca_bundle_path(ca_path)) {
        komi_result("FAIL trust bundle %s", ca_path);
        return false;
    }
    /* From the XMB, connect the way the browser version does: the firmware
       joins the last-used access point by itself and shows its chooser only
       if that fails. Under PSPLink (host0:) nobody is there to press a
       button, so try the saved profiles without any dialog instead. */
    bool unattended = komi.argv0 != NULL
        && strncmp(komi.argv0, "host0:", 6) == 0;
    if (unattended) {
        komi.network_ready = connect_network();
    } else {
        komi_progress("network-dialog");
        int profile = 0;
        PspBootConnectResult connected = psp_network_boot_connect(&profile);
        (void) psp_display_rearm(&komi.display);
        komi_result("network dialog result=%d profile=%d heap-used=%u",
                    (int) connected, profile, komi_heap_used());
        komi.network_ready = connected == PSP_BOOT_CONNECT_READY;
        if (komi.network_ready) fetch_background_transport_set_admission(true);
    }
    if (!komi.network_ready) return false;
    budget_init(&komi.budget, BUDGET_BYTES);
    if (!browser_session_init(&komi.session, &komi.budget,
                              SESSION_CACHE_BYTES)) {
        komi_result("FAIL session init");
        return false;
    }
    komi.profile = browser_profile_create(&komi.budget);
    if (komi.profile == NULL) {
        komi_result("FAIL profile create");
        return false;
    }
    browser_profile_set_youtube_quality(komi.profile,
                                        BROWSER_YOUTUBE_QUALITY_360P);
    const PspMediaSessionPlatform platform = {
        .now_us = platform_now_us,
        .cancel_requested = media_cancel_requested,
        .free_memory = media_free_memory,
        .maximum_free_block = media_largest_block,
        .resolve_offline = media_resolve_offline,
        .profile_changed = media_profile_changed,
        .write_failure_report = media_failure_report,
        .install_paths = &komi.install_paths,
    };
    psp_media_init(&komi.media, &komi.budget, &komi.session, komi.profile,
                   NULL, NULL, &platform);
    return true;
}

/* --- presentation: the software scaler into the 16-bit page surface --- */

static bool portrait;
static uint16_t portrait_canvas[KOMI_PORTRAIT_STRIDE * KOMI_PORTRAIT_HEIGHT]
    __attribute__((aligned(64)));

void komi_set_portrait(bool on)
{
    portrait = on;
}

uint16_t *komi_canvas(void)
{
    return portrait ? portrait_canvas : psp_display_back_buffer(&komi.display);
}

/* Canvas (x, y) -> panel (479 - y, x), in 16x16 tiles so the canvas rows a
   tile reads stay in the data cache while the panel rows are written. */
static void rotate_into(uint16_t *vram)
{
    enum { TILE = 16 };
    for (int ty = 0; ty < KOMI_SCREEN_HEIGHT; ty += TILE) {
        for (int tx = 0; tx < KOMI_SCREEN_WIDTH; tx += TILE) {
            for (int y = ty; y < ty + TILE; y++) {
                uint16_t *out = vram + (size_t) y * KOMI_VRAM_STRIDE + tx;
                const uint16_t *in = portrait_canvas
                    + (size_t) (KOMI_PORTRAIT_HEIGHT - 1 - tx)
                        * KOMI_PORTRAIT_STRIDE + y;
                for (int x = 0; x < TILE; x++) {
                    out[x] = *in;
                    in -= KOMI_PORTRAIT_STRIDE;
                }
            }
        }
    }
}

void komi_canvas_present(void)
{
    if (portrait) {
        uint16_t *vram = psp_display_back_buffer(&komi.display);
        if (vram == NULL) return;
        rotate_into(vram);
    }
    (void) psp_display_publish(&komi.display);
}

void komi_clear_screen(void)
{
    uint16_t *vram = psp_display_back_buffer(&komi.display);
    if (vram == NULL) return;
    memset(vram, 0,
           (size_t) KOMI_VRAM_STRIDE * KOMI_SCREEN_HEIGHT * sizeof(*vram));
    (void) psp_display_publish(&komi.display);
}

static bool draw_picture(uint16_t *vram, int width, int height, int stride)
{
    PspMediaSession *media = &komi.media;
    const MediaVideoFrame *frame = &media->frame;
    if (frame->pixels == NULL || frame->width <= 0 || frame->height <= 0
        || frame->stride_pixels < frame->width || media->playback == NULL)
        return false;
    PspMediaPresentPlan plan;
    if (!psp_media_present_plan(&plan, frame->width, frame->height,
                                frame->stride_pixels, width, height))
        return false;
    PspMediaScaleFormat format = frame->format == MEDIA_PIXEL_RGB565
        ? PSP_MEDIA_SCALE_RGB565 : PSP_MEDIA_SCALE_RGBA8888;
    if (!psp_media_scale_map_matches(&scale_map, format, frame->width,
                                     frame->height, frame->stride_pixels,
                                     plan.video.width, plan.video.height)
        && !psp_media_scale_map_build(&scale_map, format, frame->width,
                                      frame->height, frame->stride_pixels,
                                      plan.video.width, plan.video.height))
        return false;
    /* The scaler reads the decoder's surface, so it must hold the slot the
       same way the browser's presenter does. */
    if (frame->slot >= 0
        && !media_playback_borrow_video_slot(
               media->playback, (unsigned) frame->slot, frame->generation))
        return false;
    psp_media_scale_blit(
        &scale_map, frame->pixels,
        vram + (size_t) plan.video.y * (size_t) stride + plan.video.x,
        stride);
    if (frame->slot >= 0)
        media_playback_release_video_read(media->playback,
                                          (unsigned) frame->slot);
    media_playback_note_frame_displayed(media->playback, frame,
                                        MEDIA_PSP_PRESENT_PATH_SOFTWARE);
    return true;
}

void komi_playback_frame(KomiPlayback *state, KomiOverlay overlay,
                         void *context, bool redraw)
{
    uint64_t now = komi_now_us();
    unsigned elapsed_ms = state->last_us == 0
        ? 0 : (unsigned) ((now - state->last_us) / 1000u);
    state->last_us = now;
    (void) psp_media_advance(&komi.media, elapsed_ms, NULL);
    bool fresh = komi.media.frame.pixels != NULL
        && komi.media.frame.identity != state->identity;
    if (fresh || redraw) {
        uint16_t *vram = komi_canvas();
        if (vram != NULL) {
            uint64_t draw_started = komi_now_us();
            int width = portrait ? KOMI_PORTRAIT_WIDTH : KOMI_SCREEN_WIDTH;
            int height = portrait ? KOMI_PORTRAIT_HEIGHT : KOMI_SCREEN_HEIGHT;
            int stride = portrait ? KOMI_PORTRAIT_STRIDE : KOMI_VRAM_STRIDE;
            memset(vram, 0, (size_t) stride * (size_t) height * sizeof(*vram));
            if (draw_picture(vram, width, height, stride)) {
                if (fresh) state->presented++;
                state->identity = komi.media.frame.identity;
            }
            if (overlay != NULL) overlay(vram, context);
            (void) psp_media_feed_before_blocking(&komi.media);
            komi_canvas_present();
            state->draw_us += komi_now_us() - draw_started;
            state->draws++;
            return;
        }
    }
    (void) psp_media_feed_before_blocking(&komi.media);
    sceDisplayWaitVblankStart();
}

void komi_playback_close(KomiPlayback *state)
{
    komi_progress("close");
    psp_media_close(&komi.media);
    uint64_t started = komi_now_us();
    while ((psp_media_open_work_pending(&komi.media)
            || psp_media_decode_work_pending(&komi.media))
           && komi_now_us() - started < CLOSE_TIMEOUT_US) {
        komi_playback_frame(state, NULL, NULL, false);
        komi_progress("closing");
    }
    psp_media_pipeline_destroy(&komi.media);
    state->identity = 0;
}
