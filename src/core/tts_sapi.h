/* tts_sapi.h — speaking with the platform's own voices, where there are any.
 *
 * On Windows that is SAPI5: offline, already installed, no download. Everywhere
 * else (and on a Windows install with no voices) these return false and the
 * caller falls back to the builtin synthesiser in tts.c. Declared on every
 * platform so the shell needs no #ifdef. */
#ifndef OSAI_TTS_SAPI_H
#define OSAI_TTS_SAPI_H

#include <stdbool.h>

/* True when the platform can speak: the voice object was created successfully. */
bool osa_tts_platform_available(void);

/* Name of the backend for the About screen ("Windows SAPI"), or "". */
const char *osa_tts_platform_name(void);

/* Starts speaking `utf8_text` (native script, not romanised) and returns
 * immediately; a second call replaces the sentence in progress. `lang` is the
 * BCP-47 tag of the text and is only used to pick a matching voice. */
bool osa_tts_platform_speak(const char *utf8_text, const char *lang);

/* Renders to a WAV file instead of the speaker. Optional: false means "not
 * supported here", never "failed". */
bool osa_tts_platform_save(const char *utf8_text, const char *lang, const char *wav_path);

/* Stops whatever is being spoken. */
void osa_tts_platform_stop(void);

#endif /* OSAI_TTS_SAPI_H */
