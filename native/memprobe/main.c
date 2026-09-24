/*
 * komi-tube memory probe (stage 0).
 *
 * Runs once at launch, writes every measurement to memprobe-<variant>.txt
 * next to the EBOOT, and exits. It answers, in one run on the device:
 *   1. how large the user partition is under this EBOOT's MEMSIZE setting,
 *      and where its free ranges lie (below / above 0x0A000000);
 *   2. how much the AV and network modules take once loaded;
 *   3. whether a 6 MiB Media Engine region fits below 0x0A000000 on a
 *      4 MiB boundary, and how large a general arena remains after it;
 *   4. which regions the Media Engine can actually read, by decoding the
 *      same AAC frames through sceAudiocodec with buffers placed in each;
 *   5. whether ten codec open/close cycles leak partition memory.
 * Each line is appended and the file closed at once, so a hang still leaves
 * everything up to the step that hung.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <psputility.h>
#include <psputility_avmodules.h>
#include <psputility_netmodules.h>
#include <pspaudiocodec.h>
#include <pspsuspend.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspnet_apctl.h>
#include <kubridge.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aac_fixture.h"

#ifndef MEMPROBE_VARIANT
#define MEMPROBE_VARIANT "unknown"
#endif
#define MEMPROBE_VERSION 2

PSP_MODULE_INFO("komi_memprobe", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
/* Keep newlib small: the probe allocates from the partition directly, as
   the planned client will, and the map must see the partition, not newlib. */
PSP_HEAP_SIZE_KB(1024);

#define ME_LIMIT UINT32_C(0x0A000000)
#define ME_REGION_BYTES (6u * 1024u * 1024u)
#define ME_REGION_ALIGN (4u * 1024u * 1024u)
/* What the arena leaves behind for thread stacks and late firmware needs. */
#define ARENA_RESERVE_BYTES (1024u * 1024u)
#define MAP_MAX 48
#define PCM_BYTES (1024u * 2u * 2u)
#define PCM_FILL 0xA5u
/* PSP-3000 firmware 6.61 asks for 100744 bytes of AAC work memory, far more
   than tilefinch's comment assumed (v1 capped it at 32 KiB and never ran
   the decode). Leave room for twice that. */
#define EDRAM_MAX (256u * 1024u)
#define CARVE_BYTES (1024u + EDRAM_MAX + 8192u + PCM_BYTES + 64u)
#define CARVE_SLOT (320u * 1024u)

static char log_path[256];

static void out(int on_screen, const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof line - 1, format, args);
    va_end(args);
    if (n < 0) return;
    if (n > (int) sizeof line - 2) n = (int) sizeof line - 2;
    line[n++] = '\n';
    line[n] = '\0';
    SceUID fd = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, line, n);
        sceIoClose(fd);
    }
    if (on_screen) pspDebugScreenPrintf("%s", line);
}

#define LOG(...) out(0, __VA_ARGS__)
#define SHOW(...) out(1, __VA_ARGS__)

static void snapshot(const char *label)
{
    LOG("mem label=%s total_free=%u max_free=%u", label,
        (unsigned) sceKernelTotalFreeMemSize(),
        (unsigned) sceKernelMaxFreeMemSize());
}

static const char *side(uint32_t start, uint32_t size)
{
    uint32_t end = start + size;
    if (end <= ME_LIMIT) return "below";
    if (start >= ME_LIMIT) return "above";
    return "straddles";
}

/* Allocate a partition-2 block, retrying slightly smaller sizes because
   MaxFreeMemSize does not account for block-header rounding everywhere. */
static SceUID alloc_block(const char *name, int type, uint32_t *size, void *addr)
{
    for (int attempt = 0; attempt < 8 && *size >= 4096u; attempt++) {
        SceUID uid = sceKernelAllocPartitionMemory(2, name, type, *size, addr);
        if (uid >= 0) return uid;
        *size -= 256u;
    }
    return -1;
}

typedef struct { uint32_t addr, size; } Range;

/* Take the largest free block repeatedly, record each, then give all back.
   The result is the free map of the user partition at this moment. */
