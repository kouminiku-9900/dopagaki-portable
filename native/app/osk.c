/* The firmware on-screen keyboard, returning UTF-8.
 *
 * Follows tilefinch's open_keyboard (src/psp_text_input.c): with the system
 * language set to Japanese, every Japanese input mode is named explicitly,
 * because INPUTTYPE_ALL does not enable the firmware's kana-to-kanji
 * converter. SELECT cycles the modes on the keyboard. */
#include "osk.h"

#include <pspdisplay.h>
#include <pspgu.h>
#include <psputility.h>

#include <stdio.h>
#include <string.h>

#include "komi_runtime.h"

#define OSK_CAPACITY 128

static unsigned int __attribute__((aligned(64))) gu_list[4096];

static size_t utf8_to_ucs2(const char *input, unsigned short *output,
                           size_t capacity)
{
    size_t in = 0;
    size_t out = 0;
    while (input != NULL && input[in] != '\0' && out + 1 < capacity) {
        unsigned char first = (unsigned char) input[in++];
        unsigned codepoint = first;
        if ((first & 0xe0u) == 0xc0u && input[in] != '\0') {
            codepoint = (first & 0x1fu) << 6;
            codepoint |= (unsigned char) input[in++] & 0x3fu;
        } else if ((first & 0xf0u) == 0xe0u && input[in] != '\0'
                   && input[in + 1] != '\0') {
            codepoint = (first & 0x0fu) << 12;
            codepoint |= ((unsigned char) input[in++] & 0x3fu) << 6;
            codepoint |= (unsigned char) input[in++] & 0x3fu;
        } else if (first >= 0x80u) {
            codepoint = '?';
            while (((unsigned char) input[in] & 0xc0u) == 0x80u) in++;
        }
        output[out++] = (unsigned short) codepoint;
    }
    output[out] = 0;
    return out;
}

static size_t ucs2_to_utf8(const unsigned short *input, char *output,
                           size_t capacity)
{
    size_t in = 0;
    size_t out = 0;
    while (input[in] != 0) {
        unsigned value = input[in++];
        if (value < 0x80u) {
            if (out + 1 >= capacity) break;
            output[out++] = (char) value;
        } else if (value < 0x800u) {
            if (out + 2 >= capacity) break;
            output[out++] = (char) (0xc0u | (value >> 6));
            output[out++] = (char) (0x80u | (value & 0x3fu));
        } else {
            if (out + 3 >= capacity) break;
            output[out++] = (char) (0xe0u | (value >> 12));
            output[out++] = (char) (0x80u | ((value >> 6) & 0x3fu));
            output[out++] = (char) (0x80u | (value & 0x3fu));
        }
    }
    output[out] = '\0';
    return out;
}

