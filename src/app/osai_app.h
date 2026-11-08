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

/* ---- language pair ------------------------------------------------- */

/* A language the application can translate to or from. The pair itself is the
 * user's choice, exactly like every other translator: `osa_app_set_languages`
 * is what the header drop-downs call. */
typedef struct osa_lang_info {
    char code[8];        /* "fa", "en"                            */
    char name_fa[48];    /* Persian name, shown in the Persian UI */
    char name_en[48];    /* English name                          */
    bool installed;      /* the pack needed for this direction    */
    bool paired;         /* a pack exists for from<->to with the current pair */
} osa_lang_info;

/* Returns the number of languages written into `out`. */
int  osa_app_languages(osa_app *app, osa_lang_info *out, int max);

/* Fixes the direction. `src`/`dst` are language codes; pass "" for either to
 * mean "detect". Returns false (with a message) for an unsupported pair. */
bool osa_app_set_languages(osa_app *app, const char *src, const char *dst, osa_error *err);
void osa_app_get_languages(osa_app *app, char *src, size_t src_cap,
                           char *dst, size_t dst_cap);

/* ---- translation -------------------------------------------------- */

/* Register of the produced translation: neutral, formal/written or everyday
 * speech. Persisted in settings.ini, applied by every translate call. */
void     osa_app_set_style(osa_app *app, mt_style style);
mt_style osa_app_style(const osa_app *app);

/* Translate `text`; `src` may be "" to auto-detect. The row is persisted, so the
 * history screen shows it immediately. */
bool osa_app_translate(osa_app *app, const char *text, const char *src,
                       osa_app_translation *out, osa_error *err);

/* Translate for display only: nothing is written to the database. Returns a
 * pointer to an internal buffer, valid until the next call. Used by the history
 * list, which previews many rows per frame. */
const char *osa_app_translate_preview(osa_app *app, const char *text);

/* آوانویسی: Persian transliteration of `text` (English input gets Persian
 * letters with diacritics; Persian input is returned unchanged because it is
 * already readable). Returns false when there is nothing to transliterate. */
bool osa_app_pronounce(const char *text, const char *lang, char *out, size_t cap);

/* One-shot: transcribe then translate, linking both rows. */
bool osa_app_transcribe_and_translate(osa_app *app, const char *path,
                                      osa_app_transcript *tr,
                                      osa_app_translation *mt, osa_error *err);

/* ---- history ------------------------------------------------------ */

/* JSON array of recent transcripts, newest first. Caller frees with free(). */
char *osa_app_history_json(osa_app *app, int limit, osa_error *err);

/* Delete one transcript, its stored audio and any linked translation. */
bool osa_app_delete_transcript(osa_app *app, const char *id, osa_error *err);

/* ---- downloads ---------------------------------------------------- */

/* The catalogue of models and language packs that can be downloaded.
 *
 * Everything the application ships is installed offline; this list is for the
 * extra material a user may want (a bigger ASR model, another language pair).
 * It is a static table compiled into the binary, so the screen is complete and
 * browsable with the machine offline; only pressing "download" touches the
 * network.
 */
typedef struct osa_pack_info {
    char id[64];
    char name[128];
    char kind[16];          /* "asr" or "mt" or "lang"               */
    char language[16];      /* "fa", "en", "fa->en", ...             */
    char license[64];
    char url[512];          /* empty: this entry cannot be fetched   */
    char sha256[72];
    char notes[256];
    double size_mb;
    bool installed;
} osa_pack_info;

/* Number of catalogue entries written into `out`. */
int  osa_app_packs(osa_app *app, osa_pack_info *out, int max);

/* Downloads one catalogue entry into the models directory, verifies it against
 * the recorded SHA-256 and registers it as an installed model. Blocking; the
 * caller reports progress through `progress`. Returns 0 on success, -1 on
 * failure (err has the reason), -2 when cancelled. */
int  osa_app_download_pack(osa_app *app, const char *id,
                           void (*progress)(int64_t got, int64_t total, void *ud), void *ud,
                           volatile int *cancel, osa_error *err);

/* Installs a language pack from a local file the user picked (the offline path
 * for the same feature). */
bool osa_app_install_pack_file(osa_app *app, const char *path, osa_error *err);

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

/* ---- training ----------------------------------------------------- */

/* Adds a recording of one word to the active ASR model and saves the result as
 * a new, versioned model (the previous one is never deleted). This is what the
 * Training screen runs: it is the same "add an exemplar" primitive the engine
 * already uses, exposed with provenance and a rollback path. */
typedef struct osa_train_result {
    char model_id[64];
    char model_path[OSAI_MAX_PATH];
    int  n_words;
    int  n_added;
    bool activated;              /* the new version is the active ASR model */
    char log[1024];
} osa_train_result;

/* `pairs` is a JSON-ish list of "audio<tab>text" lines, one per recording, as
 * produced by the dataset scanner. When `pairs` is NULL the builtin lexicon's
 * own synthesised samples are used, so the run does something verifiable even
 * with no data of the user's own. */
bool osa_app_train(osa_app *app, const char *dataset_path, int epochs,
                   osa_train_result *out, osa_error *err);

/* ---- evaluation --------------------------------------------------- */

/* Evaluate the builtin ASR engine against a small fixed set of synthesised
 * sentences and return WER/CER as JSON. Used by the "Evaluation" screen and by
 * `make tests`. Caller frees with free(). */
char *osa_app_evaluate(osa_app *app, osa_error *err);

#endif /* OSAI_APP_H */