static void map_partition(const char *label)
{
    SceUID uids[MAP_MAX];
    Range ranges[MAP_MAX];
    int count = 0;
    while (count < MAP_MAX) {
        uint32_t size = (uint32_t) sceKernelMaxFreeMemSize();
        if (size < 16u * 1024u) break;
        SceUID uid = alloc_block("probe-map", PSP_SMEM_Low, &size, NULL);
        if (uid < 0) break;
        uids[count] = uid;
        ranges[count].addr = (uint32_t) (uintptr_t) sceKernelGetBlockHeadAddr(uid);
        ranges[count].size = size;
        count++;
    }
    for (int i = 0; i < count; i++) sceKernelFreePartitionMemory(uids[i]);
    /* Sort by address for a readable map. */
    for (int i = 1; i < count; i++) {
        Range r = ranges[i];
        int j = i - 1;
        while (j >= 0 && ranges[j].addr > r.addr) { ranges[j + 1] = ranges[j]; j--; }
        ranges[j + 1] = r;
    }
    uint32_t below = 0, above = 0;
    for (int i = 0; i < count; i++) {
        uint32_t a = ranges[i].addr, s = ranges[i].size, e = a + s;
        if (e <= ME_LIMIT) below += s;
        else if (a >= ME_LIMIT) above += s;
        else { below += ME_LIMIT - a; above += e - ME_LIMIT; }
        LOG("map label=%s start=0x%08X end=0x%08X size=%u side=%s",
            label, (unsigned) a, (unsigned) e, (unsigned) s, side(a, s));
    }
    LOG("map-total label=%s ranges=%d free_below=%u free_above=%u",
        label, count, (unsigned) below, (unsigned) above);
}

/* ---- sceAudiocodec placement test ---- */

typedef struct {
    unsigned long *ctrl;    /* 65 words, 64-byte aligned */
    unsigned char *edram;   /* size from CheckNeedMem */
    unsigned char *input;   /* one access unit */
    unsigned char *pcm;     /* one decoded frame */
} CodecPlacement;

typedef struct {
    int ok;
    int first_error_frame;
    int first_error;
    uint32_t pcm_changed;
    uint32_t checksum;
    unsigned edram_need;
} CodecResult;

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

/* Carve ctrl/edram/input/pcm out of [base, base + CARVE_BYTES). */
static void carve(CodecPlacement *p, uint32_t base)
{
    base = align_up(base, 64u);
    p->ctrl = (unsigned long *) (uintptr_t) base;
    p->edram = (unsigned char *) (uintptr_t) (base + 1024u);
    p->input = (unsigned char *) (uintptr_t) (base + 1024u + EDRAM_MAX);
    p->pcm = (unsigned char *) (uintptr_t) (base + 1024u + EDRAM_MAX + 8192u);
}