bool osk_input(const char *description, const char *initial, char *output,
               size_t capacity)
{
    unsigned short description_ucs2[48] = {0};
    unsigned short input_ucs2[OSK_CAPACITY + 1] = {0};
    unsigned short output_ucs2[OSK_CAPACITY + 1] = {0};
    utf8_to_ucs2(description, description_ucs2, 48);
    utf8_to_ucs2(initial, input_ucs2, OSK_CAPACITY + 1);

    int language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &language);
    bool japanese = language == PSP_SYSTEMPARAM_LANGUAGE_JAPANESE;

    SceUtilityOskData data;
    memset(&data, 0, sizeof data);
    data.language = japanese ? PSP_UTILITY_OSK_LANGUAGE_JAPANESE
                             : PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    data.lines = 1;
    data.unk_24 = 1;
    data.inputtype = japanese
        ? (PSP_UTILITY_OSK_INPUTTYPE_JAPANESE_KANJI
           | PSP_UTILITY_OSK_INPUTTYPE_JAPANESE_HIRAGANA
           | PSP_UTILITY_OSK_INPUTTYPE_JAPANESE_KATAKANA
           | PSP_UTILITY_OSK_INPUTTYPE_JAPANESE_HALF_KATAKANA
           | PSP_UTILITY_OSK_INPUTTYPE_JAPANESE_LOWERCASE
           | PSP_UTILITY_OSK_INPUTTYPE_JAPANESE_UPPERCASE
           | PSP_UTILITY_OSK_INPUTTYPE_JAPANESE_DIGIT
           | PSP_UTILITY_OSK_INPUTTYPE_JAPANESE_SYMBOL
           | PSP_UTILITY_OSK_INPUTTYPE_LATIN_LOWERCASE
           | PSP_UTILITY_OSK_INPUTTYPE_LATIN_UPPERCASE
           | PSP_UTILITY_OSK_INPUTTYPE_LATIN_DIGIT
           | PSP_UTILITY_OSK_INPUTTYPE_LATIN_SYMBOL)
        : PSP_UTILITY_OSK_INPUTTYPE_ALL;
    data.desc = description_ucs2;
    data.intext = input_ucs2;
    data.outtextlength = OSK_CAPACITY + 1;
    data.outtextlimit = OSK_CAPACITY;
    data.outtext = output_ucs2;

    SceUtilityOskParams params;
    memset(&params, 0, sizeof params);
    params.base.size = sizeof params;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE,
                                &params.base.language);
    /* Setting 9 is the X/O accept-button choice (PSPSDK calls it UNKNOWN). */
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN,
                                &params.base.buttonSwap);
    params.base.graphicsThread = 17;
    params.base.accessThread = 19;
    params.base.fontThread = 18;
    params.base.soundThread = 16;
    params.datacount = 1;
    params.data = &data;

    const int buffer_bytes =
        KOMI_VRAM_STRIDE * KOMI_SCREEN_HEIGHT * (int) sizeof(uint16_t);
    sceGuInit();
    sceGuStart(GU_DIRECT, gu_list);
    sceGuDrawBuffer(GU_PSM_5650, (void *) (uintptr_t) buffer_bytes,
                    KOMI_VRAM_STRIDE);
    sceGuDispBuffer(KOMI_SCREEN_WIDTH, KOMI_SCREEN_HEIGHT, (void *) 0,
                    KOMI_VRAM_STRIDE);
    sceGuOffset(2048 - KOMI_SCREEN_WIDTH / 2, 2048 - KOMI_SCREEN_HEIGHT / 2);
    sceGuViewport(2048, 2048, KOMI_SCREEN_WIDTH, KOMI_SCREEN_HEIGHT);
    sceGuScissor(0, 0, KOMI_SCREEN_WIDTH, KOMI_SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    bool initialized = sceUtilityOskInitStart(&params) >= 0;
    bool done = !initialized;
    bool shutdown_requested = false;
    while (!done) {
        komi_progress("osk");
        sceGuStart(GU_DIRECT, gu_list);
        sceGuClearColor(0x00161212u);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        switch (sceUtilityOskGetStatus()) {
        case PSP_UTILITY_DIALOG_VISIBLE:
            if (!shutdown_requested) sceUtilityOskUpdate(1);
            break;
        case PSP_UTILITY_DIALOG_QUIT:
            if (!shutdown_requested) {
                sceUtilityOskShutdownStart();
                shutdown_requested = true;
            }
            break;
        case PSP_UTILITY_DIALOG_NONE:
            done = true;
            break;
        default:
            break;
        }
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }
    sceGuDisplay(GU_FALSE);
    sceGuTerm();
    /* The keyboard owned the display; hand it back to the page surface. */
    (void) psp_display_rearm(&komi.display);

    if (!initialized || data.result == PSP_UTILITY_OSK_RESULT_CANCELLED)
        return false;
    if (data.result == PSP_UTILITY_OSK_RESULT_UNCHANGED)
        snprintf(output, capacity, "%s", initial == NULL ? "" : initial);
    else
        ucs2_to_utf8(output_ucs2, output, capacity);
    return true;
}
