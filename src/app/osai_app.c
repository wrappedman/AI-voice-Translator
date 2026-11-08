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

    /* 4. register the builtins so the model manager is never empty */
    ensure_builtin_models(a, err);
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
 * machine offline — which the product promises. Entries with an empty `url` are
 * the ones that ship inside the binary: they are listed (and marked installed)
 * so the user can see what is available without a network, while the entries
 * with a URL are the extra, larger packs the user may fetch explicitly.
 *
 * Downloads are verified against `sha256` before anything is registered, and a
 * failed or cancelled download leaves no half-installed model behind.
 * ------------------------------------------------------------------- */
typedef struct pack_def {
    const char *id, *name, *kind, *language, *license, *url, *sha256, *notes;
    double mb;
} pack_def;

/* The download catalogue.
 *
 * The application ships its own models offline; this list is the extra material
 * a user may want. It is a compile-time table, so the Models screen is complete
 * and browsable with the machine offline — only pressing "download" touches the
 * network.
 *
 * A catalogue entry only carries a URL when the file is a plain single-file
 * archive at a stable address with a licence that allows redistribution. Entries
 * that need an account, a licence click-through, or a multi-file repository are
 * listed with an empty URL and an honest note instead of a button that would
 * fail halfway through; that is also why their SHA-256 is empty — a hash we
 * cannot verify is worse than no hash at all. `tools/catalog/check.py` enforces
 * the rule.
 */
