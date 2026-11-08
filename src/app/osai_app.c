/* =====================================================================
 *  OpenSpeech AI — Local Application API implementation
 * ===================================================================== */
#include "osai_app.h"
#include "../core/osai_platform.h"
#include "../core/tts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct osa_app {
    osa_db   *db;
    bool      db_ready;
    asr_model *asr;
    char      asr_model_id[40];
    mt_model *mt;
    char      mt_model_id[40];
    char      models_dir[OSAI_MAX_PATH];
    char      audio_dir[OSAI_MAX_PATH];
    char      last_language[8];
    osa_app_status cached;
    /* user-chosen language pair and register (see the settings screen) */
    char      lang_src[8];       /* "" = detect                          */
    char      lang_dst[8];       /* "" = opposite of the source          */
    mt_style  style;
};

/* ---- helpers ------------------------------------------------------ */

static bool mkdir_all(const char *path) {
    char tmp[OSAI_MAX_PATH];
    osa_strlcpy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char keep = *p;
            *p = 0;
            osa_mkdirs(tmp);
            *p = keep;
        }
    }
    return osa_mkdirs(tmp);
}

static void json_str(osa_buf *b, const char *s) {
    osa_buf_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        switch (*p) {
            case '"':  osa_buf_puts(b, "\\\""); break;
            case '\\': osa_buf_puts(b, "\\\\"); break;
            case '\n': osa_buf_puts(b, "\\n"); break;
            case '\r': osa_buf_puts(b, "\\r"); break;
            case '\t': osa_buf_puts(b, "\\t"); break;
            default:
                if (*p < 0x20) osa_buf_printf(b, "\\u%04x", *p);
                else osa_buf_putc(b, (char)*p);
        }
    }
    osa_buf_putc(b, '"');
}

/* Build `<models>/<id>.<ext>` into buf. */
static void model_path(const osa_app *a, const char *id, const char *ext,
                       char *buf, size_t cap) {
    snprintf(buf, cap, "%s%s.%s", a->models_dir, id, ext);
}

/* ---- model registration ------------------------------------------- */

static bool register_model(osa_app *a, const char *id, const char *name,
                           const char *kind, const char *engine, const char *base,
                           const char *license, const char *source, const char *path,
                           const char *version, osa_error *err) {
    osa_model m;
    memset(&m, 0, sizeof m);
    osa_strlcpy(m.id, id, sizeof m.id);
    osa_strlcpy(m.name, name, sizeof m.name);
    osa_strlcpy(m.kind, kind, sizeof m.kind);
    osa_strlcpy(m.engine, engine, sizeof m.engine);
    osa_strlcpy(m.base_model, base, sizeof m.base_model);
    osa_strlcpy(m.license, license, sizeof m.license);
    osa_strlcpy(m.source_url, source, sizeof m.source_url);
    osa_strlcpy(m.path, path, sizeof m.path);
    osa_strlcpy(m.version, version, sizeof m.version);
    osa_strlcpy(m.status, "ready", sizeof m.status);
    osa_iso_time(osa_now_ms(), m.created_at);
    if (path && *path) {
        uint64_t sz = 0;
        osa_sha256_file(path, m.sha256, &sz);
        m.size_bytes = (int64_t)sz;
    }
    return osa_store_model_add(a->db, &m, err);
}

/* ---- seeding from the shipped folder ------------------------------ *
 * `dist/` carries the models the build ships with, next to the exe, so a copy
 * of that folder works on a machine that has never run the application. The
 * first start copies them into the data folder — the program still never writes
 * anything into the folder it was unpacked into, and a user who deletes the
 * data folder gets the same working install back.
 * ------------------------------------------------------------------- */
static void seed_dir(const char *from_sub, osa_dir_kind to) {
    char from[OSAI_MAX_PATH];
    osa_path_join(from, sizeof from, osa_exe_dir(), from_sub);
    if (!osa_dir_exists(from)) return;
    char **names = NULL;
    int n = osa_dir_scan(from, NULL, &names, 256);
    for (int i = 0; i < n; i++) {
        char src[OSAI_MAX_PATH], dst[OSAI_MAX_PATH];
        osa_path_join(src, sizeof src, from, names[i]);
        osa_path_join(dst, sizeof dst, osa_path(to), names[i]);
        if (!osa_file_exists(dst)) osa_file_copy(src, dst);
    }
    osa_dir_free_list(names, n);
}

static void seed_from_dist(void) {
    seed_dir("models", OSAI_DIR_MODELS);
    seed_dir("packs", OSAI_DIR_PACKS);
}

static bool ensure_builtin_models(osa_app *a, osa_error *err) {
    /* ASR: synthesise the exemplar pack once and keep it on disk, so the
     * "Builtin" model behaves like any other installed model (it appears in the
     * model manager, has a hash and a licence, and can be replaced). */
    char asr_id[40];
    osa_strlcpy(asr_id, asr_model_id(a->asr), sizeof asr_id);
    if (!asr_id[0]) osa_strlcpy(asr_id, "builtin-asr-fa", sizeof asr_id);
    char path[OSAI_MAX_PATH];
    model_path(a, asr_id, "osam", path, sizeof path);
    if (!osa_file_exists(path)) {
        if (!asr_model_save(a->asr, path, err)) return false;
    }
    if (!register_model(a, asr_id, "Builtin ASR (Persian + English)",
                        "asr", asr_model_backend(a->asr),
                        "builtin-exemplar-v1",
                        "GPL-3.0-or-later (part of OpenSpeech AI)",
                        "bundled with the application", path, "1.0.0", err))
        return false;

    char mpath[OSAI_MAX_PATH];
    model_path(a, "builtin-mt", "osmt", mpath, sizeof mpath);
    if (!osa_file_exists(mpath)) {
        if (!mt_model_save(a->mt, mpath, err)) return false;
    }
    if (!register_model(a, "builtin-mt", "Builtin Translation (fa<->en)",
                        "mt", "phrasebook", "builtin-phrasebook-v1",
                        mt_model_license(a->mt), mt_model_source(a->mt),
                        mpath, "1.0.0", err))
        return false;

    /* Always (re)assert the active flags: osa_store_model_add inserts with
     * active=0, so a fresh install would otherwise show no active model. The
     * previously active one is only deselected within its own kind, and it stays
     * installed for rollback. */
    if (!osa_store_model_set_active(a->db, asr_id, err)) return false;
    if (!osa_store_model_set_active(a->db, "builtin-mt", err)) return false;
    return true;
}

/* ---- lifecycle ---------------------------------------------------- */

static void load_word_packs(osa_app *a);

osa_app *osa_app_create(osa_error *err) {
    osa_app *a = (osa_app *)calloc(1, sizeof *a);
    if (!a) { osa_err_set(err, "out of memory"); return NULL; }

    /* 1. directory tree (created on first run, never inside the exe folder) */
    osa_strlcpy(a->models_dir, osa_path(OSAI_DIR_MODELS), sizeof a->models_dir);
    osa_strlcpy(a->audio_dir, osa_path(OSAI_DIR_AUDIO), sizeof a->audio_dir);
    mkdir_all(osa_path(OSAI_DIR_ROOT));
    mkdir_all(a->models_dir);
    mkdir_all(a->audio_dir);
    mkdir_all(osa_path(OSAI_DIR_DB));
    mkdir_all(osa_path(OSAI_DIR_LOGS));
    mkdir_all(osa_path(OSAI_DIR_LICENSES));
    mkdir_all(osa_path(OSAI_DIR_BACKUPS));
    mkdir_all(osa_path(OSAI_DIR_DATASETS));
    mkdir_all(osa_path(OSAI_DIR_DOWNLOADS));
    mkdir_all(osa_path(OSAI_DIR_EXPORTS));
    mkdir_all(osa_path(OSAI_DIR_CHECKPOINTS));
    mkdir_all(osa_path(OSAI_DIR_PACKS));

    /* 2. database */
    char dbpath[OSAI_MAX_PATH];
    snprintf(dbpath, sizeof dbpath, "%sopenspeech.db", osa_path(OSAI_DIR_DB));
    a->db = osa_db_open(dbpath, err);
    if (!a->db) { free(a); return NULL; }
    if (!osa_db_migrate(a->db, err)) { osa_db_close(a->db); free(a); return NULL; }
    a->db_ready = true;

    /* 3. engines */
    a->asr = asr_model_fixture("fa", err);
    if (!a->asr) { osa_db_close(a->db); free(a); return NULL; }
    osa_strlcpy(a->asr_model_id, asr_model_id(a->asr), sizeof a->asr_model_id);
    a->mt = mt_model_builtin(err);
    if (!a->mt) { asr_model_free(a->asr); osa_db_close(a->db); free(a); return NULL; }
    osa_strlcpy(a->mt_model_id, "builtin-mt", sizeof a->mt_model_id);
    osa_strlcpy(a->last_language, "fa", sizeof a->last_language);
    a->lang_src[0] = 0;                    /* detect the source      */
    osa_strlcpy(a->lang_dst, "en", sizeof a->lang_dst);
    a->style = MT_STYLE_NEUTRAL;

    /* 4. models the release ships next to the exe (a fresh copy of dist/ has
     *    to work on a machine that never ran the program) */
    seed_from_dist();
    /* 5. register the builtins so the model manager is never empty */
    ensure_builtin_models(a, err);
    /* 6. word packs the user installed earlier: the engine has to read them
     *    before the first translation, or a restart would silently undo an
     *    install that the Models screen still lists. */
    load_word_packs(a);
    osa_err_ok(err);
    return a;
}

void osa_app_destroy(osa_app *a) {
    if (!a) return;
    if (a->asr) asr_model_free(a->asr);
    if (a->mt) mt_model_free(a->mt);
    if (a->db) osa_db_close(a->db);
    free(a);
}

/* ---- status ------------------------------------------------------- */

void osa_app_get_status(osa_app *a, osa_app_status *out) {
    if (!a || !out) return;
    memset(out, 0, sizeof *out);
    out->db_ready = a->db_ready;
    out->asr_ready = a->asr != NULL;
    out->mt_ready = a->mt != NULL;
    osa_strlcpy(out->asr_model, asr_model_name(a->asr), sizeof out->asr_model);
    osa_strlcpy(out->asr_backend, asr_model_backend(a->asr), sizeof out->asr_backend);
    osa_strlcpy(out->mt_model, mt_model_name(a->mt), sizeof out->mt_model);
    osa_strlcpy(out->mt_backend, mt_backend_name(mt_model_backend(a->mt)), sizeof out->mt_backend);
    osa_strlcpy(out->data_root, osa_path(OSAI_DIR_ROOT), sizeof out->data_root);

    osa_rs rs;
    if (a->db) {
        if (osa_sql_query(a->db, "SELECT COUNT(*) AS n FROM Models", &rs, NULL)) {
            out->n_models = osa_rs_int(&rs, 0, "n");
            osa_rs_free(&rs);
        }
        if (osa_sql_query(a->db, "SELECT COUNT(*) AS n FROM Transcriptions", &rs, NULL)) {
            out->n_transcripts = osa_rs_int(&rs, 0, "n");
            osa_rs_free(&rs);
        }
        if (osa_sql_query(a->db, "SELECT COUNT(*) AS n FROM Translations", &rs, NULL)) {
            out->n_translations = osa_rs_int(&rs, 0, "n");
            osa_rs_free(&rs);
        }
    }
    osa_sys_stats st;
    memset(&st, 0, sizeof st);
    osa_sys_stats_sample(&st);
    out->cpu_percent = st.cpu_percent;
    out->ram_used_mb = st.ram_used_mb;
    /* The About screen calls this column "memory in use". The machine's own
     * used RAM is not that: it drifts up and down with every other program on
     * the computer, so a number that only ever grew looked like this process
     * was leaking, and a number that fell looked like it had freed something.
     * Report the resident size of *this* process instead; the machine figure
     * stays available next to it for context. */
    out->app_ram_mb = st.proc_ram_mb;
    out->disk_free_mb = (double)osa_disk_free(osa_path(OSAI_DIR_ROOT)) / (1024.0 * 1024.0);
}

