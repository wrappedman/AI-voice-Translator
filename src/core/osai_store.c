#include "osai_store.h"
#include "osai_platform.h"
#include "third_party/sqlite/sqlite3.h"

struct osa_db { sqlite3 *h; char path[OSAI_MAX_PATH]; };

/* the 18 tables of the design doc (§9) */
static const char *SCHEMA =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS schema_meta(key TEXT PRIMARY KEY, value TEXT);"
    "CREATE TABLE IF NOT EXISTS AudioFiles("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, path TEXT NOT NULL UNIQUE, name TEXT,"
    "  size_bytes INTEGER, duration_ms INTEGER, format TEXT, language TEXT, created_at TEXT);"
    "CREATE TABLE IF NOT EXISTS Transcriptions("
    "  id TEXT PRIMARY KEY, audio_id INTEGER, audio_path TEXT, model_id TEXT, model_name TEXT,"
    "  language TEXT, source TEXT, text TEXT, corrected_text TEXT, confidence REAL,"
    "  duration_ms INTEGER, proc_ms INTEGER, status TEXT, created_at TEXT);"
    "CREATE TABLE IF NOT EXISTS Segments("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, transcription_id TEXT, idx INTEGER,"
    "  start_ms INTEGER, end_ms INTEGER, text TEXT, confidence REAL);"
    "CREATE TABLE IF NOT EXISTS Translations("
    "  id TEXT PRIMARY KEY, transcription_id TEXT, source_text TEXT, text TEXT,"
    "  source_lang TEXT, target_lang TEXT, engine TEXT, created_at TEXT);"
    "CREATE TABLE IF NOT EXISTS Languages("
    "  code TEXT PRIMARY KEY, name_en TEXT, name_fa TEXT, native TEXT, rtl INTEGER);"
    "CREATE TABLE IF NOT EXISTS Models("
    "  id TEXT PRIMARY KEY, name TEXT, kind TEXT, engine TEXT, base_model TEXT,"
    "  version TEXT, path TEXT, size_bytes INTEGER, source_url TEXT, license TEXT,"
    "  sha256 TEXT, status TEXT, active INTEGER DEFAULT 0, notes TEXT, created_at TEXT);"
    "CREATE TABLE IF NOT EXISTS ModelVersions("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, model_id TEXT, version TEXT, path TEXT,"
    "  sha256 TEXT, metrics TEXT, created_at TEXT);"
    "CREATE TABLE IF NOT EXISTS Datasets("
    "  id TEXT PRIMARY KEY, name TEXT, language TEXT, source TEXT, license TEXT, path TEXT,"
    "  size_bytes INTEGER, sha256 TEXT, file_count INTEGER, clip_count INTEGER,"
    "  hours_x1000 INTEGER, notes TEXT, created_at TEXT);"
    "CREATE TABLE IF NOT EXISTS DatasetFiles("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, dataset_id TEXT, audio_path TEXT, text TEXT,"
    "  split TEXT, duration_ms INTEGER, speaker TEXT, valid INTEGER DEFAULT 1);"
    "CREATE TABLE IF NOT EXISTS TrainingJobs("
    "  id TEXT PRIMARY KEY, base_model_id TEXT, dataset_id TEXT, status TEXT, stage TEXT,"
    "  output_model_id TEXT, config_json TEXT, epoch INTEGER, total_epochs INTEGER,"
    "  step INTEGER, total_steps INTEGER, progress REAL, train_loss REAL, val_loss REAL,"
    "  wer REAL, cer REAL, lr REAL, started_ms INTEGER, eta_ms INTEGER,"
    "  can_resume INTEGER, can_stop INTEGER, created_at TEXT, finished_at TEXT);"
    "CREATE TABLE IF NOT EXISTS TrainingMetrics("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, job_id TEXT, name TEXT, value REAL,"
    "  step INTEGER, created_at TEXT);"
    "CREATE TABLE IF NOT EXISTS UserCorrections("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, transcription_id TEXT, original TEXT,"
    "  corrected TEXT, confidence REAL, status TEXT DEFAULT 'pending', created_at TEXT);"
    "CREATE TABLE IF NOT EXISTS DownloadHistory("
    "  id TEXT PRIMARY KEY, catalog_id TEXT, url TEXT, dest_path TEXT, status TEXT,"
    "  total_bytes INTEGER, received_bytes INTEGER, sha256 TEXT, error TEXT,"
    "  started_at TEXT, finished_at TEXT);"
    "CREATE TABLE IF NOT EXISTS Catalog("
    "  id TEXT PRIMARY KEY, name TEXT, kind TEXT, language TEXT, url TEXT, license TEXT,"
    "  homepage TEXT, size_text TEXT, size_bytes INTEGER, sha256 TEXT, local_path TEXT,"
    "  downloaded_bytes INTEGER DEFAULT 0, status TEXT DEFAULT 'available',"
    "  min_free_gb TEXT, notes TEXT);"
    "CREATE TABLE IF NOT EXISTS SystemLogs("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT, level TEXT, tag TEXT, msg TEXT, created_at TEXT);"
    "CREATE TABLE IF NOT EXISTS Settings(key TEXT PRIMARY KEY, value TEXT);"
    "CREATE UNIQUE INDEX IF NOT EXISTS ux_model_active ON Models(kind) WHERE active=1;"
    "CREATE INDEX IF NOT EXISTS ix_tr_created ON Transcriptions(created_at DESC);"
    "CREATE INDEX IF NOT EXISTS ix_dl_started ON DownloadHistory(started_at DESC);"
    "CREATE INDEX IF NOT EXISTS ix_seg_tr ON Segments(transcription_id, idx);"
    "INSERT OR IGNORE INTO schema_meta(key,value) VALUES('version','1');"
    "INSERT OR REPLACE INTO Languages(code,name_en,name_fa,native,rtl) VALUES"
    " ('fa','Persian','فارسی','فارسی',1),"
    " ('en','English','انگلیسی','English',0),"
    " ('de','German','آلمانی','Deutsch',0),"
    " ('fr','French','فرانسوی','Français',0),"
    " ('es','Spanish','اسپانیایی','Español',0),"
    " ('ar','Arabic','عربی','العربية',1),"
    " ('tr','Turkish','ترکی','Türkçe',0),"
    " ('ru','Russian','روسی','Русский',0);";

