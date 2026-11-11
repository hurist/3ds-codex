#include "voice.h"
#include "ui.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIC_BUFFER_SIZE 0x100000
#define MAX_AUDIO_BYTES (16364 * 2 * 30)

static void fail(Voice *voice, const char *error)
{
    snprintf(voice->error, sizeof(voice->error), "%s", error);
    voice->phase = VOICE_ERROR;
}

void voice_start(Voice *voice)
{
    voice_close(voice);
    voice->samples = memalign(0x1000, MIC_BUFFER_SIZE);
    if (!voice->samples) {
        fail(voice, "无法分配录音空间。");
        return;
    }
    memset(voice->samples, 0, MIC_BUFFER_SIZE);
    GSPGPU_FlushDataCache(voice->samples, MIC_BUFFER_SIZE);
    if (R_FAILED(micInit(voice->samples, MIC_BUFFER_SIZE))) {
        voice_close(voice);
        fail(voice, "无法访问麦克风。");
        return;
    }
    MICU_SetAllowShellClosed(false);
    if (R_FAILED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360,
                                   0, MAX_AUDIO_BYTES, false))) {
        micExit();
        voice_close(voice);
        fail(voice, "无法开始录音，请检查麦克风。");
        return;
    }
    voice->started_at = osGetTime();
    voice->phase = VOICE_RECORDING;
}

bool voice_recording_finished(const Voice *voice)
{
    return voice->phase == VOICE_RECORDING &&
           (osGetTime() - voice->started_at >= 30000 || voice->captured >= MAX_AUDIO_BYTES);
}

static unsigned captured_bytes(const Voice *voice)
{
    GSPGPU_InvalidateDataCache(voice->samples + MIC_BUFFER_SIZE - 32, 32);
    unsigned offset = micGetLastSampleOffset();
    return (offset < MAX_AUDIO_BYTES ? offset : MAX_AUDIO_BYTES) & ~1u;
}

void voice_update_level(Voice *voice)
{
    voice->captured = captured_bytes(voice);
    unsigned length = voice->captured < 2048 ? voice->captured : 2048;
    if (!length) {
        voice->level = 0;
        return;
    }
    unsigned start = voice->captured - length;
    GSPGPU_InvalidateDataCache(voice->samples + start, length);
    const s16 *samples = (const s16 *)(voice->samples + start);
    int low = samples[0], high = samples[0];
    for (unsigned i = 1; i < length / 2; i++) {
        if (samples[i] < low) low = samples[i];
        if (samples[i] > high) high = samples[i];
    }
    unsigned span = high - low;
    voice->level = 0;
    for (unsigned threshold = 96; threshold <= span && voice->level < 8; threshold *= 2) {
        voice->level++;
    }
}

bool voice_stop(Voice *voice)
{
    MICU_StopSampling();
    voice->length = captured_bytes(voice);
    micExit();
    if (voice->length) GSPGPU_InvalidateDataCache(voice->samples, voice->length);
    voice->phase = VOICE_TRANSCRIBING;
    if (voice->length < 16364 * 2 / 3) {
        voice_close(voice);
        fail(voice, "录音太短或未收到麦克风数据。");
        return false;
    }
    return true;
}

void voice_receive(Voice *voice, const char *response, const char *error)
{
    bool cancelled = voice->phase == VOICE_CANCELLING;
    voice_close(voice);
    if (cancelled) {
        return;
    }
    if (*error) {
        fail(voice, error);
        return;
    }
    const char marker[] = "3ds-codex/voice/1\n";
    if (strncmp(response, marker, sizeof(marker) - 1) != 0) {
        fail(voice, "电脑返回了无法识别的转写。");
        return;
    }
    const char *text = response + sizeof(marker) - 1;
    size_t size = strlen(text);
    if (!size || size >= sizeof(voice->transcript)) {
        fail(voice, "转写为空或超出草稿容量。");
        return;
    }
    memcpy(voice->transcript, text, size + 1);
    voice->phase = VOICE_RESULT;
}

void voice_close(Voice *voice)
{
    if (voice->phase == VOICE_RECORDING) {
        MICU_StopSampling();
        micExit();
    }
    if (voice->samples) {
        memset(voice->samples, 0, MIC_BUFFER_SIZE);
        free(voice->samples);
    }
    voice->samples = NULL;
    voice->length = 0;
    voice->captured = 0;
    voice->level = 0;
    voice->scroll = 0;
    voice->phase = VOICE_CLOSED;
    voice->error[0] = '\0';
    voice->transcript[0] = '\0';
}

