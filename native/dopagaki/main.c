/*
 * dopagaki-portable -- YouTube Shorts on a PSP held upright.
 *
 * The PSP is held vertically with the analog stick at the bottom (the left
 * edge of the landscape screen down). Everything is drawn on a 272x480
 * portrait canvas that the runtime rotates into the landscape panel, and the
 * d-pad and stick are read in the same rotated frame, so "down" below means
 * the d-pad button nearest the bottom of the upright PSP (landscape LEFT).
 *
 * Shorts play one after another like the phone app. The feed is either
 * おすすめ (the Shorts tab), a search restricted to Shorts, or the マイリスト
 * (Shorts liked on this PSP). The confirm/back buttons follow the system
 * setting; OK/BACK below mean those.
 *
 *   player   down/up next/previous   OK pause   △ like   left/right -/+5 s
 *            START search            SELECT or BACK menu
 *   menu     up/down choose          OK open    BACK return to the player
 *   convert  up/down choose          OK search  △ edit    BACK cancel
 *
 * dopagaki.cfg beside the program runs an unattended check instead:
 *   autotest=1, autotest_query=<UTF-8>, autotest_play_seconds=N
 * Lines go to dopagaki.txt and screenshots (the portrait canvas) to shot-*.bmp.
 */
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psputility.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/fetch.h"
#include "tilefinch/psp_threads.h"

#include "kanji.h"
#include "komi_runtime.h"
#include "mylist.h"
#include "osk.h"
#include "shorts.h"
#include "ui.h"

PSP_MODULE_INFO("dopagaki", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_PRIORITY(TILEFINCH_PSP_THREAD_PRIORITY_BROWSER);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(2560);
PSP_HEAP_SIZE_KB(-1);
PSP_HEAP_THRESHOLD_SIZE_KB(2048);

#define W KOMI_PORTRAIT_WIDTH
#define H KOMI_PORTRAIT_HEIGHT
#define TOP_BAR 26
#define OVERLAY_US (3u * 1000u * 1000u)
#define TOAST_US (2u * 1000u * 1000u)
#define OPEN_TIMEOUT_US (45u * 1000u * 1000u)
#define SEEK_STEP_US (5u * 1000u * 1000u)
#define FEED_CAPACITY 200
#define BATCH_CAPACITY 40
/* Fetch more when this few Shorts are left after the current one. */
#define FEED_LOW_WATER 2
#define MAX_FAILURES_IN_A_ROW 5
#define CONVERT_ROWS 20
#define MENU_ROW 52

/* Logical (portrait) directions, rebuilt from the landscape buttons. */
#define KEY_UP 0x10000000u
#define KEY_DOWN 0x20000000u
#define KEY_LEFT 0x40000000u
#define KEY_RIGHT 0x80000000u
#define KEY_DIRECTIONS (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)

typedef enum { FEED_RECOMMENDED, FEED_SEARCH, FEED_MYLIST } FeedKind;

typedef struct {
    FeedKind kind;
    char query[256];
    ShortItem items[FEED_CAPACITY];
    size_t count;
    size_t index;
    char token[SHORT_TOKEN_SIZE];
    /* The last fetch returned nothing new: stop asking. */
    bool exhausted;
} Feed;

typedef struct {
    uint32_t buttons;
    uint32_t pressed;
    uint64_t held_since;
    uint64_t last_repeat;
    char toast[128];
    uint64_t toast_until;
    uint64_t generation;
} App;

static App app;
static Feed feed;
static Mylist mylist;
static Budget ui_budget;
static ShortItem batch_items[BATCH_CAPACITY];

/* Confirm/back, from the system setting. */
static uint32_t BTN_OK = PSP_CTRL_CIRCLE;
static uint32_t BTN_BACK = PSP_CTRL_CROSS;
static const char *ICON_OK = UI_ICON_CIRCLE;
static const char *ICON_BACK = UI_ICON_CROSS;

static void read_button_setting(void)
{
    int accept = 0;
    int result = sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN,
                                             &accept);
    /* Setting 9 is the X/O accept choice (PSPSDK names it UNKNOWN):
       0 = ○ confirms, 1 = × confirms. */
    if (result >= 0 && accept == 1) {
        BTN_OK = PSP_CTRL_CROSS;
        BTN_BACK = PSP_CTRL_CIRCLE;
        ICON_OK = UI_ICON_CROSS;
        ICON_BACK = UI_ICON_CIRCLE;
    }
    komi_result("buttons confirm=%s (setting=%d result=0x%08x)",
                BTN_OK == PSP_CTRL_CROSS ? "cross" : "circle", accept,
                (unsigned) result);
}

/* --- input, in the upright frame --- */

/* Upright with the stick at the bottom: landscape RIGHT points up, LEFT
   down, UP left and DOWN right. The stick counts as the d-pad. */
static uint32_t upright_buttons(const SceCtrlData *pad)
{
    uint32_t raw = pad->Buttons;
    uint32_t keys = raw & ~(uint32_t) (PSP_CTRL_UP | PSP_CTRL_DOWN
                                       | PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
    if ((raw & PSP_CTRL_RIGHT) || pad->Lx > 208) keys |= KEY_UP;
    if ((raw & PSP_CTRL_LEFT) || pad->Lx < 48) keys |= KEY_DOWN;
    if ((raw & PSP_CTRL_UP) || pad->Ly < 48) keys |= KEY_LEFT;
    if ((raw & PSP_CTRL_DOWN) || pad->Ly > 208) keys |= KEY_RIGHT;
    return keys;
}

/* Keys newly pressed this frame; held up/down repeat for lists. */
static void read_input(bool repeat)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    uint32_t now = upright_buttons(&pad);
    app.pressed = now & ~app.buttons;
    uint32_t repeatable = KEY_UP | KEY_DOWN;
    uint64_t t = komi_now_us();
    if (repeat && (now & repeatable) != 0
        && (now & repeatable) == (app.buttons & repeatable)) {
        if (t - app.held_since > 400000u && t - app.last_repeat > 90000u) {
            app.pressed |= now & repeatable;
            app.last_repeat = t;
        }
    } else {
        app.held_since = t;
    }
    app.buttons = now;
}