static CodecResult run_codec(const char *label, const CodecPlacement *p)
{
    CodecResult r = { 0, -1, 0, 0, 2166136261u, 0 };
    LOG("codec-begin label=%s ctrl=0x%08X edram=0x%08X input=0x%08X pcm=0x%08X",
        label, (unsigned) (uintptr_t) p->ctrl, (unsigned) (uintptr_t) p->edram,
        (unsigned) (uintptr_t) p->input, (unsigned) (uintptr_t) p->pcm);
    unsigned long *ctrl = p->ctrl;
    memset(ctrl, 0, 65 * sizeof(unsigned long));
    sceKernelDcacheWritebackInvalidateRange(ctrl, 320);
    int status = sceAudiocodecCheckNeedMem(ctrl, PSP_CODEC_AAC);
    sceKernelDcacheWritebackInvalidateRange(ctrl, 320);
    r.edram_need = (unsigned) ctrl[4];
    if (status >= 0 && r.edram_need == 0) {
        /* PPSSPP's HLE leaves word 4 alone; firmware always fills it. Go on
           with a generous size so the decode path still runs in emulation. */
        LOG("codec-note label=%s need=0 assumed=16384", label);
    }
    if (status < 0 || ctrl[4] > EDRAM_MAX) {
        LOG("codec-fail label=%s stage=CheckNeedMem status=0x%08X need=%u",
            label, (unsigned) status, r.edram_need);
        r.first_error = status;
        return r;
    }
    memset(p->edram, 0, EDRAM_MAX);
    sceKernelDcacheWritebackInvalidateRange(p->edram, EDRAM_MAX);
    ctrl[3] = (unsigned long) (uintptr_t) p->edram;
    ctrl[10] = MEMPROBE_AAC_RATE;
    sceKernelDcacheWritebackInvalidateRange(ctrl, 320);
    status = sceAudiocodecInit(ctrl, PSP_CODEC_AAC);
    sceKernelDcacheWritebackInvalidateRange(ctrl, 320);
    if (status < 0) {
        LOG("codec-fail label=%s stage=Init status=0x%08X need=%u",
            label, (unsigned) status, r.edram_need);
        r.first_error = status;
        return r;
    }
    const unsigned char *frame = memprobe_aac_data;
    for (unsigned i = 0; i < MEMPROBE_AAC_FRAMES; i++) {
        unsigned size = memprobe_aac_size[i];
        memcpy(p->input, frame, size);
        frame += size;
        memset(p->pcm, PCM_FILL, PCM_BYTES);
        ctrl[6] = (unsigned long) (uintptr_t) p->input;
        ctrl[7] = size;
        ctrl[8] = (unsigned long) (uintptr_t) p->pcm;
        ctrl[9] = PCM_BYTES;
        sceKernelDcacheWritebackRange(p->input, align_up(size, 64u));
        sceKernelDcacheWritebackRange(ctrl, 320);
        sceKernelDcacheWritebackInvalidateRange(p->pcm, PCM_BYTES);
        status = sceAudiocodecDecode(ctrl, PSP_CODEC_AAC);
        sceKernelDcacheWritebackInvalidateRange(ctrl, 320);
        sceKernelDcacheInvalidateRange(p->pcm, PCM_BYTES);
        if (status < 0) {
            if (r.first_error_frame < 0) {
                r.first_error_frame = (int) i;
                r.first_error = status;
            }
            continue;
        }
        r.ok++;
        for (unsigned b = 0; b < PCM_BYTES; b++) {
            if (p->pcm[b] != PCM_FILL) r.pcm_changed++;
            r.checksum = (r.checksum ^ p->pcm[b]) * 16777619u;
        }
    }
    LOG("codec-result label=%s ok=%d/%u first_error_frame=%d first_error=0x%08X "
        "pcm_changed=%u checksum=0x%08X need=%u",
        label, r.ok, MEMPROBE_AAC_FRAMES, r.first_error_frame,
        (unsigned) r.first_error, (unsigned) r.pcm_changed,
        (unsigned) r.checksum, r.edram_need);
    return r;
}

static int codec_verdict(const CodecResult *r, const CodecResult *reference)
{
    /* The Media Engine read and wrote this region only if every frame
       decoded and produced the same samples as the reference placement. */
    return r->ok == (int) MEMPROBE_AAC_FRAMES && r->pcm_changed > 0
        && (reference == NULL || r->checksum == reference->checksum);
}

/* Log the MEMSIZE the installed komi-tube EBOOT asks for, so the probe
   result can be compared with what the app actually gets. */
static void log_sibling_memsize(void)
{
    char path[256];
    snprintf(path, sizeof path, "%s", log_path);
    char *slash = strrchr(path, '/');
    if (slash != NULL) *slash = '\0';
    slash = strrchr(path, '/');
    if (slash == NULL) return;
    snprintf(slash + 1, sizeof path - (size_t) (slash + 1 - path), "KOMI_TUBE/EBOOT.PBP");
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        LOG("komi-tube-eboot path=%s status=0x%08X", path, (unsigned) fd);
        return;
    }
    static unsigned char buf[4096];
    int n = sceIoRead(fd, buf, sizeof buf);
    sceIoClose(fd);
    uint32_t *head = (uint32_t *) buf;
    int memsize = -1;
    /* PBP: magic, version, then eight section offsets; PARAM.SFO is first. */
    if (n >= 40 && head[0] == 0x50425000u && head[2] < head[3] && head[3] <= (uint32_t) n) {
        const unsigned char *sfo = buf + head[2];
        uint32_t sfo_len = head[3] - head[2];
        const uint32_t *h = (const uint32_t *) sfo;
        if (sfo_len >= 20 && h[0] == 0x46535000u) {
            uint32_t keys = h[2], values = h[3], count = h[4];
            for (uint32_t i = 0; i < count && 20u + 16u * (i + 1) <= sfo_len; i++) {
                const unsigned char *e = sfo + 20 + 16 * i;
                uint16_t key_off = (uint16_t) (e[0] | e[1] << 8);
                uint32_t value_off = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t) e[15] << 24;
                if (keys + key_off + 8 <= sfo_len
                    && memcmp(sfo + keys + key_off, "MEMSIZE", 8) == 0
                    && values + value_off + 4 <= sfo_len) {
                    memcpy(&memsize, sfo + values + value_off, 4);
                }
            }
        }
    }
    LOG("komi-tube-eboot path=%s memsize=%d", path, memsize);
}