osa_db *osa_db_open(const char *path, osa_error *err) {
    sqlite3 *h = NULL;
    int rc = sqlite3_open_v2(path, &h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        osa_err_set(err, "sqlite open failed: %s", h ? sqlite3_errmsg(h) : "?");
        if (h) sqlite3_close(h);
        return NULL;
    }
    sqlite3_busy_timeout(h, 5000);
    osa_db *db = (osa_db *)calloc(1, sizeof *db);
    db->h = h;
    osa_strlcpy(db->path, path, sizeof db->path);
    if (!osa_db_migrate(db, err)) { osa_db_close(db); return NULL; }
    osa_err_ok(err);
    return db;
}

void osa_db_close(osa_db *db) {
    if (!db) return;
    if (db->h) sqlite3_close(db->h);
    free(db);
}

bool osa_db_migrate(osa_db *db, osa_error *err) {
    char *msg = NULL;
    /* Repair old interrupted/non-atomic activations before adding the invariant. */
    sqlite3_exec(db->h, "UPDATE Models SET active=0 WHERE active=1 AND rowid NOT IN (SELECT MAX(rowid) FROM Models WHERE active=1 GROUP BY kind);", NULL, NULL, NULL);
    if (sqlite3_exec(db->h, SCHEMA, NULL, NULL, &msg) != SQLITE_OK) {
        osa_err_set(err, "schema error: %s", msg ? msg : "?");
        sqlite3_free(msg);
        return false;
    }
    return true;
}

char *osa_sql_escape(osa_arena *a, const char *s) {
    if (!s) return osa_arena_strdup(a, "");
    osa_buf b; osa_buf_init(&b);
    for (const char *p = s; *p; p++) {
        if (*p == '\'') osa_buf_puts(&b, "''");
        else osa_buf_putc(&b, *p);
    }
    char *raw = osa_buf_detach(&b);
    char *out = osa_arena_strdup(a, raw);
    free(raw);
    return out;
}

bool osa_sql_exec(osa_db *db, const char *sql, osa_error *err) {
    char *msg = NULL;
    if (sqlite3_exec(db->h, sql, NULL, NULL, &msg) != SQLITE_OK) {
        osa_err_set(err, "sql: %s", msg ? msg : "?");
        sqlite3_free(msg);
        return false;
    }
    osa_err_ok(err);
    return true;
}

