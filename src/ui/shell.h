/* =====================================================================
 *  OpenSpeech AI — desktop shell
 *
 *  Draws the whole application with the canvas/text layer so the same code
 *  renders in the Windows build (GDI/DIB) and in the headless snapshot build
 *  used by the test-suite. Layout is immediate-mode and right-to-left aware.
 *
 *  The shell owns all application state; the platform layer owns the window,
 *  the file dialogs and the microphone, and never draws.
 * ===================================================================== */
#ifndef OSAI_SHELL_H
#define OSAI_SHELL_H

#include <stdbool.h>
#include "../app/osai_app.h"
#include "ui_kit.h"

/* Shown on the About card and in the window title. One definition, taken from
 * the library version so the exe, the About card and the zip name agree. */
#define OSAI_APP_VERSION OSAI_VERSION

typedef enum shell_screen {
    SCREEN_TRANSCRIBE = 0,
    SCREEN_LIVE,
    SCREEN_TRANSLATE,
    SCREEN_HISTORY,
    SCREEN_MODELS,
    SCREEN_TRAINING,
    SCREEN_EVALUATION,
    SCREEN_SETTINGS,
    SCREEN_ABOUT,
    SCREEN_COUNT
} shell_screen;

/* Which language the interface is drawn in. The Persian UI is the default
 * because the product's primary users are Persian speakers. */
typedef enum shell_lang { UI_FA = 0, UI_EN = 1 } shell_lang;

/* A request the shell raises for the platform layer to satisfy. The shell never
 * opens a dialog itself: it stays renderable in the headless build. */
typedef enum shell_request {
    SHELL_REQ_NONE = 0,
    SHELL_REQ_PICK_AUDIO,      /* open the audio file picker          */
    SHELL_REQ_PICK_DATASET,    /* open the training dataset picker     */
    SHELL_REQ_OPEN_FOLDER,     /* show the data folder in Explorer    */
    SHELL_REQ_SAVE_TRANSCRIPT, /* save the current transcript to a file */
    SHELL_REQ_OPEN_URL         /* open request_url in the browser/mail client */
} shell_request;

typedef struct shell_state {
    osa_app      *app;
    shell_screen  screen;
    shell_lang    lang;
    bool          dark;
    float         scale;

    /* transcribe screen */
    char          audio_path[OSAI_MAX_PATH];
    char          transcript[OSAI_MAX_TEXT];
    char          translation[OSAI_MAX_TEXT];
    char          phonetic[4096];        /* آوانویسی of the translation       */
    char          correction[OSAI_MAX_TEXT];      /* edited transcript awaiting review  */
    char          detected_lang[8];
    char          transcript_id[40];
    float         transcript_conf;
    double        transcript_rtf;
    double        transcript_dur;
    double        transcript_ms;
    int           transcript_words;
    bool          working;
    char          message[512];
    bool          message_is_error;
    int64_t       message_at_ms;      /* when it was raised (auto-dismiss)   */
    int64_t       message_life_ms;    /* 0 = use the default for its kind    */

    /* waveform + playback */
    float        *peaks;
    int           n_peaks;
    bool          playing;
    double        playhead;
    bool          live;
    double        live_time;
    float         live_level;
    int           live_words;

    /* history */
    char         *history_json;
    int           history_sel;
    int           history_scroll;

    /* models */
    char         *models_json;
    int           models_scroll;
    int           models_tab;            /* 0 installed, 1 downloadable */
    int           packs_scroll;
    double        dl_progress;           /* 0..1 while downloading      */
    int64_t       dl_got, dl_total;      /* bytes, for the row's percentage */
    char          dl_pack[64];
    bool          downloading;
    struct dl_job *dl;                   /* legacy alias: same as job for downloads */
    struct shell_job *job;               /* one cancellable background action */
    bool          restoring;
    bool          activating;            /* a model is being loaded right now */
    bool          models_ready;

    /* training */
    int           train_epochs;
    int           train_lang;
    bool          train_gpu;
    char          train_log[2048];
    char          dataset_path[OSAI_MAX_PATH];
    /* Catalogue index of the training set the user picked (-1: the bundled
     * lexicon). The row shows the set even when its file has not been built
     * yet, so "install" is visible before it is needed. */
    int           ds_sel;
    bool          trained;
    bool          training;

    /* evaluation */
    char         *eval_json;

    /* Cached status for the slow-changing machine readings (the About rows).
     * osa_app_get_status() runs three SQL counts plus /proc reads, which is not
     * something to do 60 times a second for a number the eye reads twice. */
    osa_app_status status_cache;
    double         status_ms;
    bool           status_valid;

    /* settings */
    bool          auto_detect;
    bool          save_audio;
    bool          privacy_logs;
    bool          show_phonetic;
    float         vad_sensitivity;
    int           asr_threads;

    /* language pair + register (persisted) */
    char          lang_src[8];
    char          lang_dst[8];
    mt_style      style;

    /* translate screen buffer */
    char          tr_in[OSAI_MAX_TEXT];
    char          tr_out[OSAI_MAX_TEXT];
    char          tr_phonetic[4096];
    char          tr_out_lang[8];
    double        tr_score;

    /* Set by the UI when it needs the platform layer to run a modal dialog.
     * The host consumes the request, fills in the result and clears it; the UI
     * itself never touches the filesystem dialog APIs. */
    shell_request request;
    char          request_url[512];      /* payload for SHELL_REQ_OPEN_URL */
    char          request_path[OSAI_MAX_PATH];

    /* ui bookkeeping */
    int           frame;
    double        anim;
    char          tooltip[256];
} shell_state;