static void show(Screen screen, int x, int y, const char *value, u32 color)
{
    text(screen, x, y, value, line_end(value, screen.width - x - 8), color);
}

static void paragraphs(Screen screen, int y, const char *value, unsigned rows, u32 color)
{
    while (*value && rows--) {
        const char *end = line_end(value, 344);
        text(screen, 26, y, value, end, color);
        value = *end == '\n' ? end + 1 : end;
        y += 20;
    }
}

static const char *next_line(const char *value)
{
    const char *end = line_end(value, 344);
    return *end == '\n' ? end + 1 : end;
}

void voice_scroll(Voice *voice, u32 repeat)
{
    if (repeat & KEY_UP) {
        if (voice->scroll) {
            voice->scroll--;
        }
    } else if (repeat & KEY_DOWN) {
        const char *cursor = voice->transcript;
        for (unsigned i = 0; *cursor && i < voice->scroll + 6; i++) {
            cursor = next_line(cursor);
        }
        if (*cursor) {
            voice->scroll++;
        }
    }
}

void voice_draw(const Voice *voice, Screen top, Screen bottom, const char *title)
{
    ui_clear(top);
    ui_clear(bottom);
    ui_title(top, title, NULL, UI_MUTED);
    ui_card(top, (Rect){16, 42, 368, 158}, UI_PANEL, 0);
    show(bottom, 16, 10, "语音听写", UI_ACCENT);
    const char *primary = NULL;
    const char *back = "B 返回草稿";
    switch (voice->phase) {
        case VOICE_READY:
            show(top, 26, 55, "准备录音", UI_TEXT);
            show(top, 26, 88, "电脑将在本地把语音转成文字。", UI_ACCENT);
            primary = "A 开始录音";
            break;
        case VOICE_RECORDING: {
            char elapsed[64];
            snprintf(elapsed, sizeof(elapsed), "正在录音  %llu / 30 秒",
                     (unsigned long long)((osGetTime() - voice->started_at) / 1000));
            show(top, 26, 55, elapsed, UI_WARNING);
            show(top, 26, 87, "麦克风音量", UI_MUTED);
            for (unsigned i = 0; i < 8; i++) {
                fill(top, 26 + 23 * i, 111, 19, 9, i < voice->level ? UI_ACCENT : UI_FOCUS);
            }
            show(top, 26, 141, voice->level ? "结束录音后可预览转写。" : "请靠近麦克风说一句完整的话。", UI_TEXT);
            primary = "A 结束并转写";
            back = "B 取消录音";
            break;
        }
        case VOICE_TRANSCRIBING:
            show(top, 26, 55, "正在上传录音并转写…", UI_TEXT);
            char duration[64];
            snprintf(duration, sizeof(duration), "已录制 %.1f 秒", voice->length / 32728.0);
            show(top, 26, 88, duration, UI_ACCENT);
            back = "B 取消转写";
            break;
        case VOICE_CANCELLING:
            show(top, 26, 55, "已取消，正在结束本地转写…", UI_TEXT);
            back = NULL;
            break;
        case VOICE_RESULT: {
            const char *cursor = voice->transcript;
            for (unsigned i = 0; *cursor && i < voice->scroll; i++) {
                cursor = next_line(cursor);
            }
            show(top, 26, 51, "转写预览", UI_ACCENT);
            paragraphs(top, 78, cursor, 6, UI_TEXT);
            if (*voice->error) {
                show(top, 26, 208, voice->error, UI_WARNING);
            }
            primary = "A 插入草稿";
            show(bottom, 12, 170, "上下滚动文字  X 重新录音", UI_TEXT);
            break;
        }
        case VOICE_ERROR:
            paragraphs(top, 55, voice->error, 3, UI_WARNING);
            primary = "A 重新录音";
            break;
        case VOICE_CLOSED:
            break;
    }
    if (primary) {
        ui_button(bottom, (Rect){10, 68, 300, 38}, primary, UI_PRIMARY);
    }
    if (back) {
        ui_button(bottom, (Rect){10, 119, 300, 38}, back, UI_SECONDARY);
    }
    show(bottom, 12, 214, "转写只进入草稿", UI_ACCENT);
}