static bool pressed(uint32_t key)
{
    return (app.pressed & key) != 0;
}

static void toast(const char *text)
{
    snprintf(app.toast, sizeof app.toast, "%s", text);
    app.toast_until = komi_now_us() + TOAST_US;
    komi_result("toast \"%s\"", text);
}

/* --- screenshots (autotest): the portrait canvas as a 24-bit BMP --- */

static void screenshot(const uint16_t *canvas, const char *name)
{
    char path[256];
    komi_sibling_path(path, sizeof path, name);
    FILE *file = fopen(path, "wb");
    if (file == NULL) return;
    const unsigned row = W * 3u;
    const unsigned size = 54u + row * H;
    unsigned char header[54] = {'B', 'M'};
    const struct { unsigned at, value; } fields[] = {
        {2, size}, {10, 54}, {14, 40}, {18, W}, {22, H}, {34, row * H},
    };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++)
        for (unsigned b = 0; b < 4; b++)
            header[fields[i].at + b] =
                (unsigned char) (fields[i].value >> (8u * b));
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, sizeof header, file);
    static unsigned char line[W * 3];
    for (int y = H - 1; y >= 0; y--) {
        for (int x = 0; x < W; x++) {
            uint16_t p = komi_canvas_read(canvas, x, y);
            line[x * 3 + 0] = (unsigned char) (((p >> 11) & 31u) << 3);
            line[x * 3 + 1] = (unsigned char) (((p >> 5) & 63u) << 2);
            line[x * 3 + 2] = (unsigned char) ((p & 31u) << 3);
        }
        fwrite(line, 1, sizeof line, file);
    }
    fclose(file);
    komi_result("screenshot %s", name);
}

/* --- drawing helpers --- */

static uint16_t *begin_frame(void)
{
    uint16_t *canvas = komi_canvas();
    if (canvas != NULL) ui_fill(canvas, 0, 0, W, H, UI_BACKGROUND);
    return canvas;
}

static void draw_centered(uint16_t *canvas, int y, const char *text,
                          uint16_t color)
{
    int width = ui_text_width(text, 16);
    if (width > W - 16) width = W - 16;
    ui_text(canvas, (W - width) / 2, y, W - 8, text, 16, color);
}

static void draw_toast(uint16_t *canvas)
{
    if (app.toast[0] == '\0' || komi_now_us() > app.toast_until) return;
    int width = ui_text_width(app.toast, 16) + 24;
    if (width > W - 16) width = W - 16;
    int x = (W - width) / 2;
    int y = H / 2 - 14;
    ui_fill(canvas, x, y, width, 28, UI_ROW_SELECTED);
    ui_text(canvas, x + 12, y + 6, x + width - 12, app.toast, 16, UI_TEXT);
}

static void draw_top_bar(uint16_t *canvas, const char *title)
{
    ui_fill(canvas, 0, 0, W, TOP_BAR, UI_BAR);
    int x = ui_text(canvas, 8, 5, 120, "dopagaki", 16, UI_ACCENT);
    ui_text(canvas, x + 10, 5, W - 8, title, 16, UI_TEXT);
}

static void show_busy(const char *line)
{
    uint16_t *canvas = begin_frame();
    if (canvas == NULL) return;
    draw_top_bar(canvas, "");
    draw_centered(canvas, H / 2 - 8, line, UI_TEXT);
    komi_canvas_present();
}

static const char *feed_label(void)
{
    static char label[300];
    switch (feed.kind) {
    case FEED_RECOMMENDED: return "おすすめ";
    case FEED_MYLIST: return "マイリスト";
    case FEED_SEARCH:
    default:
        snprintf(label, sizeof label, "「%s」", feed.query);
        return label;
    }
}

/* --- the feed --- */

static void feed_reset(FeedKind kind, const char *query)
{
    memset(&feed, 0, sizeof feed);
    feed.kind = kind;
    snprintf(feed.query, sizeof feed.query, "%s", query ? query : "");
}

/* Append a batch, skipping Shorts already in the feed. When the feed is
   full, the oldest half (all behind the current Short) is dropped. */
static size_t feed_append(const ShortBatch *batch)
{
    size_t added = 0;
    for (size_t i = 0; i < batch->count; i++) {
        bool seen = false;
        for (size_t j = 0; j < feed.count && !seen; j++)
            seen = strcmp(feed.items[j].id, batch->items[i].id) == 0;
        if (seen) continue;
        if (feed.count == FEED_CAPACITY) {
            size_t drop = feed.index < FEED_CAPACITY / 2
                ? feed.index : FEED_CAPACITY / 2;
            if (drop == 0) break;
            memmove(&feed.items[0], &feed.items[drop],
                    (feed.count - drop) * sizeof feed.items[0]);
            feed.count -= drop;
            feed.index -= drop;
        }
        feed.items[feed.count++] = batch->items[i];
        added++;
    }
    return added;
}