/* ---- boot plumbing ---- */

static int exit_callback(int a, int b, void *c)
{
    (void) a; (void) b; (void) c;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void) args; (void) argp;
    int cb = sceKernelCreateCallback("exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

static void set_log_path(int argc, char **argv)
{
    const char *fallback = "ms0:/PSP/GAME/KOMI_MEMPROBE/EBOOT.PBP";
    const char *self = (argc > 0 && argv[0] != NULL) ? argv[0] : fallback;
    snprintf(log_path, sizeof log_path, "%s", self);
    char *slash = strrchr(log_path, '/');
    if (slash == NULL) snprintf(log_path, sizeof log_path, "%s", fallback), slash = strrchr(log_path, '/');
    snprintf(slash + 1, sizeof log_path - (size_t) (slash + 1 - log_path),
             "memprobe-%s.txt", MEMPROBE_VARIANT);
}

static int wait_for_button(int seconds)
{
    SceCtrlData pad;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    for (int frame = 0; frame < seconds * 60; frame++) {
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE | PSP_CTRL_START)) return 1;
        sceDisplayWaitVblankStart();
    }
    return 0;
}

static void load_modules(void)
{
    int status = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
    LOG("module name=avcodec status=0x%08X", (unsigned) status);
    snapshot("after-avcodec");

    /* The decoder the planned player uses; tilefinch loads it the same way. */
    SceUID mpeg = kuKernelLoadModule("flash0:/kd/mpeg_vsh.prx", 0, NULL);
    int start = -1, module_status = 0;
    if (mpeg >= 0) start = sceKernelStartModule(mpeg, 0, NULL, &module_status, NULL);
    LOG("module name=mpeg_vsh load=0x%08X start=0x%08X module_status=0x%08X",
        (unsigned) mpeg, (unsigned) start, (unsigned) module_status);
    snapshot("after-mpeg-vsh");

    status = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    LOG("module name=net-common status=0x%08X", (unsigned) status);
    status = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    LOG("module name=net-inet status=0x%08X", (unsigned) status);
    snapshot("after-net-modules");

    /* Same pool sizes as tilefinch's psp_network.c; no connection is made. */
    status = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    LOG("net stage=sceNetInit status=0x%08X", (unsigned) status);
    status = sceNetInetInit();
    LOG("net stage=sceNetInetInit status=0x%08X", (unsigned) status);
    status = sceNetResolverInit();
    LOG("net stage=sceNetResolverInit status=0x%08X", (unsigned) status);
    status = sceNetApctlInit(0x8000, 0x30);
    LOG("net stage=sceNetApctlInit status=0x%08X", (unsigned) status);
    snapshot("after-net-init");
}

