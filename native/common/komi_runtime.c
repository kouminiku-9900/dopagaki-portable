#include "komi_runtime.h"

#include <pspdisplay.h>
#include <pspdmac.h>
#include <pspge.h>
#include <pspgu.h>
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

/* The codec worker takes one submission per feed and then idles until the
   next one, so a long draw starves it (audio gaps, heard as crackle). Feed
   it every couple of milliseconds while drawing. */
#define FEED_TICK_US 2000u
static uint64_t last_feed_us;
static bool feed_ticks_enabled = true;
static unsigned feed_ticks, feed_tick_submits;

static void feed_tick(void)
{
    if (!feed_ticks_enabled) return;
    uint64_t now = komi_now_us();
    if (now - last_feed_us < FEED_TICK_US) return;
    last_feed_us = now;
    feed_ticks++;
    if (psp_media_feed_before_blocking(&komi.media) > 0) feed_tick_submits++;
}

void komi_set_feed_ticks(bool enabled)
{
    feed_ticks_enabled = enabled;
}

void komi_feed_tick(void)
{
    feed_tick();
}

void komi_feed_tick_counts(unsigned *ticks, unsigned *submits)
{
    *ticks = feed_ticks;
    *submits = feed_tick_submits;
}

static KomiOrientation orientation = KOMI_LANDSCAPE;
static uint16_t portrait_canvas[KOMI_PORTRAIT_STRIDE * KOMI_PORTRAIT_HEIGHT]
    __attribute__((aligned(64)));

void komi_set_orientation(KomiOrientation value)
{
    orientation = value;
}

KomiOrientation komi_orientation(void)
{
    return orientation;
}

void komi_canvas_steps(long *step_x, long *step_y, long *origin)
{
    if (orientation == KOMI_PORTRAIT_DIRECT) {
        *step_x = KOMI_VRAM_STRIDE;
        *step_y = -1;
        *origin = KOMI_SCREEN_WIDTH - 1;
    } else {
        *step_x = 1;
        *step_y = orientation == KOMI_PORTRAIT_CANVAS
            ? KOMI_PORTRAIT_STRIDE : KOMI_VRAM_STRIDE;
        *origin = 0;
    }
}

uint16_t *komi_canvas(void)
{
    return orientation == KOMI_PORTRAIT_CANVAS
        ? portrait_canvas : psp_display_back_buffer(&komi.display);
}

uint16_t komi_canvas_read(const uint16_t *surface, int x, int y)
{
    long step_x, step_y, origin;
    komi_canvas_steps(&step_x, &step_y, &origin);
    return surface[origin + x * step_x + y * step_y];
}