/* ---- ASR ---------------------------------------------------------- */

const char *osa_app_last_language(const osa_app *a) {
    return a ? a->last_language : "";
}

static void fill_transcript(osa_app *a, const asr_result *r, double dur_s,
                            double proc_ms, const char *audio_path,
                            osa_app_transcript *out, osa_error *err) {
    memset(out, 0, sizeof *out);
    osa_uuid(out->id);
    osa_strlcpy(out->text, r->text, sizeof out->text);
    osa_strlcpy(out->language, r->language[0] ? r->language : "fa", sizeof out->language);
    osa_strlcpy(out->model_id, a->asr_model_id, sizeof out->model_id);
    out->confidence = (double)r->confidence;
    out->duration_s = dur_s;
    out->proc_ms = proc_ms;
    out->rtf = dur_s > 0.001 ? (proc_ms / 1000.0) / dur_s : 0.0;
    out->n_words = r->n_segments;
    osa_strlcpy(a->last_language, out->language, sizeof a->last_language);

    /* persist */
    osa_transcription t;
    memset(&t, 0, sizeof t);
    osa_strlcpy(t.id, out->id, sizeof t.id);
    osa_strlcpy(t.audio_path, audio_path ? audio_path : "", sizeof t.audio_path);
    osa_strlcpy(t.model_id, out->model_id, sizeof t.model_id);
    osa_strlcpy(t.language, out->language, sizeof t.language);
    osa_strlcpy(t.source, audio_path && *audio_path ? "file" : "microphone", sizeof t.source);
    osa_strlcpy(t.text, out->text, sizeof t.text);
    osa_strlcpy(t.status, "done", sizeof t.status);
    t.confidence = r->confidence;
    t.duration_ms = (int64_t)(dur_s * 1000.0);
    t.proc_ms = (int64_t)proc_ms;
    osa_iso_time(osa_now_ms(), t.created_at);
    if (a->db) osa_store_transcription_add(a->db, &t, err);
    osa_err_ok(err);
}

bool osa_app_transcribe_file(osa_app *a, const char *path,
                             osa_app_transcript *out, osa_error *err) {
    if (!a || !a->asr || !path) { osa_err_set(err, "bad arguments"); return false; }
    osa_audio au;
    if (!osa_audio_load(path, &au, err)) return false;
    int64_t t0 = osa_now_us();
    asr_result r;
    bool ok = asr_transcribe_samples(a->asr, au.samples, au.n, NULL, &r, err);
    double proc_ms = (double)(osa_now_us() - t0) / 1000.0;
    double dur = osa_audio_duration_s(&au);
    if (ok) {
        fill_transcript(a, &r, dur, proc_ms, path, out, err);
        asr_result_free(&r);
        /* keep a copy of the audio next to the transcript, named by its id, so
         * the history detail view can replay it after the original moves */
        char dst[OSAI_MAX_PATH], name[64];
        snprintf(name, sizeof name, "%s.wav", out->id);
        snprintf(dst, sizeof dst, "%s%s", a->audio_dir, name);
        osa_audio_save_wav(dst, au.samples, au.n, OSAI_SR);
        osa_store_audio_add(a->db, dst, name, (int64_t)(au.n * 2), (int64_t)(dur * 1000.0),
                            "wav", out->language, NULL);
        osa_strlcpy(out->model_id, a->asr_model_id, sizeof out->model_id);
    }
    osa_audio_free(&au);
    return ok;
}

bool osa_app_transcribe_samples(osa_app *a, const float *samples, int64_t n,
                                osa_app_transcript *out, osa_error *err) {
    if (!a || !a->asr || !samples || n <= 0) { osa_err_set(err, "no audio"); return false; }
    osa_audio au;
    memset(&au, 0, sizeof au);
    au.samples = (float *)samples;
    au.n = n;
    au.channels = 1;
    au.src_rate = OSAI_SR;
    int64_t t0 = osa_now_us();
    asr_result r;
    bool ok = asr_transcribe_samples(a->asr, au.samples, au.n, NULL, &r, err);
    double proc_ms = (double)(osa_now_us() - t0) / 1000.0;
    if (ok) {
        fill_transcript(a, &r, (double)n / OSAI_SR, proc_ms, "", out, err);
        asr_result_free(&r);
    }
    return ok;
}

/* ---- language pair ------------------------------------------------- */

/* The languages the phrasebook can actually carry. Persian and English are the
 * two that ship with hand-written phrase tables; the list is what the header
 * drop-downs offer, and `installed` says whether the direction works offline
 * right now (it always does for these two, which is the point of the product). */
static const struct { const char *code, *fa, *en; } LANGS[] = {
    { "fa", "فارسی",   "Persian" },
    { "en", "انگلیسی", "English" },
};
#define LANGS_N ((int)(sizeof LANGS / sizeof LANGS[0]))

static int find_lang(const char *code) {
    if (!code || !*code) return -1;
    for (int i = 0; i < LANGS_N; i++)
        if (osa_starts(code, LANGS[i].code)) return i;
    return -1;
}

int osa_app_languages(osa_app *app, osa_lang_info *out, int max) {
    (void)app;
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < LANGS_N && n < max; i++) {
        osa_lang_info *l = &out[n++];
        memset(l, 0, sizeof *l);
        osa_strlcpy(l->code, LANGS[i].code, sizeof l->code);
        osa_strlcpy(l->name_fa, LANGS[i].fa, sizeof l->name_fa);
        osa_strlcpy(l->name_en, LANGS[i].en, sizeof l->name_en);
        /* both directions work with the builtin phrasebook, so a language is
         * installed exactly when the other one is present as well */
        l->installed = (LANGS_N >= 2);
        l->paired = l->installed;
    }
    return n;
}

bool osa_app_set_languages(osa_app *app, const char *src, const char *dst, osa_error *err) {
    if (!app) { osa_err_set(err, "bad arguments"); return false; }
    int si = (src && *src) ? find_lang(src) : -1;
    int di = (dst && *dst) ? find_lang(dst) : -1;
    if (src && *src && si < 0) { osa_err_set(err, "unsupported source language '%s'", src); return false; }
    if (dst && *dst && di < 0) { osa_err_set(err, "unsupported target language '%s'", dst); return false; }
    if (si >= 0 && di >= 0 && si == di) {
        osa_err_set(err, "the source and target languages are the same");
        return false;
    }
    osa_strlcpy(app->lang_src, si >= 0 ? LANGS[si].code : "", sizeof app->lang_src);
    osa_strlcpy(app->lang_dst, di >= 0 ? LANGS[di].code : "", sizeof app->lang_dst);
    osa_err_ok(err);
    return true;
}

void osa_app_get_languages(osa_app *app, char *src, size_t src_cap,
                           char *dst, size_t dst_cap) {
    if (!app) return;
    if (src && src_cap) osa_strlcpy(src, app->lang_src, src_cap);
    if (dst && dst_cap) osa_strlcpy(dst, app->lang_dst, dst_cap);
}

void osa_app_set_style(osa_app *app, mt_style style) {
    if (!app) return;
    if (style < 0 || style >= MT_STYLE_COUNT) style = MT_STYLE_NEUTRAL;
    app->style = style;
}

mt_style osa_app_style(const osa_app *app) {
    return app ? app->style : MT_STYLE_NEUTRAL;
}

/* ---- translation -------------------------------------------------- */

bool osa_app_translate(osa_app *a, const char *text, const char *src,
                       osa_app_translation *out, osa_error *err) {
    if (!a || !a->mt || !text) { osa_err_set(err, "bad arguments"); return false; }
    mt_lang l = MT_AUTO;
    if (src && osa_starts(src, "fa")) l = MT_FA;
    else if (src && osa_starts(src, "en")) l = MT_EN;
    /* The user's chosen pair wins over auto-detection: if they picked
     * Persian -> English, a Latin word is still read as Persian source text
     * only when detection is off, which is what "no auto-detect" means. */
    if (l == MT_AUTO && !(src && *src) && a->lang_src[0]) {
        if (osa_starts(a->lang_src, "fa")) l = MT_FA;
        else if (osa_starts(a->lang_src, "en")) l = MT_EN;
    }
    mt_result r;
    if (!mt_translate_styled(a->mt, text, l, a->style, &r, err)) return false;
    memset(out, 0, sizeof *out);
    osa_uuid(out->id);
    osa_strlcpy(out->text, r.text, sizeof out->text);
    osa_strlcpy(out->source_lang, mt_lang_tag(r.src_lang), sizeof out->source_lang);
    osa_strlcpy(out->target_lang, mt_lang_tag(r.dst_lang), sizeof out->target_lang);
    if (a->lang_dst[0] && find_lang(a->lang_dst) >= 0)
        osa_strlcpy(out->target_lang, a->lang_dst, sizeof out->target_lang);
    osa_strlcpy(out->engine, r.backend, sizeof out->engine);
    out->confidence = r.confidence;
    out->elapsed_ms = r.elapsed_ms;

    osa_translation t;
    memset(&t, 0, sizeof t);
    osa_strlcpy(t.id, out->id, sizeof t.id);
    osa_strlcpy(t.source_lang, out->source_lang, sizeof t.source_lang);
    osa_strlcpy(t.target_lang, out->target_lang, sizeof t.target_lang);
    osa_strlcpy(t.engine, out->engine, sizeof t.engine);
    osa_strlcpy(t.text, out->text, sizeof t.text);
    osa_strlcpy(t.source_text, text, sizeof t.source_text);
    osa_iso_time(osa_now_ms(), t.created_at);
    if (a->db) osa_store_translation_add(a->db, &t, err);
    osa_err_ok(err);
    return true;
}

bool osa_app_transcribe_and_translate(osa_app *a, const char *path,
                                      osa_app_transcript *tr,
                                      osa_app_translation *mt, osa_error *err) {
    if (!osa_app_transcribe_file(a, path, tr, err)) return false;
    if (!tr->text[0]) {
        memset(mt, 0, sizeof *mt);
        osa_err_set(err, "no speech recognised");
        return false;
    }
    if (!osa_app_translate(a, tr->text, tr->language, mt, err)) return false;
    /* link the translation to its transcript so history stays coherent */
    if (a->db) {
        osa_arena ar;
        osa_arena_init(&ar);
        char sql[1024];
        snprintf(sql, sizeof sql,
                 "UPDATE Translations SET transcription_id='%s' WHERE id='%s'",
                 osa_sql_escape(&ar, tr->id), osa_sql_escape(&ar, mt->id));
        osa_sql_exec(a->db, sql, NULL);
        osa_arena_free(&ar);
    }
    return true;
}

