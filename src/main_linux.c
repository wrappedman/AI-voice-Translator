/* =====================================================================
 *  OpenSpeech AI — headless host (Linux / CI)
 *
 *  This is not the product binary: it exists so the whole engine + UI can be
 *  exercised without a desktop session. It renders every screen to PNG, runs
 *  the end-to-end transcribe/translate pipeline, and reports an exit code the
 *  test-suite can assert on. The Windows build uses its own host in
 *  src/platform/win32.
 *
 *  Usage:
 *    osai --snapshots DIR [--width W] [--height H] [--lang fa|en] [--light]
 *    osai --transcribe FILE [--json]
 *    osai --selftest
 *    osai --version
 * ===================================================================== */
#include "../app/osai_app.h"
#include "../core/osai_platform.h"
#include "../core/audio.h"
#include "../core/tts.h"
#include "../ui/shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef _WIN32
#include <stdlib.h>          /* setenv/putenv live here, and need _DEFAULT_SOURCE */
#endif

/* Fixed wall clock for snapshots: 2026-11-07 13:29:43 UTC. Deterministic, and
 * late enough that the "11-07 13:29" history row reads naturally. */
#define OSAI_SNAPSHOT_EPOCH_MS 1794058183000LL

/* Where the snapshot renderer keeps its throwaway database. Fixed, because the
 * data root is drawn on the Settings screen. */
static const char *osa_snapshot_data_dir(void) {
#ifdef _WIN32
    return "C:\\OpenSpeechAI-snapshot";
#else
    return "/tmp/openspeech-snapshot";
#endif
}

/* The streams the harness writes into. */
#define OUT stdout

static void usage(void) {
    printf("OpenSpeech AI %s — headless host\n"
           "  --snapshots DIR   render every screen to DIR/screen-*.png\n"
           "  --transcribe FILE transcribe (and translate) a WAV/FLAC/OGG file\n"
           "  --selftest        run the built-in assertions, exit 1 on failure\n"
           "  --train [FILE]    run one training cycle (FILE: optional dataset)\n"
           "  --epochs N        training passes (default 6)\n"
           "  --models-json     print the registered models as JSON\n"
           "  --version         print the version\n"
           "  --width/--height  snapshot size (default 1440x900)\n"
           "  --lang fa|en      interface language (default fa)\n"
           "  --light           light theme (default dark)\n",
           OSAI_VERSION);
}

/* ---- a real sample so the snapshots are not empty ------------------ */

static bool make_sample(const char *path) {
    if (osa_file_exists(path)) return true;
    long long n = 0;
    int64_t nn = 0;
    float *pcm = tts_speak("فایل صوتی را انتخاب کنید", "fa", &nn);
    n = nn;
    if (!pcm || n <= 0) return false;
    bool ok = osa_audio_save_wav(path, pcm, n, OSAI_SR);
    free(pcm);
    return ok;
}

/* ---- snapshots ---------------------------------------------------- */

