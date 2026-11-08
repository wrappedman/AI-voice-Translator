/* tts.h — built-in formant speech synthesizer.
 * Used for (a) the offline demo/self-test audio, (b) generating the synthetic
 * corpus that bootstraps the fixture recognizer, (c) reading text aloud in the
 * "Preview pronunciation" panel. Not a neural TTS: a small rule-based
 * Klatt-style cascade, fully offline and deterministic. */
#ifndef OSAI_TTS_H
#define OSAI_TTS_H

#include "osai_common.h"

/* Synthesizes `text` (Persian or English words known to the built-in lexicon,
 * unknown words are spelled out) into mono 16 kHz float samples. */
float *tts_speak(const char *text, const char *lang, int64_t *n_out);
/* Writes a WAV file. */
bool   tts_speak_to_wav(const char *text, const char *lang, const char *path);
/* Synthesizes a single word from its romanized code (used to build templates). */
float *tts_speak_code(const char *code, int64_t *n_out);
/* Word/pronunciation pair tables (core/lexicon.c), shared with the ASR. */
typedef struct tts_word_pair { const char *w, *code; } tts_word_pair;
const tts_word_pair *osai_lexicon(const char *lang, int *count);

/* True when the word is in the built-in lexicon (so it is pronunciation-testable). */
bool   tts_has_word(const char *word, const char *lang);
/* Romanized pronunciation of a word, or NULL. */
const char *tts_code_for_word(const char *word, const char *lang);

#endif