/* One more batch for the current feed. False when nothing was added. */
static bool feed_fetch_more(void)
{
    if (feed.kind == FEED_MYLIST || feed.exhausted) return false;
    ShortBatch batch = {batch_items, BATCH_CAPACITY, 0, ""};
    bool ok;
    komi_progress("feed");
    if (feed.count == 0 && feed.kind == FEED_RECOMMENDED) {
        ok = shorts_fetch_seedless(&batch);
        /* The seedless answer names one Short; the sequence has the rest. */
        size_t added = ok ? feed_append(&batch) : 0;
        snprintf(feed.token, sizeof feed.token, "%s", batch.token);
        if (ok && feed.token[0] != '\0') {
            komi_progress("feed");
            ok = shorts_fetch_sequence(feed.token, &batch);
            if (ok) {
                added += feed_append(&batch);
                snprintf(feed.token, sizeof feed.token, "%s", batch.token);
            }
        }
        return added > 0;
    }
    if (feed.count == 0) {
        ok = shorts_fetch_search(feed.query, &batch);
    } else if (feed.token[0] == '\0') {
        feed.exhausted = true;
        return false;
    } else if (feed.kind == FEED_RECOMMENDED) {
        ok = shorts_fetch_sequence(feed.token, &batch);
    } else {
        ok = shorts_fetch_search_more(feed.token, &batch);
    }
    if (!ok) return false;
    snprintf(feed.token, sizeof feed.token, "%s", batch.token);
    size_t added = feed_append(&batch);
    if (added == 0 && feed.token[0] == '\0') feed.exhausted = true;
    return added > 0;
}

static bool feed_load_first(void)
{
    show_busy(feed.kind == FEED_SEARCH ? "検索しています..."
                                       : "ショートを集めています...");
    if (feed.kind == FEED_MYLIST) {
        for (size_t i = 0; i < mylist.count && i < FEED_CAPACITY; i++) {
            ShortItem *item = &feed.items[feed.count++];
            memset(item, 0, sizeof *item);
            snprintf(item->id, sizeof item->id, "%s", mylist.items[i].id);
            snprintf(item->title, sizeof item->title, "%s",
                     mylist.items[i].title);
            snprintf(item->channel, sizeof item->channel, "%s",
                     mylist.items[i].channel);
        }
        return feed.count > 0;
    }
    /* A thin first answer is topped up once. */
    bool ok = feed_fetch_more();
    if (ok && feed.count < 4) (void) feed_fetch_more();
    return feed.count > 0;
}

/* --- like --- */

static bool liked(const ShortItem *item)
{
    return mylist_find(&mylist, item->id) >= 0;
}

static void toggle_like(ShortItem *item)
{
    if (liked(item)) {
        mylist_remove(&mylist, item->id);
        toast("マイリストから外しました");
        return;
    }
    YtVideo video;
    memset(&video, 0, sizeof video);
    snprintf(video.id, sizeof video.id, "%s", item->id);
    snprintf(video.title, sizeof video.title, "%s", item->title);
    snprintf(video.channel, sizeof video.channel, "%s", item->channel);
    snprintf(video.duration, sizeof video.duration, "ショート");
    toast(mylist_add(&mylist, &video) ? "♥ マイリストに追加しました"
                                      : "保存できませんでした");
}

/* --- player --- */

typedef struct {
    ShortItem *item;
    uint64_t overlay_until;
} PlayerView;

static void format_time(char *out, size_t size, uint64_t us)
{
    unsigned seconds = (unsigned) (us / 1000000u);
    snprintf(out, size, "%u:%02u", seconds / 60u, seconds % 60u);
}

static void player_overlay(uint16_t *canvas, void *context)
{
    PlayerView *view = context;
    const PspUiMediaState *ui = &komi.media.ui;
    ShortItem *item = view->item;
    /* Sequence entries carry no title; the player learns it on open. */
    if (item->title[0] == '\0' && komi.media.stream.title[0] != '\0'
        && strcmp(komi.media.stream.video_id, item->id) == 0)
        snprintf(item->title, sizeof item->title, "%s",
                 komi.media.stream.title);
    uint64_t now = komi_now_us();
    bool loading = !ui->playing && !ui->failed && !ui->ended
        && komi.media.frame.pixels == NULL;
    bool paused = !loading && !ui->playing && !ui->buffering && !ui->failed;
    bool full = now < view->overlay_until || loading || paused || ui->failed;

    /* Always: a thin progress line at the very bottom. */
    if (ui->duration_us > 0) {
        int filled = (int) ((uint64_t) W * ui->current_time_us
                            / ui->duration_us);
        ui_fill(canvas, 0, H - 3, W, 3, UI_TEXT_MUTED);
        ui_fill(canvas, 0, H - 3, filled, 3, UI_ACCENT);
    }
    if (loading) draw_centered(canvas, H / 2 - 8, "読み込み中...", UI_TEXT);
    if (ui->failed)
        draw_centered(canvas, H / 2 - 8, "再生できませんでした", UI_TEXT);
    if (paused) {
        char text[32];
        snprintf(text, sizeof text, "%s 一時停止中", UI_ICON_PAUSE);
        draw_centered(canvas, H / 2 - 8, text, UI_TEXT);
    }

    /* Bottom: title (two lines), channel, like mark. */
    int bottom_top = H - 70;
    ui_shade(canvas, 0, bottom_top, W, 67);
    int x = 8;
    if (liked(item)) x = ui_text(canvas, x, bottom_top + 6, W - 8, "♥ ", 16,
                                 UI_LIKED);
    const char *title = item->title[0] != '\0' ? item->title : "";
    ui_text_wrap(canvas, x, bottom_top + 6, W - 8, title, 16, UI_TEXT, 19,
                 2);
    if (item->channel[0] != '\0')
        ui_text(canvas, 8, bottom_top + 45, W - 8, item->channel, 16,
                UI_TEXT_MUTED);

    if (full) {
        ui_shade(canvas, 0, 0, W, TOP_BAR);
        int left = ui_text(canvas, 8, 5, 160, feed_label(), 16, UI_TEXT);
        (void) left;
        char position[48];
        snprintf(position, sizeof position, "%u/%u",
                 (unsigned) feed.index + 1, (unsigned) feed.count);
        int width = ui_text_width(position, 16);
        ui_text(canvas, W - 8 - width, 5, W, position, 16, UI_TEXT_MUTED);
        /* Time and the controls above the title. */
        ui_shade(canvas, 0, bottom_top - 44, W, 44);
        char now_text[16], total[16], line[64];
        format_time(now_text, sizeof now_text, ui->current_time_us);
        format_time(total, sizeof total, ui->duration_us);
        snprintf(line, sizeof line, "%s %s / %s",
                 ui->playing ? UI_ICON_PLAY : UI_ICON_PAUSE, now_text, total);
        ui_text(canvas, 8, bottom_top - 42, W - 8, line, 16, UI_TEXT);
        char hint[160];
        snprintf(hint, sizeof hint, "上下 次へ %s 停止 " UI_ICON_TRIANGLE
                 " いいね", ICON_OK);
        ui_text(canvas, 8, bottom_top - 22, W - 4, hint, 16, UI_TEXT_MUTED);
    }
    draw_toast(canvas);
}

