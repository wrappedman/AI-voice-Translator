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

/* ---- translation -------------------------------------------------- */

bool osa_app_translate(osa_app *a, const char *text, const char *src,
                       osa_app_translation *out, osa_error *err) {
    if (!a || !a->mt || !text) { osa_err_set(err, "bad arguments"); return false; }
    mt_lang l = MT_AUTO;
    if (src && osa_starts(src, "fa")) l = MT_FA;
    else if (src && osa_starts(src, "en")) l = MT_EN;
    mt_result r;
    if (!mt_translate(a->mt, text, l, &r, err)) return false;
    memset(out, 0, sizeof *out);
    osa_uuid(out->id);
    osa_strlcpy(out->text, r.text, sizeof out->text);
    osa_strlcpy(out->source_lang, mt_lang_tag(r.src_lang), sizeof out->source_lang);
    osa_strlcpy(out->target_lang, mt_lang_tag(r.dst_lang), sizeof out->target_lang);
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
    const char *ext = osa_path_ext(path);
    bool is_asr = ext && osa_strieq(ext, "osam");
    bool is_mt = ext && osa_strieq(ext, "osmt");
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

    if (!register_model(a, id, is_asr ? "Imported ASR model" : "Imported translation model",
                        is_asr ? "asr" : "mt", is_asr ? "osam" : "osmt", "",
                        "see the model card", "imported by the user", dst, "1.0.0", err))
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

/* ---- evaluation --------------------------------------------------- */

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
    osa_buf b;
    osa_buf_init(&b);
    osa_buf_puts(&b, "{\"engine\":");
    json_str(&b, asr_model_backend(a->asr));
    osa_buf_puts(&b, ",\"model\":");
    json_str(&b, asr_model_name(a->asr));
    osa_buf_puts(&b, ",\"languages\":[");
    for (int li = 0; li < 2; li++) {
        const char *lang = li == 0 ? "fa" : "en";
        const char **set = li == 0 ? fa : en;
        int ns = 5;
        /* the builtin model holds one language at a time */
        if (!osa_starts(asr_model_language(a->asr), lang)) continue;
        if (b.len > 0 && b.data[b.len - 1] != '[') osa_buf_putc(&b, ',');
        osa_buf_puts(&b, "{\"lang\":");
        json_str(&b, lang);
        osa_buf_puts(&b, ",\"items\":[");
        double sum_w = 0, sum_c = 0;
        int counted = 0;
        for (int i = 0; i < ns; i++) {
            int64_t n = 0;
            float *pcm = tts_speak(set[i], lang, &n);
            if (!pcm) continue;
            asr_result r;
            if (asr_transcribe_samples(a->asr, pcm, n, lang, &r, NULL)) {
                double w = asr_wer(set[i], r.text, lang);
                double c = asr_cer(set[i], r.text);
                if (i) osa_buf_putc(&b, ',');
                osa_buf_puts(&b, "{\"ref\":");
                json_str(&b, set[i]);
                osa_buf_puts(&b, ",\"hyp\":");
                json_str(&b, r.text);
                osa_buf_printf(&b, ",\"wer\":%.4f,\"cer\":%.4f,\"conf\":%.3f}", w, c,
                               r.confidence);
                sum_w += w;
                sum_c += c;
                counted++;
                asr_result_free(&r);
            }
            free(pcm);
        }
        osa_buf_printf(&b, "],\"wer\":%.4f,\"cer\":%.4f,\"n\":%d}",
                       counted ? sum_w / counted : 0.0,
                       counted ? sum_c / counted : 0.0, counted);
    }
    osa_buf_puts(&b, "]}");
    osa_err_ok(err);
    return osa_buf_detach(&b);
}

/* ---- display-only translation -------------------------------------- */

const char *osa_app_translate_preview(osa_app *app, const char *text) {
    static char buf[8192];
    static mt_result res;          /* 8 KB of pieces: keep it off the stack */
    buf[0] = 0;
    if (!app || !text || !*text || !app->mt) return buf;
    osa_error e;
    if (!mt_translate(app->mt, text, MT_AUTO, &res, &e)) return buf;
    osa_strlcpy(buf, res.text, sizeof buf);
    return buf;
}