void osa_rs_free(osa_rs *rs) {
    /* rows/values live in the arena */
    osa_arena_free(&rs->arena);
    memset(rs, 0, sizeof *rs);
}

bool osa_sql_query(osa_db *db, const char *sql, osa_rs *rs, osa_error *err) {
    memset(rs, 0, sizeof *rs);
    osa_arena_init(&rs->arena);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) {
        osa_err_set(err, "prepare: %s", sqlite3_errmsg(db->h));
        return false;
    }
    rs->ncols = sqlite3_column_count(st);
    if (rs->ncols > 40) rs->ncols = 40;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (rs->nrows == rs->cap) {
            int ncap = rs->cap ? rs->cap * 2 : 64;
            osa_row *nr = (osa_row *)osa_arena_alloc(&rs->arena, (size_t)ncap * sizeof(osa_row));
            if (!nr) break;
            if (rs->rows) memcpy(nr, rs->rows, (size_t)rs->nrows * sizeof(osa_row));
            rs->rows = nr;
            rs->cap = ncap;
        }
        osa_row *row = &rs->rows[rs->nrows];
        memset(row, 0, sizeof *row);
        for (int i = 0; i < rs->ncols; i++) {
            const char *name = sqlite3_column_name(st, i);
            row->cols[i] = osa_arena_strdup(&rs->arena, name ? name : "");
            const unsigned char *v = sqlite3_column_text(st, i);
            row->values[i] = osa_arena_strdup(&rs->arena, v ? (const char *)v : "");
        }
        row->ncols = rs->ncols;
        rs->nrows++;
    }
    sqlite3_finalize(st);
    osa_err_ok(err);
    return true;
}

const char *osa_rs_val(const osa_rs *rs, int row, const char *col) {
    if (row < 0 || row >= rs->nrows) return "";
    for (int i = 0; i < rs->rows[row].ncols; i++)
        if (osa_strieq(rs->rows[row].cols[i], col)) return rs->rows[row].values[i];
    return "";
}

int64_t osa_rs_i64(const osa_rs *rs, int row, const char *col) {
    const char *v = osa_rs_val(rs, row, col);
    return v && *v ? (int64_t)atoll(v) : 0;
}
int osa_rs_int(const osa_rs *rs, int row, const char *col) { return (int)osa_rs_i64(rs, row, col); }
double osa_rs_dbl(const osa_rs *rs, int row, const char *col) {
    const char *v = osa_rs_val(rs, row, col);
    return v && *v ? atof(v) : 0.0;
}

/* ============================== settings ============================== */
void osa_db_set_setting(osa_db *db, const char *key, const char *value) {
    osa_arena a; osa_arena_init(&a);
    char sql[2048];
    snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO Settings(key,value) VALUES('%s','%s')",
             osa_sql_escape(&a, key), osa_sql_escape(&a, value));
    char *msg = NULL;
    sqlite3_exec(db->h, sql, NULL, NULL, &msg);
    sqlite3_free(msg);
    osa_arena_free(&a);
}

char *osa_db_get_setting(osa_db *db, const char *key, osa_arena *a, const char *def) {
    osa_rs rs; osa_error e;
    osa_buf sql; osa_buf_init(&sql);
    osa_arena tmp; osa_arena_init(&tmp);
    osa_buf_printf(&sql, "SELECT value FROM Settings WHERE key='%s'", osa_sql_escape(&tmp, key));
    char *out = NULL;
    if (osa_sql_query(db, sql.data, &rs, &e) && rs.nrows > 0)
        out = osa_arena_strdup(a, osa_rs_val(&rs, 0, "value"));
    osa_rs_free(&rs);
    osa_arena_free(&tmp);
    osa_buf_free(&sql);
    if (!out) out = osa_arena_strdup(a, def ? def : "");
    return out;
}