static int run_snapshots(const char *dir, int w, int h, shell_lang lang, bool dark) {
    if (!osa_mkdirs(dir)) {
        fprintf(stderr, "cannot create %s\n", dir);
        return 1;
    }
    /* The screenshots are committed and CI compares them, so they must not
     * depend on machine state:
     *   - the clock is pinned, because history rows show timestamps;
     *   - the data directory is a scratch copy, because the real one holds
     *     however many transcripts the user happened to make.
     * On the first run the sample transcription below therefore produces the
     * only history row, and the images are byte-identical every time. */
    osa_time_freeze(OSAI_SNAPSHOT_EPOCH_MS);
    {
        /* A fixed path, not one derived from the output directory: the Settings
         * screen prints the data root, so a per-run path would change the
         * image. Reusing the same directory means the sample transcription
         * must not append a new row on every run. */
        const char *scratch = osa_snapshot_data_dir();
        static char db[OSAI_MAX_PATH + 32];
        snprintf(db, sizeof db, "%s/Database/openspeech.db", scratch);
        remove(db);
#ifdef _WIN32
        _putenv_s("OSAI_DATA_DIR", scratch);
#else
        setenv("OSAI_DATA_DIR", scratch, 1);
#endif
    }
    osa_error err;
    osa_app *app = osa_app_create(&err);
    if (!app) {
        fprintf(stderr, "app init failed: %s\n", err.msg);
        return 1;
    }
    osa_canvas canvas;
    if (!canvas_init(&canvas, w, h)) {
        fprintf(stderr, "canvas init failed\n");
        return 1;
    }
    ui_context ui;
    if (!ui_init(&ui, &canvas)) {
        fprintf(stderr, "font/UI init failed (is src/ui/assets present?)\n");
        return 1;
    }
    shell_state shell;
    shell_init(&shell, app);
    shell.lang = lang;
    shell.dark = dark;
    shell.scale = 1.0f;

    /* a real transcription, so the transcribe screen shows real content */
    char sample[OSAI_MAX_PATH];
    snprintf(sample, sizeof sample, "%s/osai-sample-fa.wav", osa_path(OSAI_DIR_CACHE));
    if (make_sample(sample)) {
        osa_strlcpy(shell.audio_path, sample, sizeof shell.audio_path);
        shell_load_waveform(&shell, sample);
        shell_run_transcribe(&shell);
    }

    static const char *names[SCREEN_COUNT] = {
        "transcribe", "live", "translate", "history", "models",
        "training", "evaluation", "settings", "about"
    };
    int written = 0;
    for (int i = 0; i < SCREEN_COUNT; i++) {
        shell.screen = (shell_screen)i;
        shell.message[0] = 0;               /* no leftover toast in a still */
        /* each screen gets its data loaded the way a click on the sidebar would */
        if (i == SCREEN_HISTORY) {
            free(shell.history_json);
            shell.history_json = osa_app_history_json(app, 100, NULL);
        } else if (i == SCREEN_MODELS) {
            free(shell.models_json);
            shell.models_json = osa_app_models_json(app, NULL);
        } else if (i == SCREEN_EVALUATION) {
            free(shell.eval_json);
            shell.eval_json = NULL;          /* show the empty state first */
        }
        shell_draw(&shell, &canvas, &ui, NULL);
        char path[OSAI_MAX_PATH + 64];
        snprintf(path, sizeof path, "%s/screen-%02d-%s.png", dir, i, names[i]);
        if (!canvas_save_png(&canvas, path)) {
            fprintf(stderr, "cannot write %s\n", path);
            continue;
        }
        printf("  %s\n", path);
        written++;
    }

    /* the Models screen's second tab: the catalogue is the part of that screen a
     * first-time user cannot reach without knowing to click, so it gets a frame */
    shell.screen = SCREEN_MODELS;
    shell.models_tab = 1;
    shell.message[0] = 0;
    shell_draw(&shell, &canvas, &ui, NULL);
    {
        char mpath[OSAI_MAX_PATH + 64];
        snprintf(mpath, sizeof mpath, "%s/screen-04b-models-download.png", dir);
        if (canvas_save_png(&canvas, mpath)) {
            printf("  %s\n", mpath);
            written++;
        }
    }
    shell.models_tab = 0;

    /* the evaluation screen with results, as a second frame */
    shell.screen = SCREEN_EVALUATION;
    free(shell.eval_json);
    shell.eval_json = osa_app_evaluate(app, NULL);
    if (shell.eval_json) {
        shell_draw(&shell, &canvas, &ui, NULL);
        char path[OSAI_MAX_PATH + 64];
        snprintf(path, sizeof path, "%s/screen-06b-evaluation-results.png", dir);
        if (canvas_save_png(&canvas, path)) {
            printf("  %s\n", path);
            written++;
        }
    }

    /* English + light theme, so both directions of the layout are covered */
    shell.lang = lang == UI_FA ? UI_EN : UI_FA;
    shell.dark = !dark;
    shell.screen = SCREEN_TRANSCRIBE;
    shell_draw(&shell, &canvas, &ui, NULL);
    char path[OSAI_MAX_PATH + 64];
    snprintf(path, sizeof path, "%s/screen-00b-transcribe-%s-%s.png", dir,
             shell.lang == UI_EN ? "en" : "fa", shell.dark ? "dark" : "light");
    if (canvas_save_png(&canvas, path)) {
        printf("  %s\n", path);
        written++;
    }

    shell_shutdown(&shell);
    ui_shutdown(&ui);
    canvas_free(&canvas);
    osa_app_destroy(app);
    printf("%d images written to %s\n", written, dir);
    return written == SCREEN_COUNT + 3 ? 0 : 1;
}

