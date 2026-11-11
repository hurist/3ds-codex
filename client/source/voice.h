#ifndef VOICE_H
#define VOICE_H

#include "composer.h"

typedef enum {
    VOICE_CLOSED,
    VOICE_READY,
    VOICE_RECORDING,
    VOICE_TRANSCRIBING,
    VOICE_CANCELLING,
    VOICE_RESULT,
    VOICE_ERROR,
} VoicePhase;

typedef struct {
    VoicePhase phase;
    u8 *samples;
    unsigned length;
    unsigned scroll;
    u64 started_at;
    char error[128];
    char transcript[MESSAGE_CAPACITY];
} Voice;

void voice_start(Voice *voice);
bool voice_recording_finished(const Voice *voice);
bool voice_stop(Voice *voice);
void voice_receive(Voice *voice, const char *response, const char *error);
void voice_close(Voice *voice);
void voice_scroll(Voice *voice, u32 repeat);
void voice_draw(const Voice *voice, Screen top, Screen bottom, const char *title);

#endif