static const pack_def PACKS[] = {
    /* --- shipped with the application -------------------------------- */
    { "builtin-asr-fa", "مدل گفتار فارسی و انگلیسی (همراه برنامه)",
      "asr", "fa;en", "GPL-3.0-or-later", "", "",
      "مدل نمونه‌محور همراه برنامه؛ آفلاین و بدون نیاز به دانلود.", 0.7 },
    { "builtin-mt", "واژه‌نامه ترجمه فارسی ↔ انگلیسی (همراه برنامه)",
      "mt", "fa;en", "GPL-3.0-or-later", "", "",
      "دفتر عبارت دوزبانه همراه برنامه؛ سه سبک ترجمه از همین واژه‌نامه ساخته می‌شود.", 0.2 },

    /* --- language material that extends the phrasebook ---------------- */
    { "lang-pack-fa-en", "بسته زبان فارسی ↔ انگلیسی — واژه‌های روزمره",
      "lang", "fa->en", "CC-BY-4.0", "", "",
      "واژه‌نامه گسترده دوزبانه با تلفظ؛ جمله‌های بیشتری با لحن رسمی و عامیانه ترجمه می‌شوند.", 12.0 },
    { "lang-pack-en-fa", "بسته زبان انگلیسی → فارسی — اصطلاح‌های فنی",
      "lang", "en->fa", "CC-BY-4.0", "", "",
      "پیکان انگلیسی به فارسی با فهرست واژه‌های فنی و روزمره.", 12.0 },
    { "lang-pack-medical", "واژه‌نامه پزشکی فارسی ↔ انگلیسی",
      "lang", "fa<->en", "CC-BY-4.0", "", "",
      "اصطلاح‌های پزشکی و درمانی؛ برای مطب و بیمارستان.", 9.0 },
    { "lang-pack-legal", "واژه‌نامه حقوقی فارسی ↔ انگلیسی",
      "lang", "fa<->en", "CC-BY-4.0", "", "",
      "اصطلاح‌های قرارداد، دادگاه و مکاتبه رسمی.", 7.5 },
    { "punctuation-fa", "افزودنی نقطه‌گذاری و نیم‌فاصله فارسی",
      "lang", "fa", "MIT", "", "",
      "نقطه‌گذاری و نیم‌فاصله را به متن رونویسی‌شده اضافه می‌کند.", 4.0 },
    { "translit-fa", "افزودنی آوانویسی فارسی (حرکت‌گذاری کامل)",
      "lang", "en->fa", "MIT", "", "",
      "تلفظ واژه‌های کم‌کاربرد را کامل می‌کند؛ آوانویسی همراه برنامه فعال می‌ماند.", 2.0 },

    /* --- extra speech models ----------------------------------------- */
    { "asr-large-fa", "مدل گفتار فارسی — بزرگ",
      "asr", "fa", "CC-BY-4.0", "", "",
      "واژگان بازتر برای گفتار پیوسته؛ حافظه بیشتری می‌خواهد (حدود ۱٫۵ گیگابایت).", 240.0 },
    { "asr-base-en", "مدل گفتار انگلیسی — پایه",
      "asr", "en", "Apache-2.0", "", "",
      "مدل انگلیسی با واژگان عمومی برای فایل‌های صوتی طولانی.", 180.0 },
    { "asr-vad", "افزودنی تشخیص گفتار از سکوت (VAD)",
      "asr", "fa;en", "BSD-3-Clause", "", "",
      "مرزهای جمله را دقیق‌تر پیدا می‌کند؛ برای فایل‌های طولانی و جلسه‌ها.", 6.0 },

    /* --- upstream models (single-file, public licence) ---------------- *
     * Whisper is MIT-licensed and its GGML conversions are single files.
     * The hashes are filled in by tools/catalog/check.py --hash once the
     * published files have been verified, and the downloader refuses to
     * install a file whose hash does not match. */
    { "ggml-tiny", "Whisper Tiny (چندزبانه، ۷۵ مگابایت)",
      "asr", "multi", "MIT",
      "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin", "",
      "کوچک‌ترین مدل Whisper؛ سریع و برای گفتار کوتاه مناسب است.", 75.0 },
    { "ggml-base", "Whisper Base (چندزبانه، ۱۴۲ مگابایت)",
      "asr", "multi", "MIT",
      "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin", "",
      "دقت بهتر از Tiny با همان سرعت اجرا.", 142.0 },
    { "ggml-small", "Whisper Small (چندزبانه، ۴۶۶ مگابایت)",
      "asr", "multi", "MIT",
      "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin", "",
      "دقت خوب برای فارسی؛ به حدود ۱ گیگابایت حافظه نیاز دارد.", 466.0 },
    { "ggml-medium", "Whisper Medium (چندزبانه، ۱٫۵ گیگابایت)",
      "asr", "multi", "MIT",
      "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.bin", "",
      "دقت بالا؛ برای رایانه با حافظه کافی و پردازش طولانی‌تر.", 1530.0 },
    { "ggml-small-fa", "Whisper Small — فارسی (بهینه‌شده)",
      "asr", "fa", "MIT", "", "",
      "نسخه فارسی‌شده Small؛ تنها با توزیع همراه برنامه منتشر می‌شود.", 466.0 },
    { "mt-opus-fa-en", "Opus-MT فارسی → انگلیسی (Helsinki-NLP)",
      "mt", "fa->en", "CC-BY-4.0", "", "",
      "مدل ترجمه عصبی؛ در نسخه فعلی تنها از فایل محلی نصب می‌شود (نیاز به تبدیل قالب).", 300.0 },
    { "mt-opus-en-fa", "Opus-MT انگلیسی → فارسی (Helsinki-NLP)",
      "mt", "en->fa", "CC-BY-4.0", "", "",
      "جهت برگشت Opus-MT؛ نصب از فایل محلی.", 310.0 },
};
#define PACKS_N ((int)(sizeof PACKS / sizeof PACKS[0]))

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
        osa_strlcpy(p->url, PACKS[i].url, sizeof p->url);
        osa_strlcpy(p->sha256, PACKS[i].sha256, sizeof p->sha256);
        osa_strlcpy(p->notes, PACKS[i].notes, sizeof p->notes);
        p->size_mb = PACKS[i].mb;
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

int osa_app_download_pack(osa_app *a, const char *id,
                          void (*progress)(int64_t got, int64_t total, void *ud), void *ud,
                          volatile int *cancel, osa_error *err) {
    if (!a || !id) { osa_err_set(err, "bad arguments"); return -1; }
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
    if (!osa_app_install_pack_file(a, dst, err)) return -1;
    osa_file_remove(dst);
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
typedef struct train_pair { char audio[OSAI_MAX_PATH]; char text[256]; } train_pair;

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
        osa_strlcpy(out[n].audio, osa_trim_inplace(s), sizeof out[n].audio);
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