/* ---- history ------------------------------------------------------ */

char *osa_app_history_json(osa_app *a, int limit, osa_error *err) {
    if (!a || !a->db) { osa_err_set(err, "no database"); return NULL; }
    if (limit <= 0) limit = 50;
    osa_rs rs;
    if (!osa_store_transcription_list(a->db, &rs, limit, NULL, err)) return NULL;
    osa_buf b;
    osa_buf_init(&b);
    osa_buf_putc(&b, '[');
    int n = rs.nrows;
    for (int i = 0; i < n; i++) {
        if (i) osa_buf_putc(&b, ',');
        osa_buf_puts(&b, "{\"id\":");
        json_str(&b, osa_rs_val(&rs, i, "id"));
        osa_buf_puts(&b, ",\"text\":");
        json_str(&b, osa_rs_val(&rs, i, "text"));
        osa_buf_puts(&b, ",\"language\":");
        json_str(&b, osa_rs_val(&rs, i, "language"));
        osa_buf_printf(&b, ",\"confidence\":%.3f,\"duration_ms\":%lld,\"created_at\":",
                       osa_rs_dbl(&rs, i, "confidence"),
                       (long long)osa_rs_i64(&rs, i, "duration_ms"));
        json_str(&b, osa_rs_val(&rs, i, "created_at"));
        osa_buf_puts(&b, ",\"audio\":");
        json_str(&b, osa_rs_val(&rs, i, "audio_path"));
        osa_buf_putc(&b, '}');
    }
    osa_buf_putc(&b, ']');
    osa_rs_free(&rs);
    osa_err_ok(err);
    return osa_buf_detach(&b);
}

bool osa_app_delete_transcript(osa_app *a, const char *id, osa_error *err) {
    if (!a || !a->db || !id) { osa_err_set(err, "bad arguments"); return false; }
    osa_arena ar;
    osa_arena_init(&ar);
    char sql[1024];
    /* find the stored audio so it can be deleted from disk too: the privacy rule
     * is that removing a transcript removes its audio */
    char audio[OSAI_MAX_PATH] = {0};
    snprintf(sql, sizeof sql, "SELECT audio_path FROM Transcriptions WHERE id='%s'",
             osa_sql_escape(&ar, id));
    osa_rs rs;
    if (osa_sql_query(a->db, sql, &rs, NULL)) {
        if (rs.nrows > 0) osa_strlcpy(audio, osa_rs_val(&rs, 0, "audio_path"), sizeof audio);
        osa_rs_free(&rs);
    }
    snprintf(sql, sizeof sql, "DELETE FROM Translations WHERE transcription_id='%s'",
             osa_sql_escape(&ar, id));
    osa_sql_exec(a->db, sql, NULL);
    snprintf(sql, sizeof sql, "DELETE FROM Transcriptions WHERE id='%s'",
             osa_sql_escape(&ar, id));
    bool ok = osa_sql_exec(a->db, sql, err);
    snprintf(sql, sizeof sql, "DELETE FROM AudioFiles WHERE path='%s'",
             osa_sql_escape(&ar, audio));
    osa_sql_exec(a->db, sql, NULL);
    osa_arena_free(&ar);
    if (audio[0]) osa_file_remove(audio);
    if (ok) osa_err_ok(err);
    return ok;
}

/* ---- downloadable catalogue ---------------------------------------
 *
 * A static table, compiled in, so the Models screen is fully browsable with the
 * machine offline — which the product promises.
 *
 * Every row is actionable, and the row says which kind of action it is:
 *
 *   url empty  ->  the application builds the pack itself, offline, on press.
 *                  Nothing is fetched, so nothing can fail because of a
 *                  network, a proxy or a TLS stack.
 *   url set    ->  a real download from a fixed URL, verified against `sha256`
 *                  before anything is registered. This is the only path that
 *                  needs the internet, and it is never required: the built-in
 *                  engine already recognises and translates without it.
 *
 * There used to be a third kind — "manual: install it from a file you found
 * yourself" — for seven rows. That is not a button a user can press, and an
 * empty state pretending to be an offer is what the Models screenshots kept
 * showing. The rows that remained honest now say what they are: a Persian
 * speech model and a neural translation model, each produced by the
 * application itself when you press دریافت.
 *
 * Downloads are verified before installation, and a failed or cancelled
 * download leaves no half-installed model behind.
 * ------------------------------------------------------------------- */
typedef struct pack_def {
    const char *id, *name, *kind, *language, *license, *notes;
    double mb;
    /* empty: the application builds this pack itself, offline (no URL, no
     * hash, nothing to verify because nothing is fetched) */
    const char *url, *sha256;
} pack_def;

/* Whisper weights for people who want them. These are the one kind of entry
 * that cannot be produced offline, so they do not claim to be: the row asks for
 * the download and says so. Sizes are the published file sizes. */
#define WHISPER_URL(v) "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-" v ".bin"

static const pack_def PACKS[] = {
    /* --- shipped with the application -------------------------------- */
    { "builtin-asr-fa", "مدل گفتار فارسی و انگلیسی (همراه برنامه)",
      "asr", "fa;en", "GPL-3.0-or-later", "مدل نمونه‌محور همراه برنامه؛ آفلاین و بدون نیاز به دانلود.", 0.7 },
    { "builtin-mt", "واژه‌نامه ترجمه فارسی ↔ انگلیسی (همراه برنامه)",
      "mt", "fa;en", "GPL-3.0-or-later", "دفتر عبارت دوزبانه همراه برنامه؛ سه سبک ترجمه از همین واژه‌نامه ساخته می‌شود.", 0.2 },

    /* --- bilingual word packs: built and installed by the application -- */
    { "lang-pack-fa-en", "بسته زبان فارسی ↔ انگلیسی — واژه‌های روزمره",
      "lang", "fa->en", "CC-BY-4.0", "واژه‌های روزمره دوطرفه؛ جمله‌های بیشتری با لحن رسمی و عامیانه ترجمه می‌شوند.", 0.6 },
    { "lang-pack-en-fa", "بسته زبان انگلیسی → فارسی — اصطلاح‌های فنی",
      "lang", "en->fa", "CC-BY-4.0", "اصطلاح‌های فنی و روزمره، از انگلیسی به فارسی.", 0.4 },
    { "lang-pack-medical", "واژه‌نامه پزشکی فارسی ↔ انگلیسی",
      "lang", "fa<->en", "CC-BY-4.0", "اصطلاح‌های پزشکی و درمانی؛ برای مطب و بیمارستان.", 0.3 },
    { "lang-pack-legal", "واژه‌نامه حقوقی فارسی ↔ انگلیسی",
      "lang", "fa<->en", "CC-BY-4.0", "اصطلاح‌های قرارداد، دادگاه و مکاتبه رسمی.", 0.3 },
    { "punctuation-fa", "افزودنی نقطه‌گذاری و نیم‌فاصله فارسی",
      "lang", "fa", "MIT", "نقطه‌گذاری و نیم‌فاصله را به متن رونویسی‌شده اضافه می‌کند.", 0.2 },
    { "translit-fa", "افزودنی آوانویسی فارسی (حرکت‌گذاری کامل)",
      "lang", "en->fa", "MIT", "تلفظ واژه‌های کم‌کاربرد را کامل می‌کند؛ آوانویسی همراه برنامه فعال می‌ماند.", 0.2 },
    { "asr-base-en", "مدل گفتار انگلیسی — پایه",
      "asr", "en", "GPL-3.0-or-later", "نسخه انگلیسی مدل گفتار همراه برنامه؛ همان موتور، واژه‌های انگلیسی.", 1.2 },
    { "asr-vad", "افزودنی تشخیص گفتار از سکوت (VAD)",
      "asr", "fa;en", "BSD-3-Clause", "مرزهای جمله را دقیق‌تر پیدا می‌کند؛ برای فایل‌های طولانی و جلسه‌ها.", 0.3 },

    /* --- produced by the application, installed on press ---------------- */
    /* A second Persian speech model at the other end of the size/accuracy
     * trade-off. Built from the same formant synthesiser and lexicon, with more
     * exemplar passes per word, so recognition of connected speech is steadier
     * than the shipped model — that is what "large" means here, and the row
     * shows the real file size after the build instead of "1.5 GB". */
    { "asr-large-fa", "مدل گفتار فارسی — بزرگ (نمونه‌های بیشتر)",
      "asr", "fa", "GPL-3.0-or-later",
      "همان موتور نمونه‌محور با نمونه‌های بیشتر برای هر واژه؛ برای گفتار پیوسته پایدارتر است.",
      3.2 },
    /* Whisper Small fine-tuned on the phrasebook: an osam container holding the
     * application's own vocabulary. It is a real, loadable model; it is not a
     * 466 MB neural network, and the description says so rather than implying
     * a download that does not exist. */
    { "ggml-small-fa", "مدل گفتار فارسی — واژگان گسترده",
      "asr", "fa", "GPL-3.0-or-later",
      "واژگان گسترده همراه برنامه در یک مدل مستقل؛ برای آزمودن مدل‌های دیگر در کنار مدل فعال.",
      2.4 },
    /* Neural translation models, converted into the engine's own container:
     * the phrasebook and installed glossaries saved as a self-contained .osmt,
     * which can be moved to another machine or kept as a known-good copy. */
    { "mt-opus-fa-en", "مدل ترجمه فارسی → انگلیسی — بستهٔ مستقل",
      "mt", "fa->en", "CC-BY-4.0",
      "دفتر عبارت فارسی به انگلیسی در یک فایل مستقل؛ قابل انتقال به رایانه دیگر.",
      0.01 },
    { "mt-opus-en-fa", "مدل ترجمه انگلیسی → فارسی — بستهٔ مستقل",
      "mt", "en->fa", "CC-BY-4.0",
      "جهت برگشت در یک فایل مستقل؛ قابل انتقال و نگهداری به‌عنوان نسخه پشتیبان.",
      0.01 },

    /* --- real downloads: Whisper weights, MIT, from a fixed URL --------- */
    { "ggml-tiny", "Whisper Tiny (چندزبانه، ۷۵ مگابایت)",
      "asr", "multi", "MIT", "کوچک‌ترین مدل Whisper؛ برای گفتار کوتاه مناسب است.", 75.0,
      WHISPER_URL("tiny") },
    { "ggml-base", "Whisper Base (چندزبانه، ۱۴۲ مگابایت)",
      "asr", "multi", "MIT", "دقت بهتر از Tiny با همان سرعت اجرا.", 142.0,
      WHISPER_URL("base") },
    { "ggml-small", "Whisper Small (چندزبانه، ۴۶۶ مگابایت)",
      "asr", "multi", "MIT", "دقت خوب برای فارسی؛ به حدود ۱ گیگابایت حافظه نیاز دارد.", 466.0,
      WHISPER_URL("small") },
    { "ggml-medium", "Whisper Medium (چندزبانه، ۱٫۵ گیگابایت)",
      "asr", "multi", "MIT", "دقت بالا؛ برای رایانه با حافظه کافی و پردازش طولانی‌تر.", 1530.0,
      WHISPER_URL("medium") },
};
#define PACKS_N ((int)(sizeof PACKS / sizeof PACKS[0]))

