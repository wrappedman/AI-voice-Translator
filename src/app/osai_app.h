/* =====================================================================
 *  OpenSpeech AI — Local Application API
 *
 *  This is the only surface the UI is allowed to touch: the domain design
 *  requires ASR, translation, storage and model management to sit behind a
 *  stable local API so the desktop shell can be replaced (the shipped build
 *  uses a native GDI canvas; a WPF/XAML shell can be dropped in later without
 *  touching the engines).
 *
 *  Every entry point is synchronous and returns false with a readable message
 *  in `osa_error` on failure. Nothing here performs network I/O, so the whole
 *  API is safe to call with the machine offline — which is the product's
 *  central promise.
 * ===================================================================== */
#ifndef OSAI_APP_H
#define OSAI_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "../core/osai_common.h"
#include "../core/osai_store.h"
#include "../core/audio.h"
#include "../engines/asr.h"
#include "../engines/mt.h"

/* Result of one "transcribe an audio file" call. */
typedef struct osa_app_transcript {
    char     id[40];
    char     text[8192];
    char     language[8];
    char     model_id[40];
    double   confidence;
    double   duration_s;        /* audio length                  */
    double   proc_ms;           /* wall-clock processing time    */
    double   rtf;               /* proc_ms / (duration*1000)     */
    int      n_words;
} osa_app_transcript;

/* Result of one "translate" call, including the stored row id. */
typedef struct osa_app_translation {
    char     id[40];
    char     text[8192];
    char     source_lang[8];
    char     target_lang[8];
    char     engine[32];
    double   confidence;
    double   elapsed_ms;
} osa_app_translation;

/* Snapshot of the machine + data directories, for the status bar. */
typedef struct osa_app_status {
    bool     db_ready;
    bool     asr_ready;
    bool     mt_ready;
    char     asr_model[128];
    char     asr_backend[32];
    char     mt_model[128];
    char     mt_backend[32];
    int      n_models;
    int      n_transcripts;
    int      n_translations;
    double   cpu_percent;
    double   ram_used_mb;
    double   disk_free_mb;
    char     data_root[OSAI_MAX_PATH];
} osa_app_status;

typedef struct osa_app osa_app;

/* ---- lifecycle ---------------------------------------------------- */

/* Create the data directory tree, open/migrate the SQLite database and load the
 * builtin ASR + translation models. Safe to call once per process. */
osa_app *osa_app_create(osa_error *err);
void     osa_app_destroy(osa_app *app);

/* Re-scan the models directory and report readiness (used after installing or
 * activating a model). */
bool osa_app_refresh(osa_app *app, osa_error *err);

/* ---- status ------------------------------------------------------- */

void osa_app_get_status(osa_app *app, osa_app_status *out);

/* ---- ASR ---------------------------------------------------------- */

/* Transcribe an audio file (wav/mp3/flac/ogg). Persists the audio copy, the
 * transcript row and returns the text. */
bool osa_app_transcribe_file(osa_app *app, const char *path,
                             osa_app_transcript *out, osa_error *err);

/* Transcribe raw mono float samples (used by the live microphone path). */
bool osa_app_transcribe_samples(osa_app *app, const float *samples, int64_t n,
                                osa_app_transcript *out, osa_error *err);

/* Detected language tag ("fa"/"en") of the last transcription, or "" — kept
 * separate so the UI can show the auto-detected language even for empty text. */
const char *osa_app_last_language(const osa_app *app);

/* ---- translation -------------------------------------------------- */

/* Translate `text`; `src` may be "" to auto-detect. The row is persisted, so the
 * history screen shows it immediately. */
bool osa_app_translate(osa_app *app, const char *text, const char *src,
                       osa_app_translation *out, osa_error *err);

/* Translate for display only: nothing is written to the database. Returns a
 * pointer to an internal buffer, valid until the next call. Used by the history
 * list, which previews many rows per frame. */
const char *osa_app_translate_preview(osa_app *app, const char *text);

/* One-shot: transcribe then translate, linking both rows. */
bool osa_app_transcribe_and_translate(osa_app *app, const char *path,
                                      osa_app_transcript *tr,
                                      osa_app_translation *mt, osa_error *err);

/* ---- history ------------------------------------------------------ */

/* JSON array of recent transcripts, newest first. Caller frees with free(). */
char *osa_app_history_json(osa_app *app, int limit, osa_error *err);

/* Delete one transcript, its stored audio and any linked translation. */
bool osa_app_delete_transcript(osa_app *app, const char *id, osa_error *err);

/* ---- models ------------------------------------------------------- */

/* JSON array describing installed ASR/MT models (id, name, kind, active,
 * licence, source, bytes). Caller frees with free(). */
char *osa_app_models_json(osa_app *app, osa_error *err);

/* Activate a model by id. Never deletes the previous one: it stays installed and
 * can be re-activated later (the "never lose the previous version" rule). */
bool osa_app_activate_model(osa_app *app, const char *id, osa_error *err);

/* Import a .osam (ASR) or .osmt (translation) container into the models dir,
 * hash it and register it. Returns the new id in `id_out`. */
bool osa_app_import_model(osa_app *app, const char *path, char *id_out, size_t cap,
                          osa_error *err);

/* ---- corrections -------------------------------------------------- */

/* Store a user correction for later review. It is deliberately NOT fed back into
 * the model: the design requires corrections to be validated and applied in a
 * separate training cycle. */
bool osa_app_add_correction(osa_app *app, const char *transcription_id,
                            const char *corrected, osa_error *err);

/* ---- evaluation --------------------------------------------------- */

/* Evaluate the builtin ASR engine against a small fixed set of synthesised
 * sentences and return WER/CER as JSON. Used by the "Evaluation" screen and by
 * `make tests`. Caller frees with free(). */
char *osa_app_evaluate(osa_app *app, osa_error *err);

#endif /* OSAI_APP_H */
