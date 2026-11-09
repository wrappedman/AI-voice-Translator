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
    char     text[OSAI_MAX_TEXT];
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
    char     text[OSAI_MAX_TEXT];
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
    bool     lid_ready;
    char     lid_model[128];
    char     lid_backend[32];
    char     asr_model[128];
    char     asr_backend[32];
    char     mt_model[128];
    char     mt_backend[32];
    int      n_models;
    int      n_transcripts;
    int      n_translations;
    double   cpu_percent;
    double   ram_used_mb;      /* whole machine, for context */
    double   app_ram_mb;       /* this process: the honest "in use" figure */
    double   disk_free_mb;
    char     data_root[OSAI_MAX_PATH];
} osa_app_status;

typedef struct osa_app osa_app;

/* ---- lifecycle ---------------------------------------------------- */

/* Open storage without installing or activating anything. Saved activations
 * are restored explicitly on a worker via osa_app_restore_saved(). */
osa_app *osa_app_create(osa_error *err);
void     osa_app_destroy(osa_app *app);

/* Re-scan the models directory and report readiness (used after installing or
 * activating a model). */
bool osa_app_refresh(osa_app *app, osa_error *err);
typedef struct osa_lang_info osa_lang_info;
bool osa_app_restore_saved(osa_app *app, osa_error *err);
bool osa_app_ready(osa_app *app);
void osa_app_cancel(osa_app *app);
void osa_app_reset_cancel(osa_app *app);
int osa_app_asr_languages(osa_app *app, osa_lang_info *out, int max);
const char *osa_lang_name(const char *code, bool persian);

/* ---- status ------------------------------------------------------- */

void osa_app_get_status(osa_app *app, osa_app_status *out);

/* ---- ASR ---------------------------------------------------------- */

/* Transcribe an audio file (wav/mp3/flac/ogg). Persists the audio copy, the
 * transcript row and returns the text. */
bool osa_app_transcribe_file(osa_app *app, const char *path,
                             osa_app_transcript *out, osa_error *err);

/* Transcribe raw mono float samples (used by the live microphone path). */
/* Recognition without side effects: text, language and confidence only. Safe to
 * call from a worker thread; it serialises with every other recogniser use. */
bool osa_app_recognize(osa_app *app, const float *samples, int64_t n,
                       char *text, size_t text_cap, char *lang, size_t lang_cap,
                       float *confidence, osa_error *err);

/* Threads each worker may use: half of this machine's cores, capped at 8 by
 * default and never zero. The remainder is headroom for the UI, the operating
 * system and audio capture, which is what stops a translation from pinning
 * every core at 100% and making the whole PC unresponsive. Set from Settings. */
void osa_app_set_threads(osa_app *app, int threads);
int  osa_app_threads(osa_app *app);

/* The same, for the live screen: `partial` marks a pass over a sentence the
 * speaker has not finished, so the decoder's text context is capped (a partial
 * is re-decoded every second and must cost little) and the engine may use more
 * threads than a file pass. Results are identical in shape. */
bool osa_app_recognize_live(osa_app *app, const float *samples, int64_t n, bool partial,
                            char *text, size_t text_cap, char *lang, size_t lang_cap,
                            float *confidence, osa_error *err);

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
/* Same active models, no history row: for repeatedly refreshed live partials. */
bool osa_app_translate_live(osa_app *app, const char *text, const char *src,
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

/* Verified downloadable models. No weights or phrasebooks are bundled. */
typedef struct osa_pack_info {
    char id[64];
    char name[128];
    char kind[16];          /* "asr" or "mt" or "lang"               */
    char language[512];     /* model-derived capability list */
    char license[64];
    char url[512];          /* empty: this entry cannot be fetched   */
    char sha256[72];
    char notes[256];
    double size_mb;
    int min_ram_mb;         /* approximate memory the model needs while running */
    bool installed;
    /* Every catalog row is a real, integrity-checked download. */
    bool installable;
} osa_pack_info;

/* Number of catalogue entries written into `out`. */
int  osa_app_packs(osa_app *app, osa_pack_info *out, int max);

/* Downloads one catalogue entry into the models directory, verifies it against
 * the recorded SHA-256 and registers it as an installed model. Blocking; the
 * caller reports progress through `progress`. Returns 0 on success, -1 on
 * failure (err has the reason), -2 when cancelled. */
/* Thread-safe half of a download: fetches and verifies only. The result is a
 * file in the downloads folder, to be installed on the UI thread. */
int  osa_app_fetch_pack(const char *id,
                        void (*progress)(int64_t got, int64_t total, void *ud), void *ud,
                        volatile int *cancel, char *file_out, size_t cap, osa_error *err);

int  osa_app_download_pack(osa_app *app, const char *id,
                           void (*progress)(int64_t got, int64_t total, void *ud), void *ud,
                           volatile int *cancel, osa_error *err);

/* Builds and installs one of the packs the application carries (the offline
 * path: no network, no file picker). Reloads the translation engine, so the
 * pack is active when the call returns. Entries whose material is not in the
 * binary fail with the catalogue note as the reason. */
bool osa_app_install_builtin_pack(osa_app *app, const char *id, osa_error *err);

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

/* Deletes a model: unloads it when active, removes the database row and deletes
 * the file (plus any leftover download). This is the only destructive model
 * operation and the UI asks for confirmation before calling it. */
bool osa_app_remove_model(osa_app *app, const char *id, osa_error *err);

/* ---- models folder ------------------------------------------------ */

/* Where installed model files are written. Defaults to <data>/Models and can be
 * pointed at any writable folder (an external disk, a shared model library). */
void osa_app_models_dir(osa_app *app, char *out, size_t cap);
bool osa_app_set_models_dir(osa_app *app, const char *dir, osa_error *err);

typedef struct osa_sync_result {
    int scanned;                 /* candidate files looked at              */
    int added;                   /* newly registered models                */
    int existing;                /* already registered, nothing to do      */
    int unknown;                 /* not in the verified catalogue          */
    char unknown_names[400];     /* a few names, for an honest report      */
    /* The file being read right now. Hashing one large model takes minutes and
     * the file counter cannot move meanwhile, so the UI shows this instead of
     * an apparently frozen "0 / 6". */
    char current[128];
} osa_sync_result;

/* Identifies the files already present in the models folder by SHA-256 against
 * the pinned catalogue and registers the matches as installed (inactive), so the
 * user can activate them from the Models screen. Unknown files are reported, not
 * registered: an unverified file can never become an engine. */
bool osa_app_sync_models(osa_app *app, volatile int *cancel,
                         void (*progress)(int64_t done, int64_t total, void *ud), void *ud,
                         osa_sync_result *out, osa_error *err);

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

/* One entry of the in-app training-data catalogue. Training data is picked the
 * same way a model is: a list inside the application, one row per set, with a
 * press that makes it available. Nothing here needs a file manager. */
typedef struct osa_dataset_info {
    char  id[64];
    char  name[160];
    char  language[16];
    char  notes[320];
    int   n_pairs;               /* pairs in the set                        */
    bool  installed;             /* the file is in the data folder already  */
    bool  installable;           /* the application can build it offline    */
    char  path[OSAI_MAX_PATH];   /* the file, once installed                */
} osa_dataset_info;

/* Lists the catalogue (bundled sets, built on press). Returns the count. */
int  osa_app_datasets(osa_app *app, osa_dataset_info *out, int max);
/* Builds and installs one set into <data>/Datasets, returning its path. */
bool osa_app_install_dataset(osa_app *app, const char *id, char *path_out,
                             size_t cap, osa_error *err);

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