int main(int argc, char **argv)
{
    SceUID thread = sceKernelCreateThread("exit-cb", callback_thread, 0x11, 0x1000, 0, NULL);
    if (thread >= 0) sceKernelStartThread(thread, 0, NULL);
    pspDebugScreenInit();
    set_log_path(argc, argv);

    SHOW("komi-tube memprobe v%d variant=%s", MEMPROBE_VERSION, MEMPROBE_VARIANT);
    SHOW("log: %s", log_path);
    LOG("==== run begin version=%d variant=%s", MEMPROBE_VERSION, MEMPROBE_VARIANT);
    int model = kuKernelGetModel();
    LOG("device devkit=0x%08X model=%d", (unsigned) sceKernelDevkitVersion(), model);
    int stack_marker = 0;
    void *heap_marker = malloc(16);
    LOG("image main=0x%08X stack=0x%08X newlib_block=0x%08X",
        (unsigned) (uintptr_t) &main, (unsigned) (uintptr_t) &stack_marker,
        (unsigned) (uintptr_t) heap_marker);
    free(heap_marker);

    log_sibling_memsize();
    snapshot("boot");
    map_partition("boot");

    SHOW("loading AV / net modules...");
    load_modules();
    map_partition("after-modules");

    /* ---- the planned layout ---- */
    SHOW("reserving ME region + arena...");
    SceUID me_uid = -1;
    uint32_t me_addr = 0, me_size = ME_REGION_BYTES;
    for (uint32_t candidate = 0x08800000u; candidate + ME_REGION_BYTES <= ME_LIMIT;
         candidate += ME_REGION_ALIGN) {
        uint32_t size = ME_REGION_BYTES;
        SceUID uid = sceKernelAllocPartitionMemory(2, "me-region", PSP_SMEM_Addr, size,
                                                   (void *) (uintptr_t) candidate);
        uint32_t head = uid >= 0 ? (uint32_t) (uintptr_t) sceKernelGetBlockHeadAddr(uid) : 0;
        LOG("me-candidate addr=0x%08X uid=0x%08X head=0x%08X", (unsigned) candidate,
            (unsigned) uid, (unsigned) head);
        if (uid >= 0 && head == candidate) { me_uid = uid; me_addr = head; break; }
        if (uid >= 0) sceKernelFreePartitionMemory(uid);
    }
    const char *me_mode = "aligned";
    if (me_uid < 0) {
        /* No 4 MiB boundary free: fall back to the lowest fit and report it. */
        me_mode = "low-fallback";
        me_size = ME_REGION_BYTES;
        me_uid = alloc_block("me-region", PSP_SMEM_Low, &me_size, NULL);
        if (me_uid >= 0) me_addr = (uint32_t) (uintptr_t) sceKernelGetBlockHeadAddr(me_uid);
    }
    LOG("me-region mode=%s uid=0x%08X start=0x%08X end=0x%08X size=%u side=%s",
        me_mode, (unsigned) me_uid, (unsigned) me_addr, (unsigned) (me_addr + me_size),
        (unsigned) me_size, me_uid >= 0 ? side(me_addr, me_size) : "none");

    uint32_t max_free = (uint32_t) sceKernelMaxFreeMemSize();
    uint32_t arena_size = max_free > ARENA_RESERVE_BYTES ? max_free - ARENA_RESERVE_BYTES : 0;
    SceUID arena_uid = arena_size ? alloc_block("arena", PSP_SMEM_High, &arena_size, NULL) : -1;
    uint32_t arena_addr = arena_uid >= 0
        ? (uint32_t) (uintptr_t) sceKernelGetBlockHeadAddr(arena_uid) : 0;
    uint32_t arena_end = arena_addr + arena_size;
    uint32_t arena_above = arena_uid < 0 ? 0
        : arena_addr >= ME_LIMIT ? arena_size
        : arena_end > ME_LIMIT ? arena_end - ME_LIMIT : 0;
    LOG("arena uid=0x%08X start=0x%08X end=0x%08X size=%u above_limit=%u side=%s",
        (unsigned) arena_uid, (unsigned) arena_addr, (unsigned) arena_end,
        (unsigned) arena_size, (unsigned) arena_above,
        arena_uid >= 0 ? side(arena_addr, arena_size) : "none");
    snapshot("after-layout");
    map_partition("after-layout");

    /* ---- Media Engine visibility ---- */
    SHOW("Media Engine visibility test (AAC)...");
    CodecPlacement low, high, mixed, vol;
    CodecResult r_low = { 0 }, r_vol = { 0 }, r_mixed = { 0 }, r_high = { 0 };
    int have_low = me_uid >= 0 && me_addr + me_size <= ME_LIMIT;
    int have_high = arena_uid >= 0 && arena_end >= ME_LIMIT + 2u * CARVE_SLOT;
    int low_ok = 0, vol_ok = -1, mixed_ok = -1, high_ok = -1;
    if (have_low) {
        SHOW("ME test: low memory...");
        /* The tail of the ME region, leaving its 4 MiB-aligned head for DDR. */
        carve(&low, me_addr + me_size - CARVE_SLOT);
        r_low = run_codec("me-region", &low);
        low_ok = codec_verdict(&r_low, NULL);
    } else {
        LOG("codec-skip label=me-region reason=no-low-region");
    }

    /* ---- ten open/close cycles in the ME region ---- */
    int cycles_ok = 0;
    uint32_t free_before = (uint32_t) sceKernelTotalFreeMemSize();
    if (have_low) {
        SHOW("10 codec cycles...");
        for (int cycle = 0; cycle < 10; cycle++) {
            char label[24];
            snprintf(label, sizeof label, "cycle-%d", cycle + 1);
            CodecResult r = run_codec(label, &low);
            if (codec_verdict(&r, &r_low)) cycles_ok++;
        }
    }
    uint32_t free_after = (uint32_t) sceKernelTotalFreeMemSize();
    LOG("cycles ok=%d/10 free_before=%u free_after=%u", cycles_ok,
        (unsigned) free_before, (unsigned) free_after);

    /* Volatile memory (0x08400000, 4 MiB on 2000/3000) is another candidate
       for ME buffers outside the user partition. Lock, test, unlock. */
    void *vol_ptr = NULL;
    int vol_size = 0;
    int vol_status = sceKernelVolatileMemTryLock(0, &vol_ptr, &vol_size);
    LOG("volatile lock=0x%08X ptr=0x%08X size=%d", (unsigned) vol_status,
        (unsigned) (uintptr_t) vol_ptr, vol_size);
    if (vol_status >= 0 && vol_ptr != NULL && vol_size >= 65536 && have_low) {
        SHOW("ME test: volatile memory...");
        carve(&vol, (uint32_t) (uintptr_t) vol_ptr);
        r_vol = run_codec("volatile", &vol);
        vol_ok = codec_verdict(&r_vol, &r_low);
    }
    if (vol_status >= 0) sceKernelVolatileMemUnlock(0);

    if (have_high && have_low) {
        /* High memory is tested last: if the Media Engine hangs on it,
           everything else is already on the card. */
        SHOW("ME test: high memory. If the screen stops here for 30 s,");
        SHOW("hold POWER to switch off; the log is already saved.");
        carve(&high, arena_end - CARVE_SLOT);
        mixed = low;
        mixed.input = high.input;
        mixed.pcm = high.pcm;
        r_mixed = run_codec("mixed-io-high", &mixed);
        mixed_ok = codec_verdict(&r_mixed, &r_low);
        r_high = run_codec("arena-high", &high);
        high_ok = codec_verdict(&r_high, &r_low);
    } else {
        LOG("codec-skip label=arena-high reason=%s", have_low ? "no-high-memory" : "no-low-region");
    }

    /* ---- summary: the lines scripts/memprobe_report.py reads ---- */
    SHOW("");
    SHOW("SUMMARY variant=%s model=%d", MEMPROBE_VARIANT, model);
    SHOW("SUMMARY me_region=0x%08X-0x%08X mode=%s below_limit=%s", (unsigned) me_addr,
         (unsigned) (me_addr + me_size), me_mode, have_low ? "yes" : "NO");
    SHOW("SUMMARY arena=0x%08X-0x%08X size=%uKB above_limit=%uKB", (unsigned) arena_addr,
         (unsigned) arena_end, (unsigned) (arena_size / 1024u),
         (unsigned) (arena_above / 1024u));
    SHOW("SUMMARY me_read low=%s volatile=%s mixed_io_high=%s high=%s",
         low_ok ? "ok" : "FAIL",
         vol_ok < 0 ? "n/a" : vol_ok ? "ok" : "FAIL",
         mixed_ok < 0 ? "n/a" : mixed_ok ? "ok" : "FAIL",
         high_ok < 0 ? "n/a" : high_ok ? "ok" : "FAIL");
    SHOW("SUMMARY cycles=%d/10 leak=%d", cycles_ok, (int) free_before - (int) free_after);
    LOG("==== run end");

    if (arena_uid >= 0) sceKernelFreePartitionMemory(arena_uid);
    if (me_uid >= 0) sceKernelFreePartitionMemory(me_uid);

    SHOW("");
    SHOW("Done. Saved to memory stick. Press X / O / START (auto exit 30 s).");
    wait_for_button(30);
    sceKernelExitGame();
    return 0;
}