/* ============================== models ============================== */
bool osa_store_model_add(osa_db *db, const osa_model *m, osa_error *err) {
    osa_arena a; osa_arena_init(&a); osa_buf sql; osa_buf_init(&sql);
    osa_buf_printf(&sql,
        "INSERT INTO Models(id,name,kind,engine,base_model,version,path,size_bytes,source_url,license,sha256,status,active,notes,created_at) VALUES("
        "'%s','%s','%s','%s','%s','%s','%s',%lld,'%s','%s','%s','%s',0,'%s','%s') "
        "ON CONFLICT(id) DO UPDATE SET name=excluded.name,kind=excluded.kind,engine=excluded.engine,base_model=excluded.base_model,version=excluded.version,path=excluded.path,size_bytes=excluded.size_bytes,source_url=excluded.source_url,license=excluded.license,sha256=excluded.sha256,status=excluded.status,notes=excluded.notes;"
        "INSERT INTO ModelVersions(model_id,version,path,sha256,metrics,created_at) SELECT '%s','%s','%s','%s','','%s' WHERE NOT EXISTS (SELECT 1 FROM ModelVersions WHERE model_id='%s' AND version='%s' AND sha256='%s');",
        osa_sql_escape(&a,m->id),osa_sql_escape(&a,m->name),osa_sql_escape(&a,m->kind),osa_sql_escape(&a,m->engine),osa_sql_escape(&a,m->base_model),osa_sql_escape(&a,m->version),osa_sql_escape(&a,m->path),(long long)m->size_bytes,osa_sql_escape(&a,m->source_url),osa_sql_escape(&a,m->license),osa_sql_escape(&a,m->sha256),osa_sql_escape(&a,m->status),osa_sql_escape(&a,m->notes),osa_sql_escape(&a,m->created_at),
        osa_sql_escape(&a,m->id),osa_sql_escape(&a,m->version),osa_sql_escape(&a,m->path),osa_sql_escape(&a,m->sha256),osa_sql_escape(&a,m->created_at),osa_sql_escape(&a,m->id),osa_sql_escape(&a,m->version),osa_sql_escape(&a,m->sha256));
    bool ok=osa_sql_exec(db,sql.data,err);osa_buf_free(&sql);osa_arena_free(&a);return ok;
}