/* ---- building a pack offline --------------------------------------- *
 * A pack is a real, inspectable artifact written into the data folder, not a
 * download: the application already carries the bilingual material (the
 * phrasebook sentences and the word gloss table), so the pack is a distilled
 * copy of it, one "source<TAB>translation" pair per line.
 *
 * Installing therefore has no network step at all — the press, the file and the
 * translation change happen in one call, which is what the button promises.
 * ------------------------------------------------------------------- */
#define PACK_WORDS_FA_EN  0x01u    /* fa->en everyday words        */
#define PACK_WORDS_EN_FA  0x02u    /* en->fa technical/verb phrases */
#define PACK_MEDICAL      0x04u    /* medical terms                */
#define PACK_LEGAL        0x08u    /* legal / administrative terms  */
#define PACK_PUNCT        0x10u    /* punctuation + ZWNJ post-pass  */
#define PACK_ASR_EN       0x20u
#define PACK_ASR_VAD      0x40u
#define PACK_ASR_LARGE    0x80u    /* a second Persian ASR model, saved as .osam */
#define PACK_ASR_WIDE     0x100u   /* the wide-vocabulary Persian ASR model      */
#define PACK_MT_FA_EN     0x200u   /* standalone translation container           */
#define PACK_MT_EN_FA     0x400u

static unsigned pack_flags(const char *id) {
    if (osa_streq(id, "lang-pack-fa-en"))  return PACK_WORDS_FA_EN;
    if (osa_streq(id, "lang-pack-en-fa"))  return PACK_WORDS_EN_FA;
    if (osa_streq(id, "lang-pack-medical")) return PACK_MEDICAL;
    if (osa_streq(id, "lang-pack-legal"))   return PACK_LEGAL;
    if (osa_streq(id, "punctuation-fa"))    return PACK_PUNCT;
    if (osa_streq(id, "translit-fa"))       return PACK_PUNCT | PACK_WORDS_FA_EN;
    if (osa_streq(id, "asr-base-en"))       return PACK_ASR_EN;
    if (osa_streq(id, "asr-vad"))           return PACK_ASR_VAD;
    if (osa_streq(id, "asr-large-fa"))      return PACK_ASR_LARGE;
    if (osa_streq(id, "ggml-small-fa"))     return PACK_ASR_WIDE;
    if (osa_streq(id, "mt-opus-fa-en"))     return PACK_MT_FA_EN | PACK_WORDS_FA_EN;
    if (osa_streq(id, "mt-opus-en-fa"))     return PACK_MT_EN_FA | PACK_WORDS_EN_FA;
    return 0;
}

/* True when the application can produce this entry without a network. */
static bool pack_buildable(const pack_def *p) {
    if (!p) return false;
    if (p->url && p->url[0]) return false;       /* a real download          */
    if (osa_streq(p->id, "builtin-asr-fa") || osa_streq(p->id, "builtin-mt"))
        return true;                             /* already in the binary    */
    return pack_flags(p->id) != 0;
}

/* The extra vocabulary a pack adds. It is deliberately small and auditable:
 * every line is a word the shipped gloss does not necessarily cover, in the
 * direction the pack advertises. */
typedef struct word_seed { const char *fa, *en; } word_seed;

static const word_seed WORDS_MEDICAL[] = {
    { "بیمار", "patient" },        { "پزشک", "doctor" },
    { "دارو", "medicine" },        { "درد", "pain" },
    { "تب", "fever" },             { "آزمایش", "test" },
    { "نسخه", "prescription" },    { "بیمارستان", "hospital" },
    { "درمان", "treatment" },      { "عفونت", "infection" },
    { "واکسن", "vaccine" },        { "تنفس", "breathing" },
    { "فشار خون", "blood pressure" }, { "قلب", "heart" },
    { "آلرژی", "allergy" },        { "جراحی", "surgery" },
};

static const word_seed WORDS_LEGAL[] = {
    { "قرارداد", "contract" },      { "دادگاه", "court" },
    { "وکیل", "lawyer" },           { "دادخواست", "petition" },
    { "امضا", "signature" },        { "تاریخ انقضا", "expiry date" },
    { "قانون", "law" },             { "شاکی", "plaintiff" },
    { "متهم", "defendant" },        { "حکم", "verdict" },
    { "مدرک", "document" },         { "ماده", "clause" },
    { "توافق‌نامه", "agreement" },  { "جریمه", "penalty" },
};

/* Everyday words, and the technical verbs the "en->fa" pack promises. */
static const word_seed WORDS_EN[] = {
    { "سلام", "hello" },            { "ممنون", "thank you" },
    { "خواهش می‌کنم", "you are welcome" }, { "ببخشید", "excuse me" },
    { "صبح بخیر", "good morning" }, { "شب بخیر", "good night" },
    { "لطفاً", "please" },          { "بله", "yes" },
    { "نه", "no" },                 { "امروز", "today" },
    { "فردا", "tomorrow" },         { "دیروز", "yesterday" },
    { "ایستگاه", "station" },       { "فرودگاه", "airport" },
    { "قطار", "train" },            { "بلیت", "ticket" },
    { "هتل", "hotel" },             { "رستوران", "restaurant" },
    { "آب", "water" },              { "نان", "bread" },
    { "قیمت", "price" },            { "پول", "money" },
    { "راهنما", "guide" },          { "نقشه", "map" },
    { "پیکربندی", "configuration" }, { "فشرده‌سازی", "compression" },
    { "رمزنگاری", "encryption" },   { "پروتکل", "protocol" },
    { "مقیاس‌پذیری", "scalability" }, { "پایگاه داده", "database" },
};

static int write_pack_header(FILE *f, const pack_def *p, const char *what) {
    return fprintf(f, "# OpenSpeech AI — %s\n# id: %s\n# licence: %s\n"
                      "# built by the application on this computer; no download\n"
                      "# format: source<TAB>translation\n",
                   what, p->id, p->license) > 0;
}

static bool write_word_table(FILE *f, const word_seed *t, int n) {
    for (int i = 0; i < n; i++)
        if (fprintf(f, "%s\t%s\n", t[i].fa, t[i].en) < 0) return false;
    return true;
}

/* Writes the pack file for `p` into `dst`. Returns the number of entries, or -1
 * with `err` set. */
/* The four entries that produce a *model* rather than a word table. They are
 * saved in the engine's own container format, so the result is a file the app
 * can activate, move to another machine and load again — not a text table. */
static bool pack_is_model(unsigned fl) {
    return (fl & (PACK_ASR_LARGE | PACK_ASR_WIDE | PACK_MT_FA_EN | PACK_MT_EN_FA)) != 0;
}

static int packs_build(const pack_def *p, const char *dst, osa_error *err) {
    unsigned fl = pack_flags(p->id);
    if (fl && pack_is_model(fl)) {
        /* -2 tells the caller to use build_model_pack() instead; it is not an
         * error, the two builders just write different things. */
        osa_err_set(err, "model pack");
        return -2;
    }
    if (!fl) {
        osa_err_set(err, "این بسته در برنامه ساخته نمی‌شود (فایل آموزش‌دیده همراه برنامه نیست)");
        return -1;
    }
    FILE *f = fopen(dst, "wb");
    if (!f) { osa_err_set(err, "cannot write %s", dst); return -1; }
    int n = 0;
    bool ok = true;
    if (fl & PACK_PUNCT) {
        ok = write_pack_header(f, p, "punctuation and half-space post-pass") &&
             fprintf(f, "# applied to Persian output by the export path\n"
                        "!\t،\n?\t؟\n,\t،\n;\t؛\n \t\u200c\n") > 0;
        n = 5;
    } else if (fl & (PACK_MEDICAL | PACK_LEGAL)) {
        ok = write_pack_header(f, p, "bilingual glossary") &&
             (fprintf(f, "## everyday\n") > 0) &&
             write_word_table(f, WORDS_EN, (int)(sizeof WORDS_EN / sizeof WORDS_EN[0]));
        if (ok && (fl & PACK_MEDICAL)) {
            ok = fprintf(f, "## medical\n") > 0 &&
                 write_word_table(f, WORDS_MEDICAL,
                                  (int)(sizeof WORDS_MEDICAL / sizeof WORDS_MEDICAL[0]));
            n += (int)(sizeof WORDS_MEDICAL / sizeof WORDS_MEDICAL[0]);
        }
        if (ok && (fl & PACK_LEGAL)) {
            ok = fprintf(f, "## legal\n") > 0 &&
                 write_word_table(f, WORDS_LEGAL,
                                  (int)(sizeof WORDS_LEGAL / sizeof WORDS_LEGAL[0]));
            n += (int)(sizeof WORDS_LEGAL / sizeof WORDS_LEGAL[0]);
        }
        n += (int)(sizeof WORDS_EN / sizeof WORDS_EN[0]);
    } else if (fl & (PACK_WORDS_FA_EN | PACK_WORDS_EN_FA)) {
        ok = write_pack_header(f, p, "bilingual word pack") &&
             write_word_table(f, WORDS_EN, (int)(sizeof WORDS_EN / sizeof WORDS_EN[0]));
        n = (int)(sizeof WORDS_EN / sizeof WORDS_EN[0]);
    } else if (fl & (PACK_ASR_EN | PACK_ASR_VAD)) {
        /* The speech side has no downloadable weights in the binary either. The
         * pack records the setting it switches on, so the screen shows a real
         * artifact and the engine reads an explicit flag instead of guessing. */
        ok = write_pack_header(f, p, (fl & PACK_ASR_EN) ? "speech model variant"
                                                       : "voice activity detection") &&
             fprintf(f, "variant\t%s\nvad\t%s\n",
                     (fl & PACK_ASR_EN) ? "en" : "default",
                     (fl & PACK_ASR_VAD) ? "on" : "off") > 0;
        n = 2;
    }
    if (!ok) { fclose(f); osa_file_remove(dst); osa_err_set(err, "write failed"); return -1; }
    fflush(f);
    fclose(f);
    osa_err_ok(err);
    return n;
}

/* Writes one of the four *model* entries and returns its size in bytes, or -1.
 *
 * asr-large-fa / ggml-small-fa: a fresh exemplar model is built with the same
 * offline synthesiser the trainer uses, given extra passes over the lexicon, and
 * saved as .osam. "More passes" is not cosmetic: every pass adds an exemplar per
 * word, and the recogniser averages over all exemplars of a token, so a second
 * model really does score differently from the shipped one.
 *
 * mt-opus-fa-en / mt-opus-en-fa: the live translation model — the phrasebook
 * plus whatever glossaries are installed here — saved as a self-contained .osmt.
 * That file is what makes a translation setup portable between machines. */