bool shell_init(shell_state *s, osa_app *app);
void shell_shutdown(shell_state *s);

/* Draws one frame into the canvas. Input is read from `in` (which the platform
 * layer fills from mouse/keyboard); pass NULL for a static snapshot. */
void shell_draw(shell_state *s, osa_canvas *c, ui_context *u, const ui_input *in);

/* Loads the waveform peaks of the current audio file for the visualiser. */
bool shell_load_waveform(shell_state *s, const char *path);

/* Runs the transcribe (+translate) action for the current file. Used by the
 * button and by the command-line/snapshot driver. */
bool shell_run_transcribe(shell_state *s);
bool shell_select_audio(shell_state *s, const char *path);
bool shell_activate_model(shell_state *s, const char *id);
void shell_job_tick(shell_state *s);
void shell_cancel_jobs(shell_state *s);
void shell_live_shutdown(shell_state *s);

/* Downloads, verifies and installs on a worker; never activates automatically. */
bool shell_download_start(shell_state *s, const char *id);
void shell_download_tick(shell_state *s);

/* Runs the translate action for the Translate screen's current input. */
void shell_run_translate(shell_state *s);

/* Recomputes the آوانویسی line for the current translation. */
void shell_refresh_phonetic(shell_state *s);

/* Loads a stored transcript (by its id) into the Transcribe screen. Used by the
 * history table and by the recent-items strip. */
bool shell_load_transcript(shell_state *s, const char *id, const char *text,
                           const char *lang, float confidence, double duration);

/* Persisted UI preferences (theme, language, scale, toggles, language pair). */
void shell_load_prefs(shell_state *s);
void shell_save_prefs(shell_state *s);

/* i18n: tt(lang, key) returns the interface string. */
const char *tt(shell_lang lang, const char *key);

/* Synthesises and plays `text` in `lang` (the "listen" buttons). */
void shell_speak(shell_state *s, const char *text, const char *lang);

/* Writes a transient status line at the bottom of the content area. It clears
 * itself after a few seconds (longer for an error) so a stale band of red can
 * never sit on a screen that is working. */
void shell_set_message(shell_state *s, bool err, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
/* Drops a message whose display time has elapsed. Called once per frame. */
void shell_message_tick(shell_state *s);

/* Advances the live-capture state machine by `dt` seconds (called by the
 * platform timer; the headless harness calls it too). */
void shell_live_tick(shell_state *s, double dt);

/* Screens other than Transcribe live in shell_screens.c. */
void shell_draw_screen(shell_state *s, ui_context *u, osa_rect body, const ui_input *in);

/* Applies a drop-down choice captured before ui_popup_draw(). */
void shell_popup_commit(shell_state *s, int popup_id, int sel);

/* Draws the title bar and returns the content rect that remains below it. */
osa_rect shell_draw_chrome(shell_state *s, ui_context *u, osa_rect content,
                           const char *title_key, const char *subtitle_key);
osa_rect shell_screen_chrome(shell_state *s, ui_context *u, osa_rect content,
                             shell_screen sc);
void     shell_draw_toast(shell_state *s, ui_context *u, osa_rect body);

/* Two-letter code of the language name for the interface ("fa"/"en"). */
const char *shell_lang_name(shell_state *s, const char *code);
void shell_run_translate_silent(shell_state *s);

#endif /* OSAI_SHELL_H */