/* ---- transcribe --------------------------------------------------- */

static void json_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 8 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = (char)ch; }
        else if (ch == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (ch < 0x20) { o += (size_t)snprintf(out + o, cap - o, "\\u%04x", ch); }
        else out[o++] = (char)ch;
    }
    out[o] = 0;
}

static int run_transcribe(const char *path, bool json) {
    if (!osa_file_exists(path)) {
        fprintf(stderr, "no such file: %s\n", path);
        return 2;
    }
    osa_error err;
    osa_app *app = osa_app_create(&err);
    if (!app) {
        fprintf(stderr, "app init failed: %s\n", err.msg);
        return 1;
    }
    osa_app_transcript tr;
    osa_app_translation mt;
    bool ok = osa_app_transcribe_and_translate(app, path, &tr, &mt, &err);
    if (!ok) {
        fprintf(stderr, "transcribe failed: %s\n", err.msg);
        osa_app_destroy(app);
        return 1;
    }
    if (json) {
        char a[OSAI_MAX_PATH * 2], b[8192 * 2], c[8192 * 2];
        json_escape(tr.text, b, sizeof b);
        json_escape(mt.text, c, sizeof c);
        json_escape(path, a, sizeof a);
        printf("{\"audio\":\"%s\",\"language\":\"%s\",\"text\":\"%s\",\"confidence\":%.3f,"
               "\"duration_ms\":%.0f,\"elapsed_ms\":%.0f,\"rtf\":%.3f,"
               "\"translation\":{\"language\":\"%s\",\"text\":\"%s\",\"confidence\":%.3f}}\n",
               a, tr.language, b, tr.confidence, tr.duration_s * 1000.0, tr.proc_ms,
               tr.rtf, mt.target_lang, c, mt.confidence);
    } else {
        printf("language : %s (%.0f%% confidence)\n", tr.language, tr.confidence * 100.0);
        printf("text     : %s\n", tr.text);
        printf("dt       : %.0f ms audio, %.0f ms processing (rtf %.2f)\n",
               tr.duration_s * 1000.0, tr.proc_ms, tr.rtf);
        printf("translate: %s -> %s: %s\n", mt.source_lang, mt.target_lang, mt.text);
    }
    osa_app_destroy(app);
    return 0;
}

/* ---- selftest ----------------------------------------------------- */

static int failures;
static void check(bool cond, const char *what) {
    printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) failures++;
}

