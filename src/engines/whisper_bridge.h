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
    char   text[8192];
    char   language[8];     /* ISO 639-1 as detected or forced, e.g. "fa" */
    float  confidence;      /* mean token probability, 0..1 */
} wsp_result;

/* Loads a ggml model file (for example ggml-tiny.bin). Returns NULL and fills
 * `err` when the file is missing, is not a whisper model, or cannot be read. */
wsp_ctx *wsp_load(const char *path, char *err, size_t cap);
void     wsp_free(wsp_ctx *c);

/* Returns true when `path` starts with the ggml magic. Cheap: reads 4 bytes. */
bool wsp_is_model_file(const char *path);

/* Transcribes mono 16 kHz float samples. `lang` is "fa", "en" or NULL/"" for
 * automatic detection. Blocking. */
bool wsp_transcribe(wsp_ctx *c, const float *pcm, int64_t n, const char *lang,
                    wsp_result *out, char *err, size_t cap);

/* Short name of the loaded model (its file name without extension). */
const char *wsp_model_name(const wsp_ctx *c);

#ifdef __cplusplus
}
#endif

#endif
