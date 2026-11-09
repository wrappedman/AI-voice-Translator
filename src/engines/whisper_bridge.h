/* whisper_bridge.h — the speech-recognition engine behind the ASR model
 * interface. Only the C++ side (whisper_bridge.cpp) sees whisper.cpp; the rest
 * of the application stays plain C. */
#ifndef OSAI_WHISPER_BRIDGE_H
#define OSAI_WHISPER_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wsp_ctx wsp_ctx;

typedef struct wsp_result {
    char   text[65536];
    char   language[8];     /* ISO 639-1 as detected or forced, e.g. "fa" */
    float  confidence;      /* mean token probability, 0..1 */
} wsp_result;

/* Loads a ggml model file (a verified multilingual GGUF/GGML file). Returns NULL and fills
 * `err` when the file is missing, is not a whisper model, or cannot be read. */
wsp_ctx *wsp_load(const char *path, char *err, size_t cap);
void     wsp_free(wsp_ctx *c);
void     wsp_set_cancel(wsp_ctx *c, volatile int *cancel);
int      wsp_languages(wsp_ctx *c, const char **codes, int max);

/* Returns true when `path` starts with the ggml magic. Cheap: reads 4 bytes. */
bool wsp_is_model_file(const char *path);

/* Transcribes mono 16 kHz float samples. `lang` is a Whisper language code or NULL/"" for
 * automatic detection. Blocking. */
bool wsp_transcribe(wsp_ctx *c, const float *pcm, int64_t n, const char *lang,
                    wsp_result *out, char *err, size_t cap);

/* Language identification only: computes the mel spectrogram, asks whisper for
 * the language, and returns without decoding any text. `wsp_transcribe` with a
 * NULL language calls this itself, because whisper_full() stops after detection
 * when detect_language is set and would return no words at all. */
bool wsp_detect_language(wsp_ctx *c, const float *pcm, int64_t n, char *lang, size_t cap,
                         float *prob);

/* Load with an explicit thread count. File transcription wants a few threads
 * (the UI still has to paint); live transcription uses what the machine has,
 * because its latency is the delay the speaker hears. */
wsp_ctx *wsp_load_ex(const char *path, int n_threads, char *err, size_t cap);
void     wsp_set_threads(wsp_ctx *c, int n);
int      wsp_threads(const wsp_ctx *c);

/* Transcribe with a cap on the decoder's text context. 0 keeps whisper's 16k
 * default, which is right for a whole file; a live partial pass wants a few
 * hundred, because it decodes the same audio over and over. */
bool wsp_transcribe_ex(wsp_ctx *c, const float *pcm, int64_t n, const char *lang,
                       int max_tokens, wsp_result *out, char *err, size_t cap);

/* Short name of the loaded model (its file name without extension). */
const char *wsp_model_name(const wsp_ctx *c);

#ifdef __cplusplus
}
#endif

#endif