static int run_selftest(void) {
    osa_error err;
    osa_app *app = osa_app_create(&err);
    if (!app) {
        fprintf(stderr, "app init failed: %s\n", err.msg);
        return 1;
    }
    printf("app\n");
    osa_app_status st;
    osa_app_get_status(app, &st);
    check(st.db_ready, "database opened");
    check(st.asr_ready, "ASR model loaded");
    check(st.mt_ready, "translation model loaded");
    check(st.n_models >= 2, "builtin models registered");
    check(strlen(st.data_root) > 0, "data root resolved");

    printf("translation\n");
    struct { const char *in, *out; } cases[] = {
        { "سلام حال شما خوب است", "Hello, how are you?" },
        { "ممنون", "Thanks" },
        { "ممنونم", "Thank you" },
        { "خداحافظ", "Goodbye" },
        { "Hello, how are you?", "سلام حال شما خوب است" },
        { "Thank you", "ممنونم" },
        { "Good morning", "صبح بخیر" },
        { "Stop the recording", "ضبط را متوقف کنید" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        osa_app_translation mt;
        bool ok = osa_app_translate(app, cases[i].in, NULL, &mt, &err);
        /* The pair can be long; %.*s keeps gcc from warning about truncation
         * and keeps the label readable in the log. */
        char what[512];
        snprintf(what, sizeof what, "%.180s -> %.180s", cases[i].in,
                 mt.text[0] ? mt.text : "(empty)");
        check(ok && osa_strieq(mt.text, cases[i].out), what);
    }

    printf("speech\n");
    char wav[OSAI_MAX_PATH];
    snprintf(wav, sizeof wav, "%s/selftest.wav", osa_path(OSAI_DIR_CACHE));
    int64_t n = 0;
    float *pcm = tts_speak("فایل صوتی را انتخاب کنید", "fa", &n);
    check(pcm && n > 0, "TTS produced samples");
    if (pcm) {
        float peak = 0;
        for (long long i = 0; i < n; i++) if (fabsf(pcm[i]) > peak) peak = fabsf(pcm[i]);
        check(peak > 0.01f, "TTS is not silent");
        check(osa_audio_save_wav(wav, pcm, n, OSAI_SR), "WAV written");
        free(pcm);
    }
    osa_app_transcript tr;
    osa_app_translation mt;
    bool ok = osa_app_transcribe_and_translate(app, wav, &tr, &mt, &err);
    check(ok, "transcribe + translate completed");
    if (ok) {
        printf("       heard: %s\n", tr.text);
        check(tr.text[0] != 0, "transcript is not empty");
        check(tr.confidence > 0.0f && tr.confidence <= 1.0f, "confidence in range");
        check(tr.duration_s > 0.3, "duration measured");
        check(mt.text[0] != 0, "translation is not empty");
    }
    osa_file_remove(wav);

    printf("database\n");
    char *hist = osa_app_history_json(app, 50, NULL);
    check(hist && strstr(hist, "\"text\""), "history has rows");
    free(hist);
    char *models = osa_app_models_json(app, NULL);
    check(models && strstr(models, "\"active\":true"), "a model is active");
    free(models);

    osa_app_destroy(app);
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

/* ---- main --------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *snapdir = NULL;
    const char *file = NULL;
    bool json = false, light = false, selftest = false, models_json = false;
    int w = 1440, h = 900;
    shell_lang lang = UI_FA;
    const char *train_data = NULL;
    int train_epochs = 6;
    bool train = false;

    if (argc <= 1) { usage(); return 0; }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (osa_streq(a, "--version")) { printf("OpenSpeech AI %s\n", OSAI_VERSION); return 0; }
        else if (osa_streq(a, "--help") || osa_streq(a, "-h")) { usage(); return 0; }
        else if (osa_streq(a, "--snapshots") && i + 1 < argc) snapdir = argv[++i];
        else if (osa_streq(a, "--transcribe") && i + 1 < argc) file = argv[++i];
        else if (osa_streq(a, "--width") && i + 1 < argc) w = atoi(argv[++i]);
        else if (osa_streq(a, "--height") && i + 1 < argc) h = atoi(argv[++i]);
        else if (osa_streq(a, "--json")) json = true;
        else if (osa_streq(a, "--light")) light = true;
        else if (osa_streq(a, "--selftest")) selftest = true;
        else if (osa_streq(a, "--models-json")) models_json = true;
        else if (osa_streq(a, "--train")) {
            /* `--train` on its own trains on the bundled lexicon; a following
             * argument that is not a switch is a dataset file. */
            train = true;
            if (i + 1 < argc && !osa_starts(argv[i + 1], "--")) train_data = argv[++i];
        }
        else if (osa_streq(a, "--epochs") && i + 1 < argc) train_epochs = atoi(argv[++i]);
        else if (osa_streq(a, "--lang") && i + 1 < argc)
            lang = osa_starts(argv[++i], "en") ? UI_EN : UI_FA;
        else { fprintf(stderr, "unknown argument: %s\n", a); usage(); return 2; }
    }

    if (snapdir) return run_snapshots(snapdir, w, h, lang, !light);
    if (file) return run_transcribe(file, json);
    if (selftest) return run_selftest();
    if (train) {
        osa_error err;
        osa_app *app = osa_app_create(&err);
        if (!app) { fprintf(stderr, "app init failed: %s\n", err.msg); return 1; }
        osa_train_result res;
        memset(&res, 0, sizeof res);
        bool ok = osa_app_train(app, train_data, train_epochs, &res, &err);
        printf("%s\n", ok ? res.log : err.msg);
        if (ok) printf("model: %s\npath: %s\nwords: %d, added: %d\n", res.model_id,
                       res.model_path, res.n_words, res.n_added);
        osa_app_destroy(app);
        return ok ? 0 : 1;
    }
    if (models_json) {
        osa_error err;
        osa_app *app = osa_app_create(&err);
        if (!app) { fprintf(stderr, "app init failed: %s\n", err.msg); return 1; }
        char *json = osa_app_models_json(app, &err);
        if (json) { printf("%s\n", json); free(json); }
        osa_app_destroy(app);
        return json ? 0 : 1;
    }
    usage();
    return 0;
}