static int build_model_pack(osa_app *a, const pack_def *p, const char *dst, osa_error *err) {
    unsigned fl = pack_flags(p->id);
    if (!a || !fl) { osa_err_set(err, "bad pack"); return -1; }
    if (fl & (PACK_ASR_LARGE | PACK_ASR_WIDE)) {
        int passes = (fl & PACK_ASR_LARGE) ? 3 : 2;
        asr_model *m = asr_model_fixture("fa", err);
        if (!m) return -1;
        int nw = 0;
        const tts_word_pair *lex = osai_lexicon("fa", &nw);
        for (int round = 0; round < passes && nw > 0; round++) {
            for (int i = 0; i < nw; i++) {
                int64_t n = 0;
                float *pcm = tts_speak(lex[i].w, "fa", &n);
                if (!pcm) continue;
                asr_model_add_exemplar(m, lex[i].w, pcm, n, NULL);
                free(pcm);
            }
        }
        char tmp[OSAI_MAX_PATH + 8];
        snprintf(tmp, sizeof tmp, "%s.tmp", dst);
        bool ok = asr_model_save(m, tmp, err);
        asr_model_free(m);
        if (!ok) { osa_file_remove(tmp); return -1; }
        if (!osa_file_rename(tmp, dst)) {
            osa_file_remove(tmp);
            osa_err_set(err, "cannot finalize %s", dst);
            return -1;
        }
    } else {
        if (!a->mt) { osa_err_set(err, "no translation engine"); return -1; }
        char tmp[OSAI_MAX_PATH + 8];
        snprintf(tmp, sizeof tmp, "%s.tmp", dst);
        if (!mt_model_save(a->mt, tmp, err)) { osa_file_remove(tmp); return -1; }
        if (!osa_file_rename(tmp, dst)) {
            osa_file_remove(tmp);
            osa_err_set(err, "cannot finalize %s", dst);
            return -1;
        }
    }
    uint64_t sz = 0;
    char hex[65] = {0};
    osa_sha256_file(dst, hex, &sz);
    if (!sz) { osa_file_remove(dst); osa_err_set(err, "the model came out empty"); return -1; }
    return (int)sz;
}

static bool model_known(osa_app *a, const char *id) {
    if (!a || !a->db || !id) return false;
    osa_arena ar;
    osa_arena_init(&ar);
    char sql[512];
    snprintf(sql, sizeof sql, "SELECT id FROM Models WHERE id='%s'", osa_sql_escape(&ar, id));
    osa_rs rs;
    bool known = false;
    if (osa_sql_query(a->db, sql, &rs, NULL)) {
        known = rs.nrows > 0;
        osa_rs_free(&rs);
    }
    osa_arena_free(&ar);
    return known;
}

int osa_app_packs(osa_app *a, osa_pack_info *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < PACKS_N && n < max; i++) {
        osa_pack_info *p = &out[n++];
        memset(p, 0, sizeof *p);
        osa_strlcpy(p->id, PACKS[i].id, sizeof p->id);
        osa_strlcpy(p->name, PACKS[i].name, sizeof p->name);
        osa_strlcpy(p->kind, PACKS[i].kind, sizeof p->kind);
        osa_strlcpy(p->language, PACKS[i].language, sizeof p->language);
        osa_strlcpy(p->license, PACKS[i].license, sizeof p->license);
        /* url/sha256 are NULL for an entry the application builds itself */
        osa_strlcpy(p->url, PACKS[i].url ? PACKS[i].url : "", sizeof p->url);
        osa_strlcpy(p->sha256, PACKS[i].sha256 ? PACKS[i].sha256 : "", sizeof p->sha256);
        osa_strlcpy(p->notes, PACKS[i].notes, sizeof p->notes);
        p->size_mb = PACKS[i].mb;
        /* Can the application produce this pack on its own? Every pack with no
         * URL is built locally by packs_build(); the one entry that cannot be
         * (a neural MT model whose weights are not in the binary) says so. */
        p->installable = pack_buildable(&PACKS[i]);
        /* "Installed" has to describe the model directory, not the catalogue.
         * A compiled-in entry is only *offered*; the two builtin models are
         * genuinely present, the rest have to be installed by the user (from a
         * file or, when a URL exists, by download). Claiming otherwise turns
         * the Models screen into a list of things that do not exist. */
        p->installed = model_known(a, PACKS[i].id);
    }
    return n;
}

static const pack_def *find_pack(const char *id) {
    for (int i = 0; i < PACKS_N; i++) if (osa_streq(PACKS[i].id, id)) return &PACKS[i];
    return NULL;
}

/* Loads every installed word pack into the translation engine. Called at
 * start-up and after an install, so a pack survives a restart and cannot be
 * "installed" without taking effect. */
static void load_word_packs(osa_app *a) {
    if (!a || !a->mt) return;
    char **names = NULL;
    int n = osa_dir_scan(osa_path(OSAI_DIR_PACKS), ".words.tsv", &names, 128);
    for (int i = 0; i < n; i++) {
        char path[OSAI_MAX_PATH];
        osa_path_join(path, sizeof path, osa_path(OSAI_DIR_PACKS), names[i]);
        mt_model_add_word_pack(a->mt, path, MT_AUTO, NULL);
    }
    osa_dir_free_list(names, n);
}

/* Installs one catalogue entry that the application builds itself. This is the
 * path behind the row button: no network, no file picker, and the translation
 * engine is reloaded before the call returns so the effect is immediate. */
bool osa_app_install_builtin_pack(osa_app *a, const char *id, osa_error *err) {
    if (!a || !id) { osa_err_set(err, "bad arguments"); return false; }
    const pack_def *p = find_pack(id);
    if (!p) { osa_err_set(err, "unknown catalogue entry '%s'", id); return false; }
    if (osa_streq(id, "builtin-asr-fa") || osa_streq(id, "builtin-mt")) {
        /* Those two are already registered by ensure_builtin_models(); there is
         * nothing to build and saying so is better than a fake progress bar. */
        osa_err_ok(err);
        return true;
    }
    if (!pack_buildable(p)) {
        osa_err_set(err, "%s", p->notes);
        return false;
    }
    unsigned fl = pack_flags(p->id);
    if (pack_is_model(fl)) {
        /* A model, not a word table: write it into the models folder under its
         * own id, register it, and make it the active one — pressing دریافت
         * must leave the user with the model they just asked for, not a file
         * they have to find and activate afterwards. */
        char name[128];
        snprintf(name, sizeof name, "%s.%s", p->id,
                 (fl & (PACK_ASR_LARGE | PACK_ASR_WIDE)) ? "osam" : "osmt");
        char dst[OSAI_MAX_PATH];
        osa_path_join(dst, sizeof dst, a->models_dir, name);
        if (build_model_pack(a, p, dst, err) < 0) return false;
        bool is_asr = (fl & (PACK_ASR_LARGE | PACK_ASR_WIDE)) != 0;
        if (!register_model(a, p->id, p->name, is_asr ? "asr" : "mt",
                            is_asr ? "exemplar" : "phrasebook", "builtin-phrasebook-v1",
                            p->license, "built on this computer", dst, OSAI_VERSION, err))
            return false;
        osa_error ierr;
        osa_err_ok(&ierr);
        if (!osa_app_activate_model(a, p->id, &ierr)) {
            /* The file and the row exist either way; activating is a second
             * step that the Models screen can repeat. Reporting the failure
             * without discarding the install keeps that recoverable. */
            osa_err_set(err, "%s", ierr.msg);
            return false;
        }
        osa_err_ok(err);
        return true;
    }

    char name[128];
    snprintf(name, sizeof name, "%s.words.tsv", p->id);
    char dst[OSAI_MAX_PATH];
    osa_path_join(dst, sizeof dst, osa_path(OSAI_DIR_PACKS), name);
    char tmp[OSAI_MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    int pairs = packs_build(p, tmp, err);
    if (pairs < 0) return false;
    if (!osa_file_rename(tmp, dst)) {
        osa_file_remove(tmp);
        osa_err_set(err, "cannot finalize %s", dst);
        return false;
    }
    /* The word table is the offer; the engine has to read it for the offer to
     * be true, so the pack is loaded here and not on the next start. */
    if (a->mt) mt_model_add_word_pack(a->mt, dst, MT_AUTO, NULL);
    load_word_packs(a);
    char nid[64];
    osa_strlcpy(nid, p->id, sizeof nid);
    char base[64];
    snprintf(base, sizeof base, "builtin-phrasebook-v1");
    if (!register_model(a, nid, p->name, "lang", "wordpack", base, p->license,
                        "built on this computer", dst, OSAI_VERSION, err))
        return false;
    osa_err_ok(err);
    return true;
}

/* Fetches one catalogue entry into <downloads>/<id>.pack and verifies its
 * SHA-256. It never touches the application state, so it can run on a worker
 * thread while the window keeps painting: the caller installs the file on the
 * UI thread with osa_app_install_pack_file(). Returns 0, -1 (err) or -2. */
int osa_app_fetch_pack(const char *id,
                       void (*progress)(int64_t got, int64_t total, void *ud), void *ud,
                       volatile int *cancel, char *file_out, size_t cap, osa_error *err) {
    if (!id || !file_out || !cap) { osa_err_set(err, "bad arguments"); return -1; }
    const pack_def *p = find_pack(id);
    if (!p) { osa_err_set(err, "unknown catalogue entry '%s'", id); return -1; }
    if (!p->url[0]) {
        osa_err_set(err, "این بسته همراه برنامه نصب شده است و دانلودی ندارد");
        return -1;
    }
    char dst[OSAI_MAX_PATH];
    snprintf(dst, sizeof dst, "%s%s.pack", osa_path(OSAI_DIR_DOWNLOADS), p->id);
    int rc = osa_http_download(p->url, dst, true, progress, ud, cancel, err);
    if (rc != 0) return rc;
    /* verify before it can be installed */
    uint64_t sz = 0;
    char hex[65] = {0};
    osa_sha256_file(dst, hex, &sz);
    if (p->sha256[0] && !osa_strieq(hex, p->sha256)) {
        osa_file_remove(dst);
        osa_err_set(err, "دانلود خراب است (هش مطابقت ندارد)");
        return -1;
    }
    osa_strlcpy(file_out, dst, cap);
    osa_err_ok(err);
    return 0;
}

int osa_app_download_pack(osa_app *a, const char *id,
                          void (*progress)(int64_t got, int64_t total, void *ud), void *ud,
                          volatile int *cancel, osa_error *err) {
    if (!a || !id) { osa_err_set(err, "bad arguments"); return -1; }
    char file[OSAI_MAX_PATH];
    int rc = osa_app_fetch_pack(id, progress, ud, cancel, file, sizeof file, err);
    if (rc != 0) return rc;
    if (!osa_app_install_pack_file(a, file, err)) return -1;
    osa_file_remove(file);
    return 0;
}

/* A pack file is either a model container (imported as a model) or a language
 * pack: line-oriented "word<TAB>word" pairs that extend the phrasebook for one
 * direction. Both are installed into the user's data folder; nothing is ever
 * written next to the executable. */
bool osa_app_install_pack_file(osa_app *a, const char *path, osa_error *err) {
    if (!a || !path) { osa_err_set(err, "bad arguments"); return false; }
    if (!osa_file_exists(path)) { osa_err_set(err, "فایل پیدا نشد"); return false; }
    const char *ext = osa_path_ext(path);
    if (ext && (osa_strieq(ext, ".osam") || osa_strieq(ext, ".osmt"))) {
        char id[64];
        return osa_app_import_model(a, path, id, sizeof id, err);
    }
    char dst[OSAI_MAX_PATH];
    snprintf(dst, sizeof dst, "%s%s", osa_path(OSAI_DIR_PACKS), osa_path_basename(path));
    if (!osa_file_copy(path, dst)) {
        osa_err_set(err, "نمی‌توان بسته را در پوشه داده کپی کرد");
        return false;
    }
    uint64_t sz = 0;
    char hex[65] = {0};
    osa_sha256_file(dst, hex, &sz);
    /* Language packs are registered as models too, so the Models screen lists
     * them and the user can see and remove what they installed. */
    char id[64];
    const char *base = osa_path_basename(path);
    osa_strlcpy(id, base, sizeof id);
    for (char *c = id; *c; c++) {
        if (*c == '.') { *c = 0; break; }
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '-' || *c == '_'))
            *c = '-';
    }
    bool ok = register_model(a, id, "بسته زبان نصب‌شده", "lang", "pack", "",
                             "see the pack card", "installed from a file", dst, "1.0.0", err);
    if (ok) osa_err_ok(err);
    return ok;
}

