/*
 * voice_tts — on-device TTS for prompt read-aloud (v1.4.0 scaffold).
 *
 * Two paths: on-device esp_tts (preferred) or bridge-supplied WAV via
 * `dash audio_play`. Caller picks via voice_tts_engine_t.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_TTS_AUTO       = 0,
    VOICE_TTS_ON_DEVICE,
    VOICE_TTS_BRIDGE_WAV,
} voice_tts_engine_t;

/* Init audio codec + esp_tts. Idempotent. */
bool voice_tts_init(void);

/* Speak text. Returns immediately; the engine plays asynchronously
 * on the audio task. Concurrent calls queue. */
bool voice_tts_say(const char *text, voice_tts_engine_t engine);

/* Cancel any in-flight speech (e.g. button pressed before TTS done). */
void voice_tts_cancel(void);

#ifdef __cplusplus
}
#endif
