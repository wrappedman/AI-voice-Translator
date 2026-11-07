/* =====================================================================
 *  OpenSpeech AI — desktop shell
 *
 *  Draws the whole application with the canvas/text layer so the same code
 *  renders in the Windows build (GDI/DIB) and in the headless snapshot build
 *  used by the test-suite. Layout is immediate-mode and right-to-left aware.
 * ===================================================================== */
#ifndef OSAI_SHELL_H
#define OSAI_SHELL_H

#include <stdbool.h>
#include "../app/osai_app.h"
#include "ui_kit.h"

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

typedef struct shell_state {
    osa_app      *app;
    shell_screen  screen;
    shell_lang    lang;
    bool          dark;
    float         scale;

    /* transcribe screen */
    char          audio_path[OSAI_MAX_PATH];
    char          transcript[8192];
    char          translation[8192];
    char          detected_lang[8];
    char          transcript_id[40];
    float         transcript_conf;
    double        transcript_rtf;
    double        transcript_dur;
    bool          working;
    char          message[512];
    bool          message_is_error;

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

    /* evaluation */
    char         *eval_json;

    /* settings */
    bool          auto_detect;
    bool          save_audio;
    bool          privacy_logs;
    float         vad_sensitivity;
    int           asr_threads;

    /* Set by the UI when it needs the platform layer to run a modal dialog.
     * The host consumes the request, fills in the result and clears it; the UI
     * itself never touches the filesystem dialog APIs. */
    int           request;        /* 0 none, 1 pick audio, 2 pick model */
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

/* Persisted UI preferences (theme, language, scale, toggles). */
void shell_load_prefs(shell_state *s);
void shell_save_prefs(shell_state *s);

/* i18n: tt(lang, key) returns the interface string. */
const char *tt(shell_lang lang, const char *key);

/* Writes a transient status line at the bottom of the content area. */
void shell_set_message(shell_state *s, bool err, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* Advances the live-capture state machine by `dt` seconds (called by the
 * platform timer; the headless harness calls it too). */
void shell_live_tick(shell_state *s, double dt);

/* Screens other than Transcribe live in shell_screens.c. */
void shell_draw_screen(shell_state *s, ui_context *u, osa_rect body, const ui_input *in);

/* Draws the title bar (resolving the two translation keys) and returns the
 * content rect that remains below it. */
osa_rect shell_draw_chrome(shell_state *s, ui_context *u, osa_rect content,
                           const char *title_key, const char *subtitle_key);
osa_rect shell_screen_chrome(shell_state *s, ui_context *u, osa_rect content,
                             shell_screen sc);
void     shell_draw_toast(shell_state *s, ui_context *u, osa_rect body);

#endif /* OSAI_SHELL_H */