typedef enum {
    PLAY_NEXT,
    PLAY_PREVIOUS,
    PLAY_MENU,
    PLAY_SEARCH,
    PLAY_FAILED
} PlayExit;

typedef struct {
    bool active;
    uint64_t play_us;
    bool like;
    const char *shot;
} AutoPlay;

/* Play feed.items[feed.index] until the user moves on (or it ends, which
   moves on too). */
static PlayExit play_current(const AutoPlay *automatic)
{
    ShortItem *item = &feed.items[feed.index];
    char url[96];
    snprintf(url, sizeof url, "https://www.youtube.com/watch?v=%s", item->id);
    komi_result("play %u/%u id=%s title=\"%s\"", (unsigned) feed.index + 1,
                (unsigned) feed.count, item->id, item->title);
    PlayerView view = {item, komi_now_us() + OVERLAY_US};
    KomiPlayback playback = {0};
    uint64_t started = komi_now_us();
    uint64_t playing_since = 0;
    bool shot_taken = false, auto_liked = false;
    PlayExit exit = PLAY_NEXT;
    const char *outcome = "moved";
    komi_playback_audio_mark(&playback);
    bool accepted = psp_media_open_provider_route(&komi.media, url,
                                                  ++app.generation);
    if (!accepted) {
        outcome = "refused";
        exit = PLAY_FAILED;
    }
    app.buttons = ~0u;
    while (accepted) {
        read_input(false);
        uint64_t now = komi_now_us();
        bool redraw = false;
        if (app.pressed != 0) {
            view.overlay_until = now + OVERLAY_US;
            redraw = true;
        }
        if (pressed(KEY_DOWN)) { exit = PLAY_NEXT; break; }
        if (pressed(KEY_UP)) { exit = PLAY_PREVIOUS; break; }
        if (pressed(PSP_CTRL_SELECT) || pressed(BTN_BACK)) {
            exit = PLAY_MENU;
            break;
        }
        if (pressed(PSP_CTRL_START)) { exit = PLAY_SEARCH; break; }
        if (pressed(BTN_OK)) {
            PspUiMediaIntent intent = {
                .action = PSP_UI_MEDIA_ACTION_PLAY_PAUSE};
            psp_media_execute_intent(&komi.media, intent);
        }
        if (pressed(PSP_CTRL_TRIANGLE)) toggle_like(item);
        if (pressed(KEY_LEFT) || pressed(KEY_RIGHT)) {
            uint64_t at = komi.media.ui.current_time_us;
            uint64_t target = pressed(KEY_RIGHT)
                ? at + SEEK_STEP_US
                : (at > SEEK_STEP_US ? at - SEEK_STEP_US : 0);
            if (komi.media.ui.duration_us > 1000000u
                && target >= komi.media.ui.duration_us)
                target = komi.media.ui.duration_us - 1000000u;
            (void) psp_media_request_seek(&komi.media, target, false);
        }
        const PspUiMediaState *ui = &komi.media.ui;
        if (ui->playing && playing_since == 0) {
            playing_since = now;
            komi_result("playing id=%s first-frame=%llums", item->id,
                        (unsigned long long) ((now - started) / 1000u));
        }
        if (now < view.overlay_until || now < app.toast_until || !ui->playing)
            redraw = redraw
                || (now / 250000u) != ((now - 16667u) / 250000u);
        komi_playback_frame(&playback, player_overlay, &view, redraw);
        komi_progress(playing_since == 0 ? "opening" : "playing");
        if (ui->ended) { outcome = "ended"; break; }
        if (ui->failed) {
            outcome = "failed";
            exit = PLAY_FAILED;
            komi_result("play failed status=\"%s\"", ui->status);
            uint64_t until = komi_now_us() + 1500000u;
            while (komi_now_us() < until) {
                komi_playback_frame(&playback, player_overlay, &view, true);
                komi_progress("failed");
            }
            break;
        }
        if (playing_since == 0 && now - started > OPEN_TIMEOUT_US) {
            outcome = "timeout";
            exit = PLAY_FAILED;
            break;
        }
        if (automatic != NULL && automatic->active && playing_since != 0) {
            if (automatic->like && !auto_liked
                && now - playing_since > 2000000u) {
                auto_liked = true;
                toggle_like(item);
                view.overlay_until = now + OVERLAY_US;
            }
            if (!shot_taken && automatic->shot != NULL
                && now - playing_since > 3000000u) {
                /* Draw the overlay over the current picture; the canvas
                   then holds exactly what was shown. */
                komi_playback_frame(&playback, player_overlay, &view, true);
                /* Drawing straight into the back buffer means the frame
                   just shown is now the front one. */
                screenshot(komi_orientation() == KOMI_PORTRAIT_DIRECT
                               ? psp_display_front_buffer(&komi.display)
                               : komi_canvas(),
                           automatic->shot);
                shot_taken = true;
            }
            if (now - playing_since > automatic->play_us) {
                outcome = "auto-next";
                break;
            }
        }
    }
    char stages[260];
    komi_playback_stages(&playback, stages, sizeof stages);
    unsigned audio_blocks = 0, audio_starves = 0;
    komi_playback_audio(&playback, &audio_blocks, &audio_starves);
    MediaBackendStats stats = {0};
    if (psp_media_backend_stats_snapshot(&komi.media, &stats))
        komi_result("stats id=%s format=%dx%d itag=%d/%d decoded=%u "
                    "dropped=%u audio-dropped=%llu stream-audio-blocks=%u",
                    item->id, komi.media.stream.width,
                    komi.media.stream.height, komi.media.stream.itag,
                    komi.media.stream.audio_itag,
                    (unsigned) stats.decoded_video_frames,
                    (unsigned) stats.dropped_video_frames,
                    (unsigned long long) stats.dropped_audio_samples,
                    (unsigned) stats.audio_output_blocks);
    char audio[260];
    komi_playback_audio_text(&playback, audio, sizeof audio);
    unsigned ticks = 0, tick_submits = 0;
    komi_feed_tick_counts(&ticks, &tick_submits);
    size_t audio_length = strlen(audio);
    unsigned ge_draws = 0;
    uint64_t ge_wait = 0, ge_wait_max = 0;
    komi_portrait_ge_counts(&ge_draws, &ge_wait, &ge_wait_max);
    uint64_t ge_stage = 0, ge_stage_max = 0;
    komi_portrait_ge_stage_counts(&ge_stage, &ge_stage_max);
    snprintf(audio + audio_length, sizeof audio - audio_length,
             " feed-ticks=%u/%u ge=%u/%llu/%lluus stage=%llu/%lluus",
             tick_submits, ticks, ge_draws,
             (unsigned long long) (ge_draws == 0 ? 0 : ge_wait / ge_draws),
             (unsigned long long) ge_wait_max,
             (unsigned long long) (ge_draws == 0 ? 0 : ge_stage / ge_draws),
             (unsigned long long) ge_stage_max);
    (void) audio_blocks;
    (void) audio_starves;
    komi_result("play end id=%s outcome=%s %s position=%llums frames=%u "
                "draws=%u draw-mean=%lluus %s heap-used=%u",
                item->id, outcome, audio,
                (unsigned long long) (komi.media.ui.current_time_us / 1000u),
                playback.presented, playback.draws,
                (unsigned long long) (playback.draws == 0 ? 0
                                      : playback.draw_us / playback.draws),
                stages, komi_heap_used());
    komi_playback_close(&playback);
    app.buttons = ~0u; /* ignore whatever is still held */
    return exit;
}