bool osa_store_model_list(osa_db *db, osa_rs *rs, const char *kind, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    if (kind && *kind)
        osa_buf_printf(&sql, "SELECT * FROM Models WHERE kind='%s' ORDER BY created_at DESC",
                       osa_sql_escape(&a, kind));
    else osa_buf_puts(&sql, "SELECT * FROM Models ORDER BY created_at DESC");
    bool ok = osa_sql_query(db, sql.data, rs, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_model_set_active(osa_db *db, const char *id, osa_error *err) {
    if(sqlite3_exec(db->h,"BEGIN IMMEDIATE;",NULL,NULL,NULL)!=SQLITE_OK){osa_err_set(err,"cannot begin model activation");return false;}
    sqlite3_stmt *st=NULL;bool ok=false;
    if(sqlite3_prepare_v2(db->h,"SELECT kind FROM Models WHERE id=?",-1,&st,NULL)==SQLITE_OK){sqlite3_bind_text(st,1,id,-1,SQLITE_TRANSIENT);if(sqlite3_step(st)==SQLITE_ROW){const char*kind=(const char*)sqlite3_column_text(st,0);char k[24];osa_strlcpy(k,kind?kind:"",sizeof k);sqlite3_finalize(st);st=NULL;
        if(sqlite3_prepare_v2(db->h,"UPDATE Models SET active=0 WHERE kind=?",-1,&st,NULL)==SQLITE_OK){sqlite3_bind_text(st,1,k,-1,SQLITE_TRANSIENT);ok=sqlite3_step(st)==SQLITE_DONE;sqlite3_finalize(st);st=NULL;}
        if(ok&&sqlite3_prepare_v2(db->h,"UPDATE Models SET active=1 WHERE id=? AND kind=?",-1,&st,NULL)==SQLITE_OK){sqlite3_bind_text(st,1,id,-1,SQLITE_TRANSIENT);sqlite3_bind_text(st,2,k,-1,SQLITE_TRANSIENT);ok=sqlite3_step(st)==SQLITE_DONE&&sqlite3_changes(db->h)==1;sqlite3_finalize(st);st=NULL;}
    }}
    if(st)sqlite3_finalize(st);
    if(ok)ok=sqlite3_exec(db->h,"COMMIT;",NULL,NULL,NULL)==SQLITE_OK;else sqlite3_exec(db->h,"ROLLBACK;",NULL,NULL,NULL);
    if(!ok)osa_err_set(err,"model activation failed; previous choice preserved");return ok;
}

const char *osa_store_model_active(osa_db *db, const char *kind, osa_arena *a) {
    osa_rs rs; osa_error e;
    osa_buf sql; osa_buf_init(&sql);
    osa_arena tmp; osa_arena_init(&tmp);
    osa_buf_printf(&sql, "SELECT path FROM Models WHERE kind='%s' AND active=1 LIMIT 1",
                   osa_sql_escape(&tmp, kind));
    const char *out = "";
    if (osa_sql_query(db, sql.data, &rs, &e) && rs.nrows > 0)
        out = osa_arena_strdup(a, osa_rs_val(&rs, 0, "path"));
    osa_rs_free(&rs);
    osa_buf_free(&sql);
    osa_arena_free(&tmp);
    return out;
}

bool osa_store_model_delete(osa_db *db, const char *id, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    const char *idq = osa_sql_escape(&a, id);
    osa_buf_printf(&sql, "DELETE FROM Models WHERE id='%s';DELETE FROM ModelVersions WHERE model_id='%s';",
                   idq, idq);
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

/* ============================== datasets ============================== */
bool osa_store_dataset_add(osa_db *db, const osa_dataset *d, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    osa_buf_printf(&sql,
        "INSERT OR REPLACE INTO Datasets(id,name,language,source,license,path,size_bytes,sha256,"
        "file_count,clip_count,hours_x1000,notes,created_at) VALUES("
        "'%s','%s','%s','%s','%s','%s',%lld,'%s',%d,%d,%lld,'%s','%s')",
        osa_sql_escape(&a, d->id), osa_sql_escape(&a, d->name), osa_sql_escape(&a, d->language),
        osa_sql_escape(&a, d->source), osa_sql_escape(&a, d->license),
        osa_sql_escape(&a, d->path), (long long)d->size_bytes, osa_sql_escape(&a, d->sha256),
        d->file_count, d->clip_count, (long long)d->hours_x1000,
        osa_sql_escape(&a, d->notes), osa_sql_escape(&a, d->created_at));
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_dataset_list(osa_db *db, osa_rs *rs, osa_error *err) {
    return osa_sql_query(db, "SELECT * FROM Datasets ORDER BY created_at DESC", rs, err);
}

/* ============================== training ============================== */
bool osa_store_job_add(osa_db *db, const osa_training_job *j, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    osa_buf_printf(&sql,
        "INSERT OR REPLACE INTO TrainingJobs(id,base_model_id,dataset_id,status,stage,"
        "output_model_id,config_json,epoch,total_epochs,step,total_steps,progress,train_loss,"
        "val_loss,wer,cer,lr,started_ms,eta_ms,can_resume,can_stop,created_at,finished_at) VALUES("
        "'%s','%s','%s','%s','%s','%s','%s',%d,%d,%d,%d,%f,%f,%f,%f,%f,%f,%lld,%lld,%d,%d,'%s','%s')",
        osa_sql_escape(&a, j->id), osa_sql_escape(&a, j->base_model_id),
        osa_sql_escape(&a, j->dataset_id), osa_sql_escape(&a, j->status),
        osa_sql_escape(&a, j->stage), osa_sql_escape(&a, j->output_model_id),
        osa_sql_escape(&a, j->config_json), j->epoch, j->total_epochs, j->step, j->total_steps,
        j->progress, j->train_loss, j->val_loss, j->wer, j->cer, j->lr,
        (long long)j->started_ms, (long long)j->eta_ms,
        j->can_resume ? 1 : 0, j->can_stop ? 1 : 0,
        osa_sql_escape(&a, j->created_at), osa_sql_escape(&a, j->finished_at));
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_job_update(osa_db *db, const osa_training_job *j, osa_error *err) {
    return osa_store_job_add(db, j, err);
}

bool osa_store_job_list(osa_db *db, osa_rs *rs, osa_error *err) {
    return osa_sql_query(db, "SELECT * FROM TrainingJobs ORDER BY started_ms DESC", rs, err);
}

bool osa_store_metric_add(osa_db *db, const char *job_id, const char *name, double value,
                          int step, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    char ts[32]; osa_iso_time(osa_now_ms(), ts);
    osa_buf_printf(&sql,
        "INSERT INTO TrainingMetrics(job_id,name,value,step,created_at) VALUES('%s','%s',%f,%d,'%s')",
        osa_sql_escape(&a, job_id), osa_sql_escape(&a, name), value, step, ts);
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

/* ============================== catalog ============================== */
bool osa_store_catalog_sync(osa_db *db, const osa_catalog_entry *e, int n, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    osa_buf_puts(&sql, "BEGIN;");
    for (int i = 0; i < n; i++) {
        const osa_catalog_entry *c = &e[i];
        osa_buf_printf(&sql,
            "INSERT INTO Catalog(id,name,kind,language,url,license,homepage,size_text,size_bytes,"
            "sha256,local_path,status,min_free_gb,notes) VALUES("
            "'%s','%s','%s','%s','%s','%s','%s','%s',%lld,'%s','%s','%s','%s','%s')"
            " ON CONFLICT(id) DO UPDATE SET name=excluded.name,kind=excluded.kind,"
            "language=excluded.language,url=excluded.url,license=excluded.license,"
            "homepage=excluded.homepage,size_text=excluded.size_text,size_bytes=excluded.size_bytes,"
            "sha256=excluded.sha256,min_free_gb=excluded.min_free_gb,notes=excluded.notes;",
            osa_sql_escape(&a, c->id), osa_sql_escape(&a, c->name), osa_sql_escape(&a, c->kind),
            osa_sql_escape(&a, c->language), osa_sql_escape(&a, c->url),
            osa_sql_escape(&a, c->license), osa_sql_escape(&a, c->homepage),
            osa_sql_escape(&a, c->size_text), (long long)c->size_bytes,
            osa_sql_escape(&a, c->sha256), osa_sql_escape(&a, c->local_path),
            *c->status ? c->status : "available",
            osa_sql_escape(&a, c->min_free_gb), osa_sql_escape(&a, c->notes));
    }
    osa_buf_puts(&sql, "COMMIT;");
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_catalog_list(osa_db *db, osa_rs *rs, osa_error *err) {
    return osa_sql_query(db, "SELECT * FROM Catalog ORDER BY kind, language, name", rs, err);
}

bool osa_store_catalog_update_status(osa_db *db, const char *id, const char *status,
                                     int64_t downloaded, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    osa_buf_printf(&sql, "UPDATE Catalog SET status='%s', downloaded_bytes=%lld WHERE id='%s'",
                   osa_sql_escape(&a, status), (long long)downloaded, osa_sql_escape(&a, id));
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_catalog_get(osa_db *db, const char *id, osa_catalog_entry *out, osa_error *err) {
    osa_rs rs;
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    osa_buf_printf(&sql, "SELECT * FROM Catalog WHERE id='%s'", osa_sql_escape(&a, id));
    bool ok = osa_sql_query(db, sql.data, &rs, err) && rs.nrows > 0;
    if (ok) {
        memset(out, 0, sizeof *out);
        osa_strlcpy(out->id, osa_rs_val(&rs, 0, "id"), sizeof out->id);
        osa_strlcpy(out->name, osa_rs_val(&rs, 0, "name"), sizeof out->name);
        osa_strlcpy(out->kind, osa_rs_val(&rs, 0, "kind"), sizeof out->kind);
        osa_strlcpy(out->language, osa_rs_val(&rs, 0, "language"), sizeof out->language);
        osa_strlcpy(out->url, osa_rs_val(&rs, 0, "url"), sizeof out->url);
        osa_strlcpy(out->license, osa_rs_val(&rs, 0, "license"), sizeof out->license);
        osa_strlcpy(out->homepage, osa_rs_val(&rs, 0, "homepage"), sizeof out->homepage);
        osa_strlcpy(out->size_text, osa_rs_val(&rs, 0, "size_text"), sizeof out->size_text);
        osa_strlcpy(out->sha256, osa_rs_val(&rs, 0, "sha256"), sizeof out->sha256);
        osa_strlcpy(out->status, osa_rs_val(&rs, 0, "status"), sizeof out->status);
        out->size_bytes = osa_rs_i64(&rs, 0, "size_bytes");
        out->downloaded_bytes = osa_rs_i64(&rs, 0, "downloaded_bytes");
    }
    osa_rs_free(&rs);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_download_add(osa_db *db, const osa_download *d, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    osa_buf_printf(&sql,
        "INSERT OR REPLACE INTO DownloadHistory(id,catalog_id,url,dest_path,status,total_bytes,"
        "received_bytes,sha256,error,started_at,finished_at) VALUES("
        "'%s','%s','%s','%s','%s',%lld,%lld,'%s','%s','%s','%s')",
        osa_sql_escape(&a, d->id), osa_sql_escape(&a, d->catalog_id), osa_sql_escape(&a, d->url),
        osa_sql_escape(&a, d->dest_path), osa_sql_escape(&a, d->status),
        (long long)d->total_bytes, (long long)d->received_bytes, osa_sql_escape(&a, d->sha256),
        osa_sql_escape(&a, d->error), osa_sql_escape(&a, d->started_at),
        osa_sql_escape(&a, d->finished_at));
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}
bool osa_store_download_update(osa_db *db, const osa_download *d, osa_error *err) {
    return osa_store_download_add(db, d, err);
}
bool osa_store_download_list(osa_db *db, osa_rs *rs, osa_error *err) {
    return osa_sql_query(db, "SELECT * FROM DownloadHistory ORDER BY started_at DESC LIMIT 200", rs, err);
}

/* ============================== transcriptions ============================== */
bool osa_store_audio_add(osa_db *db, const char *path, const char *name, int64_t bytes,
                         int64_t duration_ms, const char *fmt, const char *lang, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    char ts[32]; osa_iso_time(osa_now_ms(), ts);
    osa_buf_printf(&sql,
        "INSERT INTO AudioFiles(path,name,size_bytes,duration_ms,format,language,created_at)"
        " VALUES('%s','%s',%lld,%lld,'%s','%s','%s')"
        " ON CONFLICT(path) DO UPDATE SET duration_ms=excluded.duration_ms,"
        "size_bytes=excluded.size_bytes,language=excluded.language",
        osa_sql_escape(&a, path), osa_sql_escape(&a, name), (long long)bytes,
        (long long)duration_ms, osa_sql_escape(&a, fmt), osa_sql_escape(&a, lang), ts);
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_transcription_add(osa_db *db, const osa_transcription *t, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    osa_buf_printf(&sql,
        "INSERT OR REPLACE INTO Transcriptions(id,audio_path,model_id,language,source,text,"
        "corrected_text,confidence,duration_ms,proc_ms,status,created_at) VALUES("
        "'%s','%s','%s','%s','%s','%s','%s',%f,%lld,%lld,'%s','%s')",
        osa_sql_escape(&a, t->id), osa_sql_escape(&a, t->audio_path),
        osa_sql_escape(&a, t->model_id), osa_sql_escape(&a, t->language),
        osa_sql_escape(&a, t->source), osa_sql_escape(&a, t->text),
        osa_sql_escape(&a, t->corrected_text), t->confidence,
        (long long)t->duration_ms, (long long)t->proc_ms,
        osa_sql_escape(&a, t->status), osa_sql_escape(&a, t->created_at));
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_transcription_list(osa_db *db, osa_rs *rs, int limit, const char *q, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    if (limit <= 0) limit = 100;
    if (q && *q)
        osa_buf_printf(&sql,
            "SELECT * FROM Transcriptions WHERE text LIKE '%%%s%%' OR audio_path LIKE '%%%s%%'"
            " ORDER BY created_at DESC LIMIT %d",
            osa_sql_escape(&a, q), osa_sql_escape(&a, q), limit);
    else
        osa_buf_printf(&sql, "SELECT * FROM Transcriptions ORDER BY created_at DESC LIMIT %d", limit);
    bool ok = osa_sql_query(db, sql.data, rs, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

/* Deleting a transcript also removes the translations that point at it, so the
 * history never shows an orphaned row. */
bool osa_store_transcription_delete(osa_db *db, const char *id, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    const char *idq = osa_sql_escape(&a, id);
    osa_buf_printf(&sql,
        "DELETE FROM Translations WHERE transcription_id='%s';"
        "DELETE FROM UserCorrections WHERE transcription_id='%s';"
        "DELETE FROM Transcriptions WHERE id='%s';", idq, idq, idq);
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_translation_add(osa_db *db, const osa_translation *t, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    osa_buf_printf(&sql,
        "INSERT OR REPLACE INTO Translations(id,transcription_id,source_text,text,source_lang,"
        "target_lang,engine,created_at) VALUES('%s','%s','%s','%s','%s','%s','%s','%s')",
        osa_sql_escape(&a, t->id), osa_sql_escape(&a, t->transcription_id),
        osa_sql_escape(&a, t->source_text), osa_sql_escape(&a, t->text),
        osa_sql_escape(&a, t->source_lang), osa_sql_escape(&a, t->target_lang),
        osa_sql_escape(&a, t->engine), osa_sql_escape(&a, t->created_at));
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_translation_list(osa_db *db, osa_rs *rs, int limit, osa_error *err) {
    osa_buf sql; osa_buf_init(&sql);
    if (limit <= 0) limit = 100;
    osa_buf_printf(&sql, "SELECT * FROM Translations ORDER BY created_at DESC LIMIT %d", limit);
    bool ok = osa_sql_query(db, sql.data, rs, err);
    osa_buf_free(&sql);
    return ok;
}

bool osa_store_correction_add(osa_db *db, const char *transcription_id, const char *original,
                              const char *corrected, double confidence, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    char ts[32]; osa_iso_time(osa_now_ms(), ts);
    osa_buf_printf(&sql,
        "INSERT INTO UserCorrections(transcription_id,original,corrected,confidence,status,created_at)"
        " VALUES('%s','%s','%s',%f,'pending','%s')",
        osa_sql_escape(&a, transcription_id), osa_sql_escape(&a, original),
        osa_sql_escape(&a, corrected), confidence, ts);
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_correction_list(osa_db *db, osa_rs *rs, const char *status, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    if (status && *status)
        osa_buf_printf(&sql, "SELECT * FROM UserCorrections WHERE status='%s' ORDER BY id DESC",
                       osa_sql_escape(&a, status));
    else osa_buf_puts(&sql, "SELECT * FROM UserCorrections ORDER BY id DESC LIMIT 500");
    bool ok = osa_sql_query(db, sql.data, rs, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_correction_approve(osa_db *db, const char *id, bool approved, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    osa_buf_printf(&sql, "UPDATE UserCorrections SET status='%s' WHERE id=%s",
                   approved ? "approved" : "rejected", osa_sql_escape(&a, id));
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_log(osa_db *db, const char *level, const char *tag, const char *msg, osa_error *err) {
    osa_arena a; osa_arena_init(&a);
    osa_buf sql; osa_buf_init(&sql);
    char ts[32]; osa_iso_time(osa_now_ms(), ts);
    osa_buf_printf(&sql,
        "INSERT INTO SystemLogs(level,tag,msg,created_at) VALUES('%s','%s','%s','%s')",
        osa_sql_escape(&a, level), osa_sql_escape(&a, tag), osa_sql_escape(&a, msg), ts);
    bool ok = osa_sql_exec(db, sql.data, err);
    osa_buf_free(&sql);
    osa_arena_free(&a);
    return ok;
}

bool osa_store_logs_list(osa_db *db, osa_rs *rs, int limit, osa_error *err) {
    osa_buf sql; osa_buf_init(&sql);
    if (limit <= 0) limit = 200;
    osa_buf_printf(&sql, "SELECT * FROM SystemLogs ORDER BY id DESC LIMIT %d", limit);
    bool ok = osa_sql_query(db, sql.data, rs, err);
    osa_buf_free(&sql);
    return ok;
}

bool osa_store_stats(osa_db *db, int64_t *files, int64_t *transcriptions, int64_t *translations,
                     int64_t *models, int64_t *seconds_audio) {
    osa_rs rs; osa_error e;
    char sql[] =
        "SELECT (SELECT COUNT(*) FROM AudioFiles),(SELECT COUNT(*) FROM Transcriptions),"
        "(SELECT COUNT(*) FROM Translations),(SELECT COUNT(*) FROM Models),"
        "(SELECT COALESCE(SUM(duration_ms),0)/1000 FROM AudioFiles)";
    if (!osa_sql_query(db, sql, &rs, &e)) return false;
    *files = osa_rs_i64(&rs, 0, "1");
    *transcriptions = osa_rs_i64(&rs, 0, "2");
    *translations = osa_rs_i64(&rs, 0, "3");
    *models = osa_rs_i64(&rs, 0, "4");
    *seconds_audio = osa_rs_i64(&rs, 0, "5");
    osa_rs_free(&rs);
    return true;
}