static void rotate_into(uint16_t *vram)
{
    enum { TILE = 16 };
    for (int ty = 0; ty < KOMI_SCREEN_HEIGHT; ty += TILE) {
        feed_tick();
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

static uint64_t rotate_last_us, publish_last_us;

void komi_canvas_present(void)
{
    uint64_t started = komi_now_us();
    if (orientation == KOMI_PORTRAIT_CANVAS) {
        uint16_t *vram = psp_display_back_buffer(&komi.display);
        if (vram == NULL) return;
        rotate_into(vram);
    }
    uint64_t rotated = komi_now_us();
    (void) psp_display_publish(&komi.display);
    rotate_last_us = rotated - started;
    publish_last_us = komi_now_us() - rotated;
}

static void stage_add(KomiPlayback *state, int stage, uint64_t us)
{
    state->stage_us[stage] += us;
    if (us > state->stage_max_us[stage]) state->stage_max_us[stage] = us;
}

void komi_playback_stages(const KomiPlayback *state, char *out, size_t size)
{
    static const char *const names[6] = {
        "clear", "picture", "overlay", "feed", "rotate", "publish"};
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < 6 && used < size; i++) {
        int n = snprintf(out + used, size - used, "%s%s=%llu/%llu",
                         i == 0 ? "" : " ", names[i],
                         (unsigned long long) (state->draws == 0 ? 0
                             : state->stage_us[i] / state->draws),
                         (unsigned long long) state->stage_max_us[i]);
        if (n < 0) break;
        used += (size_t) n;
    }
    if (used < size)
        snprintf(out + used, size - used, " advance-max=%llu gaps=%u",
                 (unsigned long long) state->advance_max_us, state->gaps);
}

void komi_clear_screen(void)
{
    uint16_t *vram = psp_display_back_buffer(&komi.display);
    if (vram == NULL) return;
    memset(vram, 0,
           (size_t) KOMI_VRAM_STRIDE * KOMI_SCREEN_HEIGHT * sizeof(*vram));
    (void) psp_display_publish(&komi.display);
}

static inline uint16_t rgba_to_565(uint32_t pixel)
{
    return (uint16_t) (((pixel >> 3) & 0x1Fu) | ((pixel >> 5) & 0x7E0u)
                       | ((pixel >> 8) & 0xF800u));
}

/* Fill portrait canvas box (x, y, w, h) of the back buffer, through the
   rotation: a canvas column is a back buffer row. */
static void fill_rotated(uint16_t *vram, int x, int y, int width, int height,
                         uint16_t color)
{
    for (int column = x; column < x + width; column++) {
        uint16_t *out = vram + (size_t) column * KOMI_VRAM_STRIDE
            + (KOMI_SCREEN_WIDTH - y - height);
        for (int i = 0; i < height; i++) out[i] = color;
    }
}

/* Scale the frame into the portrait video rectangle and rotate it, 16
   canvas rows at a time. Each strip is scaled like psp_media_scale does --
   reading source rows front to back -- into a small buffer that stays in
   the data cache, then written out as back buffer rows: canvas column x is
   back buffer row x, canvas row y is back buffer column 479 - y.
   (Walking the source down its columns instead, in one pass, was 5x slower
   on a PSP-3000: rows 2 KiB apart alias onto a few cache sets.) */
#define STRIP_ROWS 16
static uint16_t strip[STRIP_ROWS][KOMI_PORTRAIT_WIDTH]
    __attribute__((aligned(64)));

static void blit_rotated(const MediaVideoFrame *frame,
                         const PspMediaPresentPlan *plan, uint16_t *vram)
{
    const PspMediaScaleMap *map = &scale_map;
    const uint8_t *source = frame->pixels;
    size_t row_bytes = (size_t) frame->stride_pixels * 4u;
    int width = map->output_width;
    for (int y0 = 0; y0 < map->output_height; y0 += STRIP_ROWS) {
        int rows = map->output_height - y0 < STRIP_ROWS
            ? map->output_height - y0 : STRIP_ROWS;
        for (int j = 0; j < rows; j++) {
            int y = y0 + j;
            if (j > 0 && map->row_repeats[y]) {
                memcpy(strip[j], strip[j - 1], (size_t) width * 2u);
                continue;
            }
            const uint8_t *row = source + map->row_index[y] * row_bytes;
            uint16_t *out = strip[j];
            for (int x = 0; x < width; x++)
                out[x] = rgba_to_565(*(const uint32_t *) (const void *)
                                         (row + map->column_offset[x]));
        }
        /* Canvas rows y0..y0+rows-1 are back buffer columns
           479-y..480-y0-rows: fill each back buffer row right to left. */
        for (int x = 0; x < width; x++) {
            uint16_t *out = vram
                + (size_t) (plan->video.x + x) * KOMI_VRAM_STRIDE
                + (KOMI_SCREEN_WIDTH - 1 - plan->video.y - y0);
            for (int j = 0; j < rows; j++) *out-- = strip[j][x];
        }
        feed_tick();
    }
}

/* --- the graphics engine path for direct portrait --- */

/* The GE scales, rotates and converts the decoded 8888 surface into the
   16-bit back buffer: about nothing on the CPU (the software strip path costs
   15-18 ms a frame, which starved the codec feed), and bilinear on top. */
static bool ge_enabled = true;
static int ge_state; /* 0 untried, 1 ready, -1 failed */
static unsigned int __attribute__((aligned(64))) ge_list[4096];
static uint64_t ge_wait_us, ge_wait_max_us;
static unsigned ge_draws;

void komi_set_portrait_ge(bool enabled)
{
    ge_enabled = enabled;
}

bool komi_portrait_ge_active(void)
{
    return ge_enabled && ge_state >= 0;
}

void komi_portrait_ge_counts(unsigned *draws, uint64_t *wait_us,
                             uint64_t *wait_max_us)
{
    *draws = ge_draws;
    *wait_us = ge_wait_us;
    *wait_max_us = ge_wait_max_us;
}

static bool ge_ready(void)
{
    if (ge_state == 0) {
        /* sceGuInit claims the GE only; scanout stays with psp_display
           (no sceGuDispBuffer/sceGuDisplay), as tilefinch's presenter. */
        ge_state = sceGuInit() < 0 ? -1 : 1;
        komi_result("portrait ge %s", ge_state > 0 ? "ready" : "FAILED");
    }
    return ge_state > 0;
}

typedef struct {
    float u, v;
    float x, y, z;
} GeVertex;

/* Where the staged texture goes: EDRAM after the display's three 16-bit
   page buffers (psp_display.h's page-mode plan ends at 0x0CC000; video mode,
   which lays EDRAM out differently, is never entered by these programs). The
   GE reads EDRAM an order of magnitude faster than main memory. */
#define GE_STAGE_OFFSET 0x0CC000u
#define GE_STAGE_LIMIT 0x200000u
static uint64_t ge_stage_us, ge_stage_max_us;

void komi_portrait_ge_stage_counts(uint64_t *stage_us, uint64_t *stage_max_us)
{
    *stage_us = ge_stage_us;
    *stage_max_us = ge_stage_max_us;
}

/* Copy the decoded surface into EDRAM with the DMA controller (the main
   thread blocks meanwhile, which lets the codec worker run). NULL when it
   does not fit or the copy fails: then the GE reads main memory. */
static const void *ge_stage(const MediaVideoFrame *frame)
{
    size_t bytes = (size_t) frame->stride_pixels * 4u
        * (size_t) frame->height;
    if (GE_STAGE_OFFSET + bytes > GE_STAGE_LIMIT) return NULL;
    void *destination = (uint8_t *) sceGeEdramGetAddr() + GE_STAGE_OFFSET;
    uint64_t started = komi_now_us();
    feed_tick();
    /* Drop CPU lines over the destination so no later writeback lands on
       the copy. The source is clean: the backend invalidates the CSC
       surface after conversion. */
    sceKernelDcacheWritebackInvalidateRange(destination, (unsigned) bytes);
    bool copied = sceDmacMemcpy(destination, frame->pixels,
                                (SceSize) bytes) >= 0;
    uint64_t took = komi_now_us() - started;
    ge_stage_us += took;
    if (took > ge_stage_max_us) ge_stage_max_us = took;
    return copied ? destination : NULL;
}

static unsigned pow2_at_least(int value)
{
    unsigned result = 1;
    while (result < (unsigned) value) result <<= 1;
    return result;
}

/* Draw the frame into canvas rectangle `video` of the back buffer, rotated.
   Canvas point (x, y) is panel point (480 - y, x). Waits for the GE. */
static bool blit_rotated_ge(const MediaVideoFrame *frame,
                            const PspMediaPresentRect *video, uint16_t *vram)
{
    unsigned tex_width = pow2_at_least(frame->width);
    unsigned tex_height = pow2_at_least(frame->height);
    if (tex_width > 512u || tex_height > 512u
        || frame->stride_pixels > 1024) return false;
    if (!ge_ready()) return false;
    uintptr_t base = (uintptr_t) sceGeEdramGetAddr() & 0x1fffffffu;
    uintptr_t target = (uintptr_t) vram & 0x1fffffffu;
    if (target < base || target - base >= 0x00200000u) {
        ge_state = -1;
        komi_result("portrait ge FAILED back buffer outside EDRAM");
        return false;
    }
    /* Nothing the CPU wrote into this buffer earlier may be written back
       over what the GE draws. */
    size_t bytes = (size_t) KOMI_VRAM_STRIDE * KOMI_SCREEN_HEIGHT * 2u;
    sceKernelDcacheWritebackInvalidateRange(vram, (unsigned) bytes);

    sceGuStart(GU_DIRECT, (void *) ((uintptr_t) ge_list | 0x40000000u));
    sceGuDrawBufferList(GU_PSM_5650, (void *) (target - base),
                        KOMI_VRAM_STRIDE);
    sceGuOffset(2048u - KOMI_SCREEN_WIDTH / 2, 2048u - KOMI_SCREEN_HEIGHT / 2);
    sceGuViewport(2048, 2048, KOMI_SCREEN_WIDTH, KOMI_SCREEN_HEIGHT);
    sceGuScissor(0, 0, KOMI_SCREEN_WIDTH, KOMI_SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_CLIP_PLANES);
    sceGuDisable(GU_DITHER);
    sceGuShadeModel(GU_FLAT);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    const void *texels = ge_stage(frame);
    if (texels == NULL) texels = frame->pixels;
    sceGuTexImage(0, (int) tex_width, (int) tex_height, frame->stride_pixels,
                  texels);
    sceGuTexFlush();
    GeVertex *v = sceGuGetMemory(4 * (int) sizeof(GeVertex));
    if (v == NULL) {
        sceGuFinish();
        (void) sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
        return false;
    }
    float x0 = (float) video->x, x1 = (float) (video->x + video->width);
    float y0 = (float) video->y, y1 = (float) (video->y + video->height);
    float w = (float) frame->width, h = (float) frame->height;
    float right = (float) KOMI_SCREEN_WIDTH;
    /* Canvas corners in strip order (x0,y0) (x1,y0) (x0,y1) (x1,y1). */
    v[0] = (GeVertex) {0.0f, 0.0f, right - y0, x0, 0.0f};
    v[1] = (GeVertex) {w, 0.0f, right - y0, x1, 0.0f};
    v[2] = (GeVertex) {0.0f, h, right - y1, x0, 0.0f};
    v[3] = (GeVertex) {w, h, right - y1, x1, 0.0f};
    sceGuDrawArray(GU_TRIANGLE_STRIP,
                   GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   4, NULL, v);
    sceGuFinish();
    /* Blocking here is what lets the codec worker run meanwhile. */
    uint64_t started = komi_now_us();
    feed_tick();
    (void) sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    uint64_t waited = komi_now_us() - started;
    ge_wait_us += waited;
    if (waited > ge_wait_max_us) ge_wait_max_us = waited;
    ge_draws++;
    return true;
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
    /* The rotated blit reads the firmware's 32-bit surfaces only. */
    if (orientation == KOMI_PORTRAIT_DIRECT
        && format != PSP_MEDIA_SCALE_RGBA8888)
        return false;
    /* The GE needs no map: it reads the surface while the slot is held. */
    if (orientation == KOMI_PORTRAIT_DIRECT && format == PSP_MEDIA_SCALE_RGBA8888
        && komi_portrait_ge_active()) {
        if (frame->slot >= 0
            && !media_playback_borrow_video_slot(
                   media->playback, (unsigned) frame->slot, frame->generation))
            return false;
        bool drawn = blit_rotated_ge(frame, &plan.video, vram);
        if (frame->slot >= 0)
            media_playback_release_video_read(media->playback,
                                              (unsigned) frame->slot);
        if (drawn) {
            const PspMediaPresentRect *v = &plan.video;
            fill_rotated(vram, 0, 0, width, v->y, 0);
            fill_rotated(vram, 0, v->y + v->height, width,
                         height - v->y - v->height, 0);
            fill_rotated(vram, 0, v->y, v->x, v->height, 0);
            fill_rotated(vram, v->x + v->width, v->y,
                         width - v->x - v->width, v->height, 0);
            media_playback_note_frame_displayed(
                media->playback, frame, MEDIA_PSP_PRESENT_PATH_GE_DIRECT);
            return true;
        }
        /* Fall through to the CPU path for this frame. */
    }
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
    if (orientation == KOMI_PORTRAIT_DIRECT) {
        blit_rotated(frame, &plan, vram);
        /* Only what the picture leaves uncovered needs clearing. */
        const PspMediaPresentRect *v = &plan.video;
        fill_rotated(vram, 0, 0, width, v->y, 0);
        fill_rotated(vram, 0, v->y + v->height, width,
                     height - v->y - v->height, 0);
        fill_rotated(vram, 0, v->y, v->x, v->height, 0);
        fill_rotated(vram, v->x + v->width, v->y, width - v->x - v->width,
                     v->height, 0);
    } else {
        psp_media_scale_blit(
            &scale_map, frame->pixels,
            vram + (size_t) plan.video.y * (size_t) stride + plan.video.x,
            stride);
    }
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
    /* A long gap between frames starves the decoder feed (audio breaks
       up); note the first few once playing, with what the last frame did. */
    if (state->last_us != 0 && komi.media.ui.playing && elapsed_ms >= 50) {
        state->gaps++;
        if (state->gaps <= 8)
            komi_result("gap %ums at=%llums last-advance=%lluus "
                        "last-draw=%lluus",
                        elapsed_ms,
                        (unsigned long long)
                            (komi.media.ui.current_time_us / 1000u),
                        (unsigned long long) state->last_advance_us,
                        (unsigned long long) state->last_draw_us);
    }
    state->last_us = now;
    (void) psp_media_advance(&komi.media, elapsed_ms, NULL);
    state->last_advance_us = komi_now_us() - now;
    if (state->last_advance_us > state->advance_max_us)
        state->advance_max_us = state->last_advance_us;
    state->last_draw_us = 0;
    bool fresh = komi.media.frame.pixels != NULL
        && komi.media.frame.identity != state->identity;
    if (fresh || redraw) {
        uint16_t *vram = komi_canvas();
        if (vram != NULL) {
            uint64_t t0 = komi_now_us();
            bool portrait = orientation != KOMI_LANDSCAPE;
            int width = portrait ? KOMI_PORTRAIT_WIDTH : KOMI_SCREEN_WIDTH;
            int height = portrait ? KOMI_PORTRAIT_HEIGHT : KOMI_SCREEN_HEIGHT;
            int stride = orientation == KOMI_PORTRAIT_CANVAS
                ? KOMI_PORTRAIT_STRIDE : KOMI_VRAM_STRIDE;
            bool have_picture = komi.media.frame.pixels != NULL;
            /* Direct portrait clears only around the picture (in
               draw_picture); everything else starts from black. */
            if (orientation != KOMI_PORTRAIT_DIRECT || !have_picture)
                memset(vram, 0, (size_t) (orientation == KOMI_PORTRAIT_CANVAS
                                              ? stride * height
                                              : KOMI_VRAM_STRIDE
                                                    * KOMI_SCREEN_HEIGHT)
                                    * sizeof(*vram));
            uint64_t t1 = komi_now_us();
            if (draw_picture(vram, width, height, stride)) {
                if (fresh) state->presented++;
                state->identity = komi.media.frame.identity;
            }
            feed_tick();
            uint64_t t2 = komi_now_us();
            if (overlay != NULL) overlay(vram, context);
            feed_tick();
            uint64_t t3 = komi_now_us();
            (void) psp_media_feed_before_blocking(&komi.media);
            uint64_t t4 = komi_now_us();
            komi_canvas_present();
            uint64_t t5 = komi_now_us();
            stage_add(state, KOMI_STAGE_CLEAR, t1 - t0);
            stage_add(state, KOMI_STAGE_PICTURE, t2 - t1);
            stage_add(state, KOMI_STAGE_OVERLAY, t3 - t2);
            stage_add(state, KOMI_STAGE_FEED, t4 - t3);
            stage_add(state, KOMI_STAGE_ROTATE, rotate_last_us);
            stage_add(state, KOMI_STAGE_PUBLISH, publish_last_us);
            state->draw_us += t5 - t0;
            state->last_draw_us = t5 - t0;
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

void komi_playback_audio_mark(KomiPlayback *state)
{
    media_psp_backend_audio_output_counters(&state->audio_blocks_at_open,
                                            &state->audio_starves_at_open);
    media_psp_backend_audio_gap_counters(&state->audio_gaps_at_open,
                                         &state->audio_gap_us_at_open, NULL);
}

void komi_playback_audio_text(const KomiPlayback *state, char *out,
                              size_t size)
{
    unsigned blocks = 0, starves = 0;
    komi_playback_audio(state, &blocks, &starves);
    uint32_t gaps = 0, gap_us = 0, max_us = 0;
    media_psp_backend_audio_gap_counters(&gaps, &gap_us, &max_us);
    snprintf(out, size, "audio-blocks=%u starves=%u gaps=%u gap-ms=%u "
             "gap-max=%uus", blocks, starves,
             (unsigned) (gaps - state->audio_gaps_at_open),
             (unsigned) ((gap_us - state->audio_gap_us_at_open) / 1000u),
             (unsigned) max_us);
}

void komi_playback_audio(const KomiPlayback *state, unsigned *blocks,
                         unsigned *starves)
{
    uint32_t now_blocks = 0, now_starves = 0;
    media_psp_backend_audio_output_counters(&now_blocks, &now_starves);
    *blocks = now_blocks - state->audio_blocks_at_open;
    *starves = now_starves - state->audio_starves_at_open;
}