/* The first portrait Short after the Media Engine boots can fail with "no
   packet accepted" while the next open of the same stream plays (PSP-3000,
   2026-09-24), so a failure gets one immediate second try. */
static PlayExit play_current_retrying(const AutoPlay *automatic)
{
    PlayExit exit = play_current(automatic);
    if (exit != PLAY_FAILED) return exit;
    komi_result("retry id=%s", feed.items[feed.index].id);
    return play_current(automatic);
}

/* Move to the next Short, fetching more when the feed runs low. False when
   there is nothing after the current one. */
static bool feed_advance(void)
{
    if (feed.index + 1 + FEED_LOW_WATER >= feed.count)
        (void) feed_fetch_more();
    if (feed.index + 1 >= feed.count) return false;
    feed.index++;
    return true;
}

/* --- kana to kanji (as komi-tube, drawn upright) --- */

typedef struct {
    char rows[CONVERT_ROWS][KANJI_TEXT * 2];
    bool suggestion[CONVERT_ROWS];
    size_t count;
} Candidates;

static bool fetch_text(const char *url, char *out, size_t size,
                       size_t *length)
{
    FetchResult result = {0};
    bool ok = fetch_url(&komi.budget, url, 64u * 1024u, 10000, &result)
        && result.status_code == 200 && result.data != NULL;
    *length = 0;
    if (ok) {
        *length = result.length < size - 1 ? result.length : size - 1;
        memcpy(out, result.data, *length);
        out[*length] = '\0';
    }
    komi_result("fetch %s status=%ld length=%u url=%.120s",
                ok ? "OK" : "FAIL", result.status_code,
                (unsigned) result.length, url);
    fetch_result_free(&result);
    return ok;
}

static void add_candidate(Candidates *c, const char *text, bool suggestion)
{
    if (text[0] == '\0' || c->count == CONVERT_ROWS) return;
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->rows[i], text) == 0) return;
    snprintf(c->rows[c->count], sizeof c->rows[0], "%s", text);
    c->suggestion[c->count] = suggestion;
    c->count++;
}

/* Whole-phrase conversions, YouTube's suggestions for the best one, and
   finally the text as typed. */