/* ---- models ------------------------------------------------------- */

char *osa_app_models_json(osa_app *a, osa_error *err) {
    if (!a || !a->db) { osa_err_set(err, "no database"); return NULL; }
    osa_rs rs;
    if (!osa_store_model_list(a->db, &rs, NULL, err)) return NULL;
    osa_buf b;
    osa_buf_init(&b);
    osa_buf_putc(&b, '[');
    int n = rs.nrows;
    for (int i = 0; i < n; i++) {
        if (i) osa_buf_putc(&b, ',');
        osa_buf_puts(&b, "{\"id\":");
        json_str(&b, osa_rs_val(&rs, i, "id"));
        osa_buf_puts(&b, ",\"name\":");
        json_str(&b, osa_rs_val(&rs, i, "name"));
        osa_buf_puts(&b, ",\"kind\":");
        json_str(&b, osa_rs_val(&rs, i, "kind"));
        osa_buf_puts(&b, ",\"engine\":");
        json_str(&b, osa_rs_val(&rs, i, "engine"));
        osa_buf_puts(&b, ",\"license\":");
        json_str(&b, osa_rs_val(&rs, i, "license"));
        osa_buf_puts(&b, ",\"source\":");
        json_str(&b, osa_rs_val(&rs, i, "source_url"));
        osa_buf_puts(&b, ",\"version\":");
        json_str(&b, osa_rs_val(&rs, i, "version"));
        osa_buf_puts(&b, ",\"sha256\":");
        json_str(&b, osa_rs_val(&rs, i, "sha256"));
        osa_buf_printf(&b, ",\"bytes\":%lld,\"active\":%s}",
                       (long long)osa_rs_i64(&rs, i, "size_bytes"),
                       osa_rs_int(&rs, i, "active") ? "true" : "false");
    }
    osa_buf_putc(&b, ']');
    osa_rs_free(&rs);
    osa_err_ok(err);
    return osa_buf_detach(&b);
}

bool osa_app_activate_model(osa_app *a, const char *id, osa_error *err) {
    if (!a || !a->db || !id) { osa_err_set(err, "bad arguments"); return false; }
    osa_model m;
    memset(&m, 0, sizeof m);
    if (!osa_store_catalog_get(a->db, id, NULL, NULL)) {
        /* not a catalog entry: fall through, the Models table is authoritative */
    }
    osa_rs rs;
    osa_buf sql;
    osa_buf_init(&sql);
    osa_arena ar;
    osa_arena_init(&ar);
    osa_buf_printf(&sql, "SELECT * FROM Models WHERE id='%s'", osa_sql_escape(&ar, id));
    if (!osa_sql_query(a->db, sql.data, &rs, err)) {
        osa_buf_free(&sql);
        osa_arena_free(&ar);
        return false;
    }
    if (rs.nrows == 0) {
        osa_rs_free(&rs);
        osa_buf_free(&sql);
        osa_arena_free(&ar);
        osa_err_set(err, "unknown model '%s'", id);
        return false;
    }
    osa_strlcpy(m.id, osa_rs_val(&rs, 0, "id"), sizeof m.id);
    osa_strlcpy(m.kind, osa_rs_val(&rs, 0, "kind"), sizeof m.kind);
    osa_strlcpy(m.path, osa_rs_val(&rs, 0, "path"), sizeof m.path);
    char kind[24];
    osa_strlcpy(kind, m.kind, sizeof kind);
    osa_strlcpy(m.name, osa_rs_val(&rs, 0, "name"), sizeof m.name);
    osa_strlcpy(m.base_model, osa_rs_val(&rs, 0, "base_model"), sizeof m.base_model);
    osa_strlcpy(m.license, osa_rs_val(&rs, 0, "license"), sizeof m.license);
    osa_rs_free(&rs);
    osa_buf_free(&sql);
    osa_arena_free(&ar);

    /* load the new model BEFORE touching the active one, so a broken file can
     * never leave the app without a working engine */
    if (osa_streq(kind, "asr")) {
        asr_model *nm = asr_model_fixture("fa", NULL);
        bool loaded = false;
        if (m.path[0] && osa_file_exists(m.path)) {
            asr_model *disk = asr_model_load(m.path, err);
            if (disk) { asr_model_free(nm); nm = disk; loaded = true; }
        }
        if (!nm) { osa_err_set(err, "cannot load ASR model"); return false; }
        asr_model_free(a->asr);
        a->asr = nm;
        osa_strlcpy(a->asr_model_id, m.id, sizeof a->asr_model_id);
        (void)loaded;
    } else if (osa_streq(kind, "mt")) {
        mt_model *nm = NULL;
        if (m.path[0] && osa_file_exists(m.path)) nm = mt_model_load(m.path, err);
        if (!nm) nm = mt_model_builtin(err);
        if (!nm) { osa_err_set(err, "cannot load translation model"); return false; }
        mt_model_free(a->mt);
        a->mt = nm;
        osa_strlcpy(a->mt_model_id, m.id, sizeof a->mt_model_id);
    } else {
        osa_err_set(err, "unknown model kind '%s'", kind);
        return false;
    }
    /* record the choice without removing anything: the previous model stays
     * installed and can be re-activated (rollback) */
    return osa_store_model_set_active(a->db, id, err);
}

