/*
 * komi-player -- stage 1: YouTube playback without the browser.
 *
 * Drives tilefinch's media session (resolver -> range reader -> MP4 demux ->
 * Media Engine decode) directly, with no browser engine, page, or chrome.
 * Launching it runs the whole test unattended: every video ID listed in
 * komi-videos.txt beside the program is opened, played for a while, and
 * closed, and one result line per video goes to komi-player.txt beside the
 * program. Under PSPLink that is host0:, so the log lands on the Mac.
 *
 * Nothing here waits for a button. The runtime's watchdog ends the program
 * if any step stops making progress, so a hang never needs the power switch.
 */
#include <pspkernel.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tilefinch/media_backend.h"
#include "tilefinch/psp_threads.h"

#include "komi_runtime.h"

PSP_MODULE_INFO("komi_player", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_PRIORITY(TILEFINCH_PSP_THREAD_PRIORITY_BROWSER);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(2560);
PSP_HEAP_SIZE_KB(-1);
PSP_HEAP_THRESHOLD_SIZE_KB(2048);

#define OPEN_TIMEOUT_US (60u * 1000u * 1000u)
#define PLAY_US (30u * 1000u * 1000u)
#define MAX_VIDEOS 32

/* komi-player.cfg beside the program: key=value lines, all optional.
     me_high=1       fill the heap below 0x0A000000 before the Media Engine
                     pool is reserved, so every decoder buffer lands in the
                     extended bank (is H.264 able to read it there?)
     play_seconds=N  how long each video plays (default 30)
     max_videos=N    play only the first N of komi-videos.txt */
typedef struct {
    bool me_high;
    unsigned play_seconds;
    unsigned max_videos;
} Config;

static void load_config(const char *path, Config *config)
{
    config->me_high = false;
    config->play_seconds = PLAY_US / 1000000u;
    config->max_videos = MAX_VIDEOS;
    FILE *file = fopen(path, "r");
    if (file == NULL) return;
    char line[128];
    while (fgets(line, sizeof line, file) != NULL) {
        unsigned value = 0;
        if (sscanf(line, "me_high=%u", &value) == 1)
            config->me_high = value != 0;
        else if (sscanf(line, "play_seconds=%u", &value) == 1 && value > 0)
            config->play_seconds = value;
        else if (sscanf(line, "max_videos=%u", &value) == 1 && value > 0
                 && value < MAX_VIDEOS)
            config->max_videos = value;
    }
    fclose(file);
}

static size_t load_videos(const char *path, char ids[][16], size_t limit)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) return 0;
    size_t count = 0;
    char line[128];
    while (count < limit && fgets(line, sizeof line, file) != NULL) {
        char *id = line;
        while (*id == ' ' || *id == '\t') id++;
        if (*id == '#' || *id == '\0' || *id == '\n' || *id == '\r') continue;
        size_t length = strcspn(id, " \t\r\n#");
        if (length != 11) continue;
        memcpy(ids[count], id, length);
        ids[count][length] = '\0';
        count++;
    }
    fclose(file);
    return count;
}

typedef struct {
    unsigned played;
    unsigned failed;
    unsigned baseline_used;
    unsigned peak_used;
} Totals;

static uint64_t play_us = PLAY_US;