static void build_candidates(const char *text, Candidates *c)
{
    memset(c, 0, sizeof *c);
    show_busy("漢字に変換しています...");
    static char body[16384];
    size_t length = 0;
    char encoded[768];
    char url[1024];
    KanjiConversion conversion;
    bool converted = false;
    if (kanji_url_encode(text, encoded, sizeof encoded)) {
        snprintf(url, sizeof url,
                 "https://www.google.com/transliterate?langpair=ja-Hira%%7Cja"
                 "&text=%s", encoded);
        converted = fetch_text(url, body, sizeof body, &length)
            && kanji_parse_transliteration(body, length, &conversion);
    }
    char best[KANJI_TEXT * 2] = {0};
    if (converted) {
        for (size_t s = 0; s < conversion.count; s++) {
            const KanjiSegment *segment = &conversion.segments[s];
            strncat(best, segment->count > 0 ? segment->candidates[0]
                                             : segment->reading,
                    sizeof best - strlen(best) - 1);
        }
        add_candidate(c, best, false);
        for (size_t s = 0; s < conversion.count; s++) {
            for (size_t k = 1; k < conversion.segments[s].count; k++) {
                char phrase[KANJI_TEXT * 2] = {0};
                for (size_t t = 0; t < conversion.count; t++) {
                    const KanjiSegment *segment = &conversion.segments[t];
                    const char *part = t == s ? segment->candidates[k]
                        : segment->count > 0 ? segment->candidates[0]
                        : segment->reading;
                    strncat(phrase, part, sizeof phrase - strlen(phrase) - 1);
                }
                if (c->count < CONVERT_ROWS / 2)
                    add_candidate(c, phrase, false);
            }
        }
    }
    const char *basis = best[0] != '\0' ? best : text;
    if (kanji_url_encode(basis, encoded, sizeof encoded)) {
        snprintf(url, sizeof url,
                 "https://suggestqueries.google.com/complete/search?"
                 "client=firefox&ds=yt&hl=ja&q=%s", encoded);
        static char suggestions[KANJI_SUGGESTIONS][KANJI_TEXT];
        size_t count = fetch_text(url, body, sizeof body, &length)
            ? kanji_parse_suggestions(body, length, suggestions,
                                      KANJI_SUGGESTIONS)
            : 0;
        for (size_t i = 0; i < count; i++)
            add_candidate(c, suggestions[i], true);
    }
    add_candidate(c, text, false);
    komi_result("convert text=\"%s\" converted=%d candidates=%u best=\"%s\"",
                text, converted, (unsigned) c->count, best);
}

/* 1 = search with out, 2 = edit out in the keyboard, 0 = cancelled. */
static int choose_candidate(const char *text, char *out, size_t size)
{
    static Candidates c;
    build_candidates(text, &c);
    enum { ROW = 32, LIST_TOP = TOP_BAR + 4, LIST_ROWS = (H - 60) / ROW };
    int selection = 0;
    app.buttons = ~0u;
    for (;;) {
        komi_progress("convert");
        read_input(true);
        if (pressed(KEY_UP) && selection > 0) selection--;
        if (pressed(KEY_DOWN) && selection + 1 < (int) c.count) selection++;
        uint16_t *canvas = begin_frame();
        if (canvas != NULL) {
            draw_top_bar(canvas, "変換");
            int first = selection - LIST_ROWS / 2;
            if (first > (int) c.count - LIST_ROWS)
                first = (int) c.count - LIST_ROWS;
            if (first < 0) first = 0;
            for (int row = 0; row < LIST_ROWS && first + row < (int) c.count;
                 row++) {
                int at = first + row;
                int y = LIST_TOP + row * ROW;
                if (at == selection)
                    ui_fill(canvas, 0, y, W, ROW, UI_ROW_SELECTED);
                const char *tag = at == (int) c.count - 1 ? "入力"
                    : c.suggestion[at] ? "候補" : "変換";
                int width = ui_text_width(tag, 16);
                ui_text(canvas, 10, y + 8, W - 16 - width, c.rows[at], 16,
                        UI_TEXT);
                ui_text(canvas, W - 8 - width, y + 8, W, tag, 16,
                        UI_TEXT_MUTED);
            }
            char hint[160];
            snprintf(hint, sizeof hint, "%s 検索 " UI_ICON_TRIANGLE
                     " 修正 %s やめる", ICON_OK, ICON_BACK);
            ui_fill(canvas, 0, H - 26, W, 26, UI_BAR);
            ui_text(canvas, 8, H - 22, W - 4, hint, 16, UI_TEXT_MUTED);
            komi_canvas_present();
        }
        if (pressed(BTN_BACK)) return 0;
        if (pressed(BTN_OK) || pressed(PSP_CTRL_TRIANGLE)) {
            snprintf(out, size, "%s", c.rows[selection]);
            return pressed(BTN_OK) ? 1 : 2;
        }
    }
}

/* Keyboard, then kana-to-kanji candidates. False when cancelled. */
static bool ask_query(char *query, size_t size)
{
    char text[256];
    snprintf(text, sizeof text, "%s",
             feed.kind == FEED_SEARCH ? feed.query : "");
    for (;;) {
        char typed[256];
        /* The firmware keyboard draws landscape: turn the PSP sideways. */
        if (!osk_input("ショートを検索（ひらがなで入力すると漢字の候補が出ます）",
                       text, typed, sizeof typed))
            return false;
        komi_result("osk text=\"%s\"", typed);
        if (typed[0] == '\0') return false;
        if (!kanji_has_hiragana(typed)) {
            snprintf(query, size, "%s", typed);
            return true;
        }
        char chosen[256];
        int choice = choose_candidate(typed, chosen, sizeof chosen);
        if (choice == 0) return false;
        if (choice == 1) {
            snprintf(query, size, "%s", chosen);
            return true;
        }
        snprintf(text, sizeof text, "%s", chosen);
    }
}

/* --- menu --- */

typedef enum {
    MENU_RETURN,
    MENU_RECOMMENDED,
    MENU_SEARCH,
    MENU_MYLIST,
    MENU_QUIT
} MenuChoice;