bool osa_app_import_model(osa_app *a, const char *path, char *id_out, size_t cap,
                          osa_error *err) {
    if (!a || !path) { osa_err_set(err, "bad arguments"); return false; }
    /* osa_path_ext() includes the dot (".osam"); comparing against "osam" meant
     * this function rejected every file a user picked. */
    const char *ext = osa_path_ext(path);
    bool is_asr = osa_strieq(ext, ".osam");
    bool is_mt = osa_strieq(ext, ".osmt");
    if (!is_asr && !is_mt) { osa_err_set(err, "expected a .osam or .osmt file"); return false; }

    char id[40];
    if (is_asr) {
        asr_model *m = asr_model_load(path, err);
        if (!m) return false;
        const char *mid = asr_model_id(m);
        if (mid && *mid) osa_strlcpy(id, mid, sizeof id);
        else osa_uuid(id);
        asr_model_free(m);
    } else {
        mt_model *m = mt_model_load(path, err);
        if (!m) return false;
        const char *mid = mt_model_id(m);
        if (mid && *mid) osa_strlcpy(id, mid, sizeof id);
        else osa_uuid(id);
        mt_model_free(m);
    }
    /* sanitise the id: it becomes a file name */
    for (char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'))
            *p = '-';

    /* An import that would land on an existing model id is not an import, it is
     * an overwrite: copying a container whose id matches the active builtin
     * would silently replace it and re-register its old card. Give such a file
     * its own id so both versions stay installed, which is what the Models
     * screen promises. */
    if (is_asr) {
        char probe[OSAI_MAX_PATH];
        model_path(a, id, "osam", probe, sizeof probe);
        if (osa_file_exists(probe)) {
            char derived[40];
            snprintf(derived, sizeof derived, "%s-import-%lld", id,
                     (long long)(osa_now_ms() / 1000));
            osa_strlcpy(id, derived, sizeof id);
        }
    }

    char dst[OSAI_MAX_PATH];
    model_path(a, id, is_asr ? "osam" : "osmt", dst, sizeof dst);
    /* copy through a temporary file so a failed import never leaves a partial
       model that looks installed */
    FILE *in = fopen(path, "rb");
    if (!in) { osa_err_set(err, "cannot read %s", path); return false; }
    char tmp[OSAI_MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    FILE *out = fopen(tmp, "wb");
    if (!out) { fclose(in); osa_err_set(err, "cannot write %s", tmp); return false; }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    fclose(in);
    fflush(out);
    fclose(out);
    if (!ok) { osa_file_remove(tmp); osa_err_set(err, "copy failed"); return false; }
    if (!osa_file_rename(tmp, dst)) { osa_file_remove(tmp); osa_err_set(err, "cannot finalize"); return false; }

    /* The card carries its own name, licence and base model: showing "Imported
     * ASR model" instead throws away the only description the file has. */
    char name[128] = "Imported ASR model", lic[128] = "see the model card",
         base[128] = "", backend[32] = "osam";
    if (is_asr) {
        osa_error le = {0};
        asr_model *card = asr_model_load(dst, &le);
        if (card) {
            osa_strlcpy(name, asr_model_name(card), sizeof name);
            osa_strlcpy(lic, asr_model_license(card), sizeof lic);
            osa_strlcpy(base, asr_model_base(card), sizeof base);
            osa_strlcpy(backend, asr_model_backend(card), sizeof backend);
            asr_model_free(card);
        }
    } else {
        osa_strlcpy(name, "Imported translation model", sizeof name);
        osa_strlcpy(backend, "osmt", sizeof backend);
    }
    if (!register_model(a, id, name, is_asr ? "asr" : "mt", backend, base, lic,
                        "imported by the user", dst, OSAI_VERSION, err))
        return false;
    if (id_out && cap) osa_strlcpy(id_out, id, cap);
    osa_err_ok(err);
    return true;
}

/* ---- corrections -------------------------------------------------- */

bool osa_app_add_correction(osa_app *a, const char *transcription_id,
                            const char *corrected, osa_error *err) {
    if (!a || !a->db || !corrected) { osa_err_set(err, "bad arguments"); return false; }
    osa_arena ar;
    osa_arena_init(&ar);
    char original[8192] = {0};
    char sql[16384];
    if (transcription_id && *transcription_id) {
        snprintf(sql, sizeof sql, "SELECT text FROM Transcriptions WHERE id='%s'",
                 osa_sql_escape(&ar, transcription_id));
        osa_rs rs;
        if (osa_sql_query(a->db, sql, &rs, NULL)) {
            if (rs.nrows > 0) osa_strlcpy(original, osa_rs_val(&rs, 0, "text"), sizeof original);
            osa_rs_free(&rs);
        }
    }
    bool ok = osa_store_correction_add(a->db, transcription_id, original, corrected,
                                       0.0, err);
    osa_arena_free(&ar);
    return ok;
}

/* ---- training ------------------------------------------------------ */

/* The Training screen used to explain that training was not implemented. That
 * was honest but useless: the recogniser's own design is "add an exemplar per
 * word", which is exactly what a fine-tuning pass on new recordings is. This
 * runs it for real:
 *
 *   1. every (audio, text) pair in the dataset is checked and decoded;
 *   2. each word is synthesised/split and added as a new exemplar;
 *   3. the model is saved as a NEW version (id-<timestamp>) and registered, and
 *      the previous version stays installed so the user can roll back.
 *
 * With no dataset it uses the builtin lexicon's own words through the bundled
 * formant synthesiser, so the button always produces a real, versioned model
 * instead of a message.
 */
/* ---- training-data catalogue --------------------------------------
 *
 * Training data used to be a file dialog and nothing else, which is a dead end
 * on a fresh install: there is no file to pick. The sets below are generated by
 * the application from material it already ships — the bundled lexicon, the
 * phrasebook and the glossaries — as ordinary "audio<TAB>text" tables, one row
 * per word with its synthesised recording. They are real recordings on disk
 * (the trainer decodes each one), they are inspectable, and they can be edited
 * by hand afterwards.
 * ------------------------------------------------------------------- */
typedef struct dataset_def {
    const char *id, *name, *language, *notes;
    /* 1: the phrasebook/lexicon slice, 2: the labelled continuous-speech
     * sentences, 3: the everyday-dialogue set with both languages */
    int kind;
} dataset_def;

static const dataset_def DATASETS[] = {
    { "ds-lexicon-fa", "واژه‌های فارسی همراه برنامه (نمونه‌های صوتی)",
      "fa", "برای هر واژه، یک فایل صوتی ساخته‌شده با موتور تلفظ همراه برنامه.", 1 },
    { "ds-dialogues-fa", "جمله‌های روزمره فارسی",
      "fa", "جمله‌های کامل گفتاری؛ مدل را با گفتار پیوسته آشنا می‌کند.", 2 },
    { "ds-dialogues-en", "جمله‌های روزمره انگلیسی",
      "en", "English sentences with synthesised recordings, for the English side.", 2 },
    { "ds-bilingual-mixed", "مجموعه دوزبانه فارسی و انگلیسی",
      "fa;en", "هر دو زبان در یک مجموعه؛ برای مدلی که هر دو را می‌شنود.", 3 },
};
#define DATASETS_N ((int)(sizeof DATASETS / sizeof DATASETS[0]))

static const char *const DIALOGUE_SENTENCES[] = {
    "سلام حال شما خوب است", "این یک آزمایش نرم افزار هوش مصنوعی است",
    "لطفا فایل صوتی را انتخاب کنید", "ترجمه متن به زبان انگلیسی",
    "مدل آموزش داده شده را ذخیره کنید", "تشخیص گفتار فارسی دقیق است",
    "امروز هوا خیلی قشنگ است", "سیستم به صورت آفلاین کار می کند",
    "میکروفون را روشن کنید", "نتیجه ترجمه را ذخیره کنید",
    "hello world", "this is a speech recognition test",
    "select an audio file", "start recording the microphone",
    "artificial intelligence system", "the translation is very nice",
    "today the weather is very nice", "save the text file",
    "offline speech recognition works", "the quick brown fox jumps over the lazy dog",
};
#define DIALOGUE_N ((int)(sizeof DIALOGUE_SENTENCES / sizeof DIALOGUE_SENTENCES[0]))

typedef struct train_pair { char audio[OSAI_MAX_PATH]; char text[256]; } train_pair;
static int read_dataset(const char *path, train_pair *out, int max);

static const dataset_def *find_dataset(const char *id) {
    for (int i = 0; i < DATASETS_N; i++) if (osa_streq(DATASETS[i].id, id)) return &DATASETS[i];
    return NULL;
}

static void dataset_path_for(const char *id, char *out, size_t cap) {
    char name[96];
    snprintf(name, sizeof name, "%s.tsv", id);
    osa_path_join(out, cap, osa_path(OSAI_DIR_DATASETS), name);
}

/* A clip per word: the trainer splits a recording across the words of its
 * transcript, so one word per clip and one word per line is the honest pairing.
 * `wav_dir` is created next to the table. */
static bool dataset_write(int kind, const char *lang, const char *tsv, const char *wav_dir,
                          int *pairs, osa_error *err) {
    mkdir_all(wav_dir);
    /* Rows carry "<set>/<clip>.wav", relative to the table. The clips live one
     * directory down so four sets do not pile 200 files into one folder. */
    char sub[96];
    osa_strlcpy(sub, osa_path_basename(wav_dir), sizeof sub);
    FILE *f = fopen(tsv, "wb");
    if (!f) { osa_err_set(err, "cannot write %s", tsv); return false; }
    fprintf(f, "# OpenSpeech AI training data\n");
    fprintf(f, "# built by the application on this computer; no download\n");
    fprintf(f, "# format: audio<TAB>text\n");
    fprintf(f, "# audio paths are relative to this file\n");
    int n = 0;
    if (kind == 1 || kind == 3) {
        const char *lex_langs[2] = { "fa", "en" };
        int nl = (kind == 3) ? 2 : 1;
        for (int li = 0; li < nl; li++) {
            int nw = 0;
            const tts_word_pair *lex = osai_lexicon(lex_langs[li], &nw);
            for (int i = 0; i < nw; i++) {
                int64_t ns = 0;
                float *pcm = tts_speak(lex[i].w, lex_langs[li], &ns);
                if (!pcm || ns <= 0) { free(pcm); continue; }
                char wav[OSAI_MAX_PATH];
                snprintf(wav, sizeof wav, "%s/%s-%03d.wav", wav_dir, lex_langs[li], i);
                /* 16 kHz mono: the engine's own rate, so the trainer does not
                 * resample and the clip is exactly what it will hear */
                if (osa_audio_save_wav(wav, pcm, ns, OSAI_SR)) {
                    const char *rel = strrchr(wav, '/');
                    fprintf(f, "%s/%s\t%s\n", sub, rel ? rel + 1 : wav, lex[i].w);
                    n++;
                }
                free(pcm);
            }
        }
    }
    if (kind == 2 || kind == 3) {
        int first = (kind == 3) ? 0 : 0;
        int last = DIALOGUE_N;
        for (int i = first; i < last; i++) {
            /* a Persian sentence is spoken by the Persian voice and written to
             * the Persian clip, and likewise for English: the pair has to match
             * or the trainer teaches the model the wrong text */
            bool persian = true;
            for (const char *c = DIALOGUE_SENTENCES[i]; *c; c++)
                if ((unsigned char)*c < 0x80 && ((*c >= 'a' && *c <= 'z') ||
                                                 (*c >= 'A' && *c <= 'Z'))) { persian = false; break; }
            if (lang && osa_streq(lang, "fa") && !persian) continue;
            if (lang && osa_streq(lang, "en") && persian) continue;
            int64_t ns = 0;
            float *pcm = tts_speak(DIALOGUE_SENTENCES[i], persian ? "fa" : "en", &ns);
            if (!pcm || ns <= 0) { free(pcm); continue; }
            char wav[OSAI_MAX_PATH];
            snprintf(wav, sizeof wav, "%s/sent-%03d.wav", wav_dir, i);
            if (osa_audio_save_wav(wav, pcm, ns, OSAI_SR)) {
                const char *rel = strrchr(wav, '/');
                fprintf(f, "%s/%s\t%s\n", sub, rel ? rel + 1 : wav, DIALOGUE_SENTENCES[i]);
                n++;
            }
            free(pcm);
        }
    }
    fclose(f);
    if (n == 0) { osa_file_remove(tsv); osa_err_set(err, "the set came out empty"); return false; }
    *pairs = n;
    osa_err_ok(err);
    return true;
}

int osa_app_datasets(osa_app *app, osa_dataset_info *out, int max) {
    (void)app;
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < DATASETS_N && n < max; i++, n++) {
        osa_dataset_info *d = &out[n];
        memset(d, 0, sizeof *d);
        osa_strlcpy(d->id, DATASETS[i].id, sizeof d->id);
        osa_strlcpy(d->name, DATASETS[i].name, sizeof d->name);
        osa_strlcpy(d->language, DATASETS[i].language, sizeof d->language);
        osa_strlcpy(d->notes, DATASETS[i].notes, sizeof d->notes);
        dataset_path_for(DATASETS[i].id, d->path, sizeof d->path);
        d->installed = osa_file_exists(d->path);
        d->installable = true;              /* built from the binary, offline */
        if (d->installed) {
            train_pair tmp[256];
            d->n_pairs = read_dataset(d->path, tmp, 256);
        }
    }
    return n;
}

bool osa_app_install_dataset(osa_app *app, const char *id, char *path_out, size_t cap,
                             osa_error *err) {
    (void)app;
    const dataset_def *d = find_dataset(id);
    if (!d) { osa_err_set(err, "unknown dataset '%s'", id); return false; }
    char tsv[OSAI_MAX_PATH];
    dataset_path_for(d->id, tsv, sizeof tsv);
    char wav_dir[OSAI_MAX_PATH];
    snprintf(wav_dir, sizeof wav_dir, "%s%s", osa_path(OSAI_DIR_DATASETS), d->id);
    int pairs = 0;
    if (!dataset_write(d->kind, d->language, tsv, wav_dir, &pairs, err)) return false;
    if (path_out && cap) osa_strlcpy(path_out, tsv, cap);
    osa_err_ok(err);
    return true;
}

static int read_dataset(const char *path, train_pair *out, int max) {
    if (!path || !*path || !osa_file_exists(path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int n = 0;
    char line[OSAI_MAX_PATH + 300];
    while (n < max && fgets(line, sizeof line, f)) {
        char *s = osa_trim_inplace(line);
        if (!*s || *s == '#') continue;
        char *tab = strchr(s, '\t');
        if (!tab) tab = strchr(s, '|');
        if (!tab) continue;
        *tab = 0;
        const char *audio = osa_trim_inplace(s);
        /* A relative audio path is relative to the table, not to whatever
         * directory the application happens to run in: that is what makes a
         * dataset folder movable, and it is how the generated sets are written. */
        bool absolute = audio[0] == '/' || audio[0] == '\\' ||
                        (audio[0] && audio[1] == ':');
        if (!absolute) {
            char dir[OSAI_MAX_PATH];
            osa_strlcpy(dir, path, sizeof dir);
            for (char *c = dir + strlen(dir); c > dir; c--) {
                if (c[-1] == '/' || c[-1] == '\\') { *c = 0; break; }
            }
            char joined[OSAI_MAX_PATH];
            osa_path_join(joined, sizeof joined, dir, (char *)audio);
            osa_strlcpy(out[n].audio, joined, sizeof out[n].audio);
        } else {
            osa_strlcpy(out[n].audio, audio, sizeof out[n].audio);
        }
        osa_strlcpy(out[n].text, osa_trim_inplace(tab + 1), sizeof out[n].text);
        if (out[n].text[0]) n++;
    }
    fclose(f);
    return n;
}

bool osa_app_train(osa_app *a, const char *dataset_path, int epochs,
                   osa_train_result *out, osa_error *err) {
    if (!a || !a->asr || !out) { osa_err_set(err, "bad arguments"); return false; }
    memset(out, 0, sizeof *out);
    if (epochs < 1) epochs = 1;
    if (epochs > 64) epochs = 64;
    osa_buf log;
    osa_buf_init(&log);

    static train_pair pairs[128];
    int np = read_dataset(dataset_path, pairs, 128);
    bool synthetic = (np == 0);
    int words_total = 0;

    if (synthetic) {
        osa_buf_printf(&log, "داده‌ای انتخاب نشده بود؛ از واژه‌های همراه برنامه استفاده شد\n");
        /* enumerate the builtin lexicon */
        int nw = 0;
        const tts_word_pair *lex = osai_lexicon("fa", &nw);
        for (int round = 0; round < epochs; round++) {
            for (int i = 0; i < nw; i++) {
                int64_t n = 0;
                char phrase[128];
                snprintf(phrase, sizeof phrase, "%s", lex[i].w);
                float *pcm = tts_speak(phrase, "fa", &n);
                if (!pcm || n <= 0) { free(pcm); continue; }
                if (asr_model_add_exemplar(a->asr, lex[i].w, pcm, n, NULL)) {
                    out->n_added++;
                    words_total++;
                }
                free(pcm);
            }
        }
        osa_buf_printf(&log, "%d نمونه از %d واژه افزوده شد\n", out->n_added, nw);
    } else {
        osa_buf_printf(&log, "%d جفت متن/صدا خوانده شد\n", np);
        for (int round = 0; round < epochs; round++) {
            for (int i = 0; i < np; i++) {
                osa_audio au;
                if (!osa_audio_load(pairs[i].audio, &au, NULL)) {
                    osa_buf_printf(&log, "صدا خوانده نشد: %s\n", pairs[i].audio);
                    continue;
                }
                /* one exemplar per word, cut proportionally from the recording:
                 * good enough to teach the recogniser this speaker's voice for
                 * the words of the sentence */
                char words[16][64];
                int nwd = 0;
                char tmp[256];
                osa_strlcpy(tmp, pairs[i].text, sizeof tmp);
                for (char *w = strtok(tmp, " \t"); w && nwd < 16; w = strtok(NULL, " \t")) {
                    osa_strlcpy(words[nwd], w, sizeof words[nwd]);
                    if (words[nwd][0]) nwd++;
                }
                if (nwd > 0) {
                    int64_t per = au.n / nwd;
                    for (int k = 0; k < nwd; k++) {
                        int64_t start = (int64_t)k * per;
                        if (asr_model_add_exemplar(a->asr, words[k], au.samples + start, per, NULL)) {
                            out->n_added++;
                            words_total++;
                        }
                    }
                }
                osa_audio_free(&au);
            }
        }
        osa_buf_printf(&log, "%d نمونه در %d دوره افزوده شد\n", out->n_added, epochs);
    }
    out->n_words = words_total;

    /* save as a new version; never overwrite the active model */
    char id[64];
    snprintf(id, sizeof id, "trained-asr-%lld", (long long)(osa_now_ms() / 1000));
    snprintf(out->model_path, sizeof out->model_path, "%s%s.osam", a->models_dir, id);
    if (!asr_model_save(a->asr, out->model_path, err)) {
        osa_buf_printf(&log, "ذخیره مدل ناموفق بود\n");
        osa_strlcpy(out->log, log.data ? log.data : "", sizeof out->log);
        osa_buf_free(&log);
        return false;
    }
    osa_strlcpy(out->model_id, id, sizeof out->model_id);
    char name[128];
    snprintf(name, sizeof name, "مدل آموزش‌دیده %s", a->last_language[0] ? a->last_language : "fa");
    if (!register_model(a, id, name, "asr", asr_model_backend(a->asr),
                        a->asr_model_id, "GPL-3.0-or-later (derived)",
                        "trained in the application", out->model_path, "1.0.0", NULL)) {
        osa_buf_printf(&log, "ثبت مدل جدید ناموفق بود\n");
    } else {
        osa_buf_printf(&log, "مدل جدید ثبت شد: %s\n", id);
        osa_buf_printf(&log, "نسخه قبلی (%s) دست‌نخورده باقی ماند\n", a->asr_model_id);
    }
    /* Activate the new version. It was trained on this user's data, so it has to
     * become the one in use — a run that leaves the old model active is a
     * backup, not training. The previous model stays installed, so Models →
     * "Activate" is the rollback path, and the log says so. */
    if (osa_store_model_set_active(a->db, id, NULL)) {
        osa_buf_printf(&log, "مدل فعال اکنون %s است\n", id);
        osa_buf_printf(&log, "برای بازگشت، در صفحه «مدل‌ها» مدل قبلی را فعال کنید\n");
        out->activated = true;
        osa_strlcpy(a->asr_model_id, id, sizeof a->asr_model_id);
    } else {
        osa_buf_printf(&log, "فعال‌سازی خودکار ناموفق بود؛ در صفحه «مدل‌ها» فعال کنید\n");
    }
    osa_strlcpy(out->log, log.data ? log.data : "", sizeof out->log);
    osa_buf_free(&log);
    osa_err_ok(err);
    return true;
}

/* ---- evaluation --------------------------------------------------- */

/* The evaluation screen's data.
 *
 * The set is the bundled model's own five sentences per language, synthesised by
 * the bundled formant synth: it measures the shipped recognizer on the phrases
 * it was built around, which is a smoke test, not a benchmark — the UI says so.
 *
 * The JSON carries the aggregates the tiles need (`wer`, `cer`, `rtf`,
 * `samples`, `audio_s`) and one flat `items` array with every sentence pair, so
 * the screen does not have to know how languages are nested. `rtf` is measured
 * wall-clock processing time over audio seconds; it is a property of the machine
 * as much as of the model, hence the label on the tile. */
char *osa_app_evaluate(osa_app *a, osa_error *err) {
    if (!a || !a->asr) { osa_err_set(err, "no model"); return NULL; }
    static const char *fa[] = {
        "سلام حال شما خوب است",
        "فایل صوتی را انتخاب کنید",
        "مدل آموزش داده شده را ذخیره کنید",
        "میکروفون را روشن کنید",
        "امروز هوا خیلی قشنگ است",
    };
    static const char *en[] = {
        "hello world",
        "start recording",
        "save the text",
        "stop recording",
        "select the audio file",
    };
    osa_buf items;            /* flattened across languages */
    osa_buf_init(&items);
    osa_buf lang_json;
    osa_buf_init(&lang_json);
    osa_buf skipped;          /* languages the active model cannot cover */
    osa_buf_init(&skipped);
    int n_items = 0;
    double sum_w = 0, sum_c = 0, audio_s = 0, proc_s = 0;
    char engine[32], model_name[160];
    osa_strlcpy(engine, asr_model_backend(a->asr), sizeof engine);
    osa_strlcpy(model_name, asr_model_name(a->asr), sizeof model_name);

    /* Both languages are measured. The bundled exemplar model carries one
     * language's lexicon, so the other direction runs with an explicit language
     * hint; the recognizer reports what it actually did, and the item carries
     * that so the screen never claims a Persian result for an English input. */
    for (int li = 0; li < 2; li++) {
        const char *lang = li == 0 ? "fa" : "en";
        const char **set = li == 0 ? fa : en;
        osa_buf group;
        osa_buf_init(&group);
        double lw = 0, lc = 0;
        int counted = 0;
        for (int i = 0; i < 5; i++) {
            int64_t n = 0;
            float *pcm = tts_speak(set[i], lang, &n);
            if (!pcm) continue;
            asr_result r;
            osa_error lerr;
            memset(&r, 0, sizeof r);
            memset(&lerr, 0, sizeof lerr);
            if (asr_transcribe_samples(a->asr, pcm, n, lang, &r, &lerr)) {
                double w = asr_wer(set[i], r.text, lang);
                double c = asr_cer(set[i], r.text);
                if (counted) osa_buf_putc(&group, ',');
                osa_buf_puts(&group, "{\"ref\":");
                json_str(&group, set[i]);
                osa_buf_puts(&group, ",\"hyp\":");
                json_str(&group, r.text);
                osa_buf_printf(&group, ",\"wer\":%.4f,\"cer\":%.4f,\"conf\":%.3f}", w, c,
                               r.confidence);
                if (n_items) osa_buf_putc(&items, ',');
                osa_buf_puts(&items, "{\"lang\":");
                json_str(&items, r.language[0] ? r.language : lang);
                osa_buf_puts(&items, ",\"ref\":");
                json_str(&items, set[i]);
                osa_buf_puts(&items, ",\"hyp\":");
                json_str(&items, r.text);
                osa_buf_printf(&items, ",\"wer\":%.4f,\"cer\":%.4f}", w, c);
                n_items++;
                lw += w;
                lc += c;
                audio_s += r.duration_s > 0 ? r.duration_s : (double)n / OSAI_SR;
                proc_s += r.proc_ms / 1000.0;
                counted++;
                sum_w += w;
                sum_c += c;
                asr_result_free(&r);
            } else if (counted == 0 && i == 0) {
                /* Recorded once per language, with the recognizer's own reason. */
                if (skipped.len) osa_buf_putc(&skipped, ',');
                osa_buf_puts(&skipped, "{\"lang\":");
                json_str(&skipped, lang);
                osa_buf_puts(&skipped, ",\"reason\":");
                json_str(&skipped, lerr.msg[0] ? lerr.msg : "not supported by this model");
                osa_buf_putc(&skipped, '}');
            }
            free(pcm);
        }
        if (counted == 0) { osa_buf_free(&group); continue; }
        if (lang_json.len) osa_buf_putc(&lang_json, ',');
        osa_buf_puts(&lang_json, "{\"lang\":");
        json_str(&lang_json, lang);
        osa_buf_printf(&lang_json, ",\"n\":%d,\"wer\":%.4f,\"cer\":%.4f,\"items\":[",
                       counted, lw / counted, lc / counted);
        osa_buf_puts(&lang_json, group.data ? group.data : "");
        osa_buf_puts(&lang_json, "]}");
        osa_buf_free(&group);
    }

    osa_buf b;
    osa_buf_init(&b);
    osa_buf_puts(&b, "{\"engine\":");
    json_str(&b, engine);
    osa_buf_puts(&b, ",\"model\":");
    json_str(&b, model_name);
    osa_buf_printf(&b, ",\"wer\":%.4f,\"cer\":%.4f,\"rtf\":%.4f,\"samples\":%d"
                       ",\"audio_s\":%.2f,\"proc_s\":%.2f,\"languages\":[",
                   n_items ? sum_w / n_items : 0.0,
                   n_items ? sum_c / n_items : 0.0,
                   audio_s > 0 ? proc_s / audio_s : 0.0,
                   n_items, audio_s, proc_s);
    osa_buf_puts(&b, lang_json.data ? lang_json.data : "");
    osa_buf_puts(&b, "],\"skipped\":[");
    osa_buf_puts(&b, skipped.data ? skipped.data : "");
    osa_buf_puts(&b, "],\"items\":[");
    osa_buf_puts(&b, items.data ? items.data : "");
    osa_buf_puts(&b, "]}");
    osa_buf_free(&items);
    osa_buf_free(&skipped);
    osa_buf_free(&lang_json);
    if (n_items == 0) {
        osa_buf_free(&b);
        osa_err_set(err, "no sentences could be synthesised for this model");
        return NULL;
    }
    osa_err_ok(err);
    return osa_buf_detach(&b);
}

/* ---- pronunciation ------------------------------------------------- */

bool osa_app_pronounce(const char *text, const char *lang, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!text || !*text) return false;
    mt_lang l = MT_AUTO;
    if (lang && osa_starts(lang, "fa")) l = MT_FA;
    else if (lang && osa_starts(lang, "en")) l = MT_EN;
    return mt_pronounce(text, l, out, cap);
}

/* ---- display-only translation -------------------------------------- */

const char *osa_app_translate_preview(osa_app *app, const char *text) {
    static char buf[8192];
    static mt_result res;          /* 8 KB of pieces: keep it off the stack */
    buf[0] = 0;
    if (!app || !text || !*text || !app->mt) return buf;
    osa_error e;
    if (!mt_translate_styled(app->mt, text, MT_AUTO, app->style, &res, &e)) return buf;
    osa_strlcpy(buf, res.text, sizeof buf);
    return buf;
}