static void play_one(const char *id, unsigned index, uint64_t generation,
                     Totals *totals)
{
    PspMediaSession *media = &komi.media;
    char url[96];
    snprintf(url, sizeof url, "https://www.youtube.com/watch?v=%s", id);
    unsigned used_before = komi_heap_used();
    unsigned used_peak = used_before;
    uint64_t started = komi_now_us();
    komi_progress("open");
    bool accepted = psp_media_open_provider_route(media, url, generation);

    KomiPlayback playback = {0};
    uint64_t first_frame_us = 0;
    uint64_t playing_since = 0;
    uint64_t buffering_us = 0;
    uint64_t max_position_us = 0;
    const char *outcome = accepted ? "timeout" : "refused";
    while (accepted) {
        komi_playback_frame(&playback, NULL, NULL, false);
        komi_progress(playing_since == 0 ? "opening" : "playing");
        uint64_t now = komi_now_us();
        unsigned used_now = komi_heap_used();
        if (used_now > used_peak) used_peak = used_now;
        if (playback.presented > 0 && first_frame_us == 0)
            first_frame_us = now;
        if (media->ui.playing && playing_since == 0) playing_since = now;
        if (media->ui.buffering && playing_since != 0) buffering_us += 16667u;
        if (media->ui.current_time_us > max_position_us)
            max_position_us = media->ui.current_time_us;
        if (media->ui.failed) { outcome = "failed"; break; }
        if (media->ui.ended) { outcome = "ended"; break; }
        if (playing_since != 0 && now - playing_since >= play_us) {
            outcome = "ok";
            break;
        }
        if (playing_since == 0 && now - started >= OPEN_TIMEOUT_US) break;
    }
    bool ok = strcmp(outcome, "ok") == 0 || strcmp(outcome, "ended") == 0;
    char title[96];
    snprintf(title, sizeof title, "%s", media->ui.title);
    char status[96];
    snprintf(status, sizeof status, "%s", media->ui.status);
    char format[64];
    snprintf(format, sizeof format, "%dx%d itag=%d/%d%s",
             media->stream.width, media->stream.height, media->stream.itag,
             media->stream.audio_itag,
             media->stream.split_streams ? " split" : "");
    MediaBackendStats stats = {0};
    bool have_stats = psp_media_backend_stats_snapshot(media, &stats);

    komi_playback_close(&playback);
    komi_clear_screen();
    unsigned used_after = komi_heap_used();

    komi_result("video %u id=%s %s outcome=%s first-frame=%llums "
                "position=%llums frames=%u buffering=%llums "
                "heap-used=%u->%u(peak %u) heap-free=%u "
                "decoded=%u dropped=%u audio-dropped=%llu format=%s "
                "title=\"%s\" status=\"%s\"",
                index + 1, id, ok ? "PASS" : "FAIL", outcome,
                (unsigned long long) (first_frame_us == 0
                    ? 0 : (first_frame_us - started) / 1000u),
                (unsigned long long) (max_position_us / 1000u),
                playback.presented,
                (unsigned long long) (buffering_us / 1000u),
                used_before, used_after, used_peak, komi_heap_free(),
                have_stats ? (unsigned) stats.decoded_video_frames : 0u,
                have_stats ? (unsigned) stats.dropped_video_frames : 0u,
                have_stats ? (unsigned long long) stats.dropped_audio_samples
                           : 0ull,
                format, title, status);
    if (ok) totals->played++;
    else totals->failed++;
    if (used_peak > totals->peak_used) totals->peak_used = used_peak;
}

int main(int argc, char **argv)
{
    komi_log_open(argc > 0 ? argv[0] : NULL, "komi-player.txt");
    komi_result("start version=4 argv0=%s heap-used=%u heap-free=%u "
                "kernel-free=%u", komi.argv0 == NULL ? "?" : komi.argv0,
                komi_heap_used(), komi_heap_free(),
                (unsigned) sceKernelTotalFreeMemSize());

    char path[256];
    komi_sibling_path(path, sizeof path, "komi-player.cfg");
    Config config;
    load_config(path, &config);
    play_us = (uint64_t) config.play_seconds * 1000000u;
    komi_result("config me_high=%d play_seconds=%u", config.me_high ? 1 : 0,
                config.play_seconds);
    komi_platform_init(config.me_high);

    komi_sibling_path(path, sizeof path, "komi-videos.txt");
    static char ids[MAX_VIDEOS][16];
    size_t count = load_videos(path, ids, config.max_videos);
    komi_result("videos=%u list=%s", (unsigned) count, path);
    if (count > 0 && komi_services_init()) {
        Totals totals = {.baseline_used = komi_heap_used()};
        totals.peak_used = totals.baseline_used;
        komi_result("ready heap-used=%u heap-free=%u budget=%u",
                    totals.baseline_used, komi_heap_free(),
                    (unsigned) komi.budget.current);
        for (size_t at = 0; at < count; at++)
            play_one(ids[at], (unsigned) at, (uint64_t) at + 1u, &totals);
        komi_result("SUMMARY played=%u failed=%u of=%u heap-used "
                    "baseline=%u end=%u peak=%u budget-now=%u",
                    totals.played, totals.failed, (unsigned) count,
                    totals.baseline_used, komi_heap_used(),
                    totals.peak_used, (unsigned) komi.budget.current);
        psp_media_shutdown(&komi.media);
    }
    komi_result("end");
    komi_log_close("complete");
    sceKernelExitGame();
    return 0;
}