static void draw_menu(uint16_t *canvas, int selection, bool can_return)
{
    draw_top_bar(canvas, "メニュー");
    char mylist_label[64];
    snprintf(mylist_label, sizeof mylist_label, "マイリスト（%u本）",
             (unsigned) mylist.count);
    const char *entries[] = {
        can_return ? "続きを見る" : "（まだ何も見ていません）",
        "おすすめのショート", "ショートを検索", mylist_label, "終了"};
    for (int i = 0; i < 5; i++) {
        int y = TOP_BAR + 40 + i * MENU_ROW;
        if (i == selection) ui_fill(canvas, 16, y, W - 32, 44,
                                    UI_ROW_SELECTED);
        ui_text(canvas, 32, y + 14, W - 24, entries[i], 16,
                i == 0 && !can_return ? UI_TEXT_MUTED : UI_TEXT);
    }
    int y = TOP_BAR + 40 + 5 * MENU_ROW + 20;
    ui_text(canvas, 16, y, W - 8, "見ている間の操作", 16, UI_TEXT_MUTED);
    char line[160];
    ui_text(canvas, 16, y + 22, W - 8, "上下（十字・アナログ） 次/前", 16,
            UI_TEXT_MUTED);
    snprintf(line, sizeof line, "%s 停止  " UI_ICON_TRIANGLE " いいね",
             ICON_OK);
    ui_text(canvas, 16, y + 44, W - 8, line, 16, UI_TEXT_MUTED);
    ui_text(canvas, 16, y + 66, W - 8, "左右 5秒戻る/進む", 16,
            UI_TEXT_MUTED);
    ui_text(canvas, 16, y + 88, W - 8, "START 検索  SELECT メニュー", 16,
            UI_TEXT_MUTED);
    draw_toast(canvas);
}

static MenuChoice run_menu(bool can_return)
{
    int selection = can_return ? 0 : 1;
    app.buttons = ~0u;
    for (;;) {
        komi_progress("menu");
        read_input(true);
        if (pressed(KEY_UP) && selection > (can_return ? 0 : 1)) selection--;
        if (pressed(KEY_DOWN) && selection < 4) selection++;
        uint16_t *canvas = begin_frame();
        if (canvas != NULL) {
            draw_menu(canvas, selection, can_return);
            komi_canvas_present();
        }
        if ((pressed(BTN_BACK) || pressed(PSP_CTRL_SELECT)) && can_return)
            return MENU_RETURN;
        if (pressed(PSP_CTRL_START)) return MENU_SEARCH;
        if (pressed(BTN_OK)) return (MenuChoice) selection;
    }
}

/* --- sessions --- */

/* Start a feed of the given kind; false (with a toast) when it is empty,
   and then the feed that was playing stays. */
static bool start_feed(FeedKind kind, const char *query)
{
    static Feed previous;
    previous = feed;
    feed_reset(kind, query);
    if (feed_load_first()) return true;
    feed = previous;
    toast(kind == FEED_MYLIST ? "マイリストは空です（見ている間に△で追加）"
          : kind == FEED_SEARCH ? "見つかりませんでした"
                                : "ショートを読み込めませんでした");
    return false;
}

/* Play the feed until the user asks for the menu. */
static void watch(void)
{
    unsigned failures_in_a_row = 0;
    for (;;) {
        PlayExit exit = play_current_retrying(NULL);
        if (exit == PLAY_FAILED) {
            if (++failures_in_a_row >= MAX_FAILURES_IN_A_ROW) {
                toast("続けて再生できませんでした");
                return;
            }
            exit = PLAY_NEXT;
        } else {
            failures_in_a_row = 0;
        }
        switch (exit) {
        case PLAY_NEXT:
            if (!feed_advance()) {
                toast("これ以上ありません");
                if (feed.kind == FEED_MYLIST) feed.index = 0;
                else return;
            }
            break;
        case PLAY_PREVIOUS:
            if (feed.index > 0) feed.index--;
            break;
        case PLAY_SEARCH: {
            char query[256];
            if (ask_query(query, sizeof query))
                (void) start_feed(FEED_SEARCH, query);
            break;
        }
        case PLAY_MENU:
        default:
            return;
        }
    }
}

/* --- config and autotest --- */

typedef struct {
    bool autotest;
    char query[128];
    unsigned play_seconds;
    /* draw=canvas: the first version's canvas-and-rotate path (for
       comparison); the default draws through the rotation directly. */
    bool draw_canvas;
    /* draw=cpu: rotate the video with the CPU instead of the GE. */
    bool draw_cpu;
    /* feed_ticks=0: feed the codec once per frame only (for comparison). */
    bool no_feed_ticks;
    /* autotest_shots=0: no screenshots during playback (writing one over
       host0: stalls the frame loop ~200 ms, which skews the audio gaps). */
    bool no_shots;
} Config;

static void load_config(Config *config)
{
    memset(config, 0, sizeof *config);
    config->play_seconds = 8;
    char path[256];
    komi_sibling_path(path, sizeof path, "dopagaki.cfg");
    FILE *file = fopen(path, "r");
    if (file == NULL) return;
    char line[256];
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        unsigned value = 0;
        if (sscanf(line, "autotest=%u", &value) == 1)
            config->autotest = value != 0;
        else if (strncmp(line, "autotest_query=", 15) == 0)
            snprintf(config->query, sizeof config->query, "%s", line + 15);
        else if (sscanf(line, "autotest_play_seconds=%u", &value) == 1
                 && value > 0)
            config->play_seconds = value;
        else if (strcmp(line, "draw=canvas") == 0)
            config->draw_canvas = true;
        else if (strcmp(line, "draw=cpu") == 0)
            config->draw_cpu = true;
        else if (strcmp(line, "feed_ticks=0") == 0)
            config->no_feed_ticks = true;
        else if (strcmp(line, "autotest_shots=0") == 0)
            config->no_shots = true;
    }
    fclose(file);
}

static bool check(bool condition, const char *what)
{
    komi_result("check %s %s", condition ? "PASS" : "FAIL", what);
    return condition;
}

static void autotest(const Config *config)
{
    komi_set_watchdog_seconds(120);
    unsigned failures = 0;
    uint16_t *canvas = begin_frame();
    if (canvas != NULL) {
        draw_menu(canvas, 1, false);
        screenshot(canvas, "shot-01-menu.bmp");
        komi_canvas_present();
    }
    uint64_t play_us = (uint64_t) config->play_seconds * 1000000u;

    /* おすすめ: three in a row, moving down like the user would. */
    failures += !check(start_feed(FEED_RECOMMENDED, NULL) && feed.count >= 3,
                       "recommended feed has Shorts");
    static const char *const shots[] = {
        "shot-02-feed1.bmp", "shot-03-feed2.bmp", "shot-04-feed3.bmp"};
    unsigned played = 0;
    for (int i = 0; i < 3 && feed.count > 0; i++) {
        AutoPlay play = {true, play_us, false,
                         config->no_shots ? NULL : shots[i]};
        if (play_current_retrying(&play) != PLAY_FAILED) played++;
        if (!feed_advance()) break;
    }
    failures += !check(played >= 2, "recommended Shorts play");
    failures += !check(feed.count > 3 || feed.token[0] != '\0',
                       "recommended feed continues");

    /* Search, like during playback, then the mylist feed. */
    const char *query = config->query[0] != '\0' ? config->query : "猫";
    failures += !check(start_feed(FEED_SEARCH, query) && feed.count >= 3,
                       "search finds Shorts");
    ShortItem first = feed.items[0];
    size_t before = mylist.count;
    AutoPlay play = {true, play_us, true,
                     config->no_shots ? NULL : "shot-05-search.bmp"};
    failures += !check(play_current_retrying(&play) != PLAY_FAILED,
                       "searched Short plays");
    failures += !check(mylist_find(&mylist, first.id) == 0
                       && mylist.count == before + 1,
                       "liked during playback is first in mylist");
    failures += !check(start_feed(FEED_MYLIST, NULL)
                       && strcmp(feed.items[0].id, first.id) == 0,
                       "mylist feed starts with the like");
    failures += !check(mylist_remove(&mylist, first.id)
                       && mylist.count == before,
                       "remove from mylist");
    komi_result("AUTOTEST %s failures=%u heap-used=%u",
                failures == 0 ? "PASS" : "FAIL", failures, komi_heap_used());
}

/* --- main --- */

static void boot_message(const char *line1, const char *line2)
{
    uint16_t *canvas = begin_frame();
    if (canvas == NULL) return;
    draw_top_bar(canvas, "");
    draw_centered(canvas, H / 2 - 20, line1, UI_TEXT);
    if (line2 != NULL)
        ui_text_wrap(canvas, 16, H / 2 + 8, W - 16, line2, 16, UI_TEXT_MUTED,
                     20, 4);
    komi_canvas_present();
}

int main(int argc, char **argv)
{
    komi_log_open(argc > 0 ? argv[0] : NULL, "dopagaki.txt");
    komi_result("start dopagaki version=1 argv0=%s", komi.argv0);
    Config config;
    load_config(&config);
    komi_platform_init(false);
    komi_set_orientation(config.draw_canvas ? KOMI_PORTRAIT_CANVAS
                                            : KOMI_PORTRAIT_DIRECT);
    long step_x, step_y, origin;
    komi_canvas_steps(&step_x, &step_y, &origin);
    ui_set_surface(W, H, step_x, step_y, origin);
    komi_set_feed_ticks(!config.no_feed_ticks);
    komi_set_portrait_ge(!config.draw_cpu);
    komi_result("draw %s feed-ticks=%d", config.draw_canvas ? "canvas"
                                         : config.draw_cpu ? "cpu" : "ge",
                !config.no_feed_ticks);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    read_button_setting();

    budget_init(&ui_budget, 4u * 1024u * 1024u);
    char path[256];
    komi_sibling_path(path, sizeof path, "fonts/TilefinchSans-Regular.ttf");
    komi_result("font %s %s", path,
                ui_init(&ui_budget, path) ? "loaded" : "missing");
    komi_sibling_path(path, sizeof path, "dopagaki-mylist.txt");
    mylist_load(&mylist, path);
    komi_result("mylist %u videos (%s)", (unsigned) mylist.count, path);

    boot_message("Wi-Fiに接続しています...", NULL);
    if (!komi_services_init()) {
        boot_message("Wi-Fiに接続できませんでした",
                     "Wi-Fiスイッチと、保存したネットワーク設定を確認してください");
        uint64_t until = komi_now_us() + 6000000u;
        while (komi_now_us() < until) {
            komi_progress("offline");
            sceDisplayWaitVblankStart();
        }
        komi_log_close("offline");
        sceKernelExitGame();
        return 0;
    }
    komi_result("ready heap-used=%u", komi_heap_used());

    if (config.autotest) {
        autotest(&config);
        komi_log_close("autotest");
        sceKernelExitGame();
        return 0;
    }

    /* Interactive: straight into おすすめ, like opening the Shorts tab. */
    komi_set_watchdog_seconds(90);
    bool have_feed = start_feed(FEED_RECOMMENDED, NULL);
    if (have_feed) watch();
    for (;;) {
        MenuChoice choice = run_menu(have_feed && feed.count > 0);
        switch (choice) {
        case MENU_RETURN:
            break;
        case MENU_RECOMMENDED:
            have_feed = start_feed(FEED_RECOMMENDED, NULL);
            break;
        case MENU_SEARCH: {
            char query[256];
            if (ask_query(query, sizeof query))
                have_feed = start_feed(FEED_SEARCH, query) || have_feed;
            break;
        }
        case MENU_MYLIST:
            have_feed = start_feed(FEED_MYLIST, NULL) || have_feed;
            break;
        case MENU_QUIT:
            goto quit;
        }
        if (have_feed && feed.count > 0) watch();
    }
quit:
    psp_media_shutdown(&komi.media);
    komi_result("quit");
    komi_log_close("quit");
    sceKernelExitGame();
    return 0;
}
