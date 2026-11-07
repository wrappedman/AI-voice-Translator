/* osai_store.h — SQLite persistence for every entity in §9 of the design.
 * Big payloads (audio, weights) live on the filesystem; the DB keeps paths +
 * metadata only, per the design rule. */
#ifndef OSAI_STORE_H
#define OSAI_STORE_H

#include "osai_common.h"

typedef struct osa_db osa_db;

osa_db *osa_db_open(const char *path, osa_error *err);
void    osa_db_close(osa_db *db);
bool    osa_db_migrate(osa_db *db, osa_error *err);
void    osa_db_set_setting(osa_db *db, const char *key, const char *value);
char   *osa_db_get_setting(osa_db *db, const char *key, osa_arena *a, const char *def);

/* ---- shared row helpers ------------------------------------------------- */
typedef struct osa_row {
    osa_arena arena;
    char *cols[40];
    int   ncols;
    char *values[40];
} osa_row;

typedef struct osa_rs {
    osa_arena arena;      /* owns strings in rows */
    osa_row  *rows;
    int       nrows, cap;
    int       ncols;
} osa_rs;

void osa_rs_free(osa_rs *rs);
const char *osa_rs_val(const osa_rs *rs, int row, const char *col);
int  osa_rs_int(const osa_rs *rs, int row, const char *col);
int64_t osa_rs_i64(const osa_rs *rs, int row, const char *col);
double osa_rs_dbl(const osa_rs *rs, int row, const char *col);

bool osa_sql_exec(osa_db *db, const char *sql, osa_error *err);
bool osa_sql_query(osa_db *db, const char *sql, osa_rs *rs, osa_error *err);
/* prepared-statement style helpers (escape-safe) */
char *osa_sql_escape(osa_arena *a, const char *s);

/* ---- entities ---------------------------------------------------------- */
typedef struct osa_model {
    char id[40], name[128], kind[24], engine[32], base_model[128];
    char source_url[512], license[128], sha256[72], path[OSAI_MAX_PATH];
    char version[32], status[16], notes[512], created_at[32];
    int64_t size_bytes;
} osa_model;

typedef struct osa_dataset {
    char id[40], name[160], language[16], source[256], license[128];
    char path[OSAI_MAX_PATH], sha256[72], created_at[32], notes[512];
    int64_t size_bytes, hours_x1000;   /* hours * 1000 to stay integer */
    int file_count, clip_count;
} osa_dataset;

typedef struct osa_training_job {
    char id[40], base_model_id[40], dataset_id[40], status[16], stage[32];
    char output_model_id[40], config_json[2048], created_at[32], finished_at[32];
    int epoch, total_epochs, step, total_steps;
    double progress, train_loss, val_loss, wer, cer, lr;
    int64_t started_ms, eta_ms;
    bool can_resume, can_stop;
} osa_training_job;

typedef struct osa_catalog_entry {
    char id[64], name[160], kind[24], language[16], url[512], license[160];
    char homepage[256], size_text[32], sha256[72], local_path[OSAI_MAX_PATH];
    char status[16], notes[400], min_free_gb[16];
    int64_t size_bytes, downloaded_bytes;
} osa_catalog_entry;

typedef struct osa_download {
    char id[40], catalog_id[64], url[512], dest_path[OSAI_MAX_PATH], status[16];
    char started_at[32], finished_at[32], error[256], sha256[72];
    int64_t total_bytes, received_bytes;
} osa_download;

typedef struct osa_transcription {
    char id[40], audio_path[OSAI_MAX_PATH], model_id[40], language[8], source[16];
    char text[8192], corrected_text[8192], created_at[32], status[16];
    double confidence;
    int64_t duration_ms, proc_ms;
} osa_transcription;

typedef struct osa_translation {
    char id[40], transcription_id[40], source_lang[8], target_lang[8], engine[32];
    char text[8192], source_text[8192], created_at[32];
} osa_translation;

/* models */
bool osa_store_model_add(osa_db *db, const osa_model *m, osa_error *err);
bool osa_store_model_list(osa_db *db, osa_rs *rs, const char *kind, osa_error *err);
bool osa_store_model_set_active(osa_db *db, const char *id, osa_error *err);
const char *osa_store_model_active(osa_db *db, const char *kind, osa_arena *a);
bool osa_store_model_delete(osa_db *db, const char *id, osa_error *err);

/* datasets */
bool osa_store_dataset_add(osa_db *db, const osa_dataset *d, osa_error *err);
bool osa_store_dataset_list(osa_db *db, osa_rs *rs, osa_error *err);

/* training */
bool osa_store_job_add(osa_db *db, const osa_training_job *j, osa_error *err);
bool osa_store_job_update(osa_db *db, const osa_training_job *j, osa_error *err);
bool osa_store_job_list(osa_db *db, osa_rs *rs, osa_error *err);
bool osa_store_metric_add(osa_db *db, const char *job_id, const char *name, double value,
                          int step, osa_error *err);

/* catalog + downloads */
bool osa_store_catalog_sync(osa_db *db, const osa_catalog_entry *entries, int n, osa_error *err);
bool osa_store_catalog_list(osa_db *db, osa_rs *rs, osa_error *err);
bool osa_store_catalog_update_status(osa_db *db, const char *id, const char *status,
                                     int64_t downloaded, osa_error *err);
bool osa_store_catalog_get(osa_db *db, const char *id, osa_catalog_entry *out, osa_error *err);
bool osa_store_download_add(osa_db *db, const osa_download *d, osa_error *err);
bool osa_store_download_update(osa_db *db, const osa_download *d, osa_error *err);
bool osa_store_download_list(osa_db *db, osa_rs *rs, osa_error *err);

/* transcriptions + translations + corrections + history */
bool osa_store_transcription_add(osa_db *db, const osa_transcription *t, osa_error *err);
bool osa_store_transcription_list(osa_db *db, osa_rs *rs, int limit, const char *q, osa_error *err);
bool osa_store_audio_add(osa_db *db, const char *path, const char *name, int64_t bytes,
                         int64_t duration_ms, const char *fmt, const char *lang, osa_error *err);
bool osa_store_translation_add(osa_db *db, const osa_translation *t, osa_error *err);
bool osa_store_translation_list(osa_db *db, osa_rs *rs, int limit, osa_error *err);
bool osa_store_correction_add(osa_db *db, const char *transcription_id, const char *original,
                              const char *corrected, double confidence, osa_error *err);
bool osa_store_correction_list(osa_db *db, osa_rs *rs, const char *status, osa_error *err);
bool osa_store_correction_approve(osa_db *db, const char *id, bool approved, osa_error *err);
bool osa_store_log(osa_db *db, const char *level, const char *tag, const char *msg, osa_error *err);
bool osa_store_logs_list(osa_db *db, osa_rs *rs, int limit, osa_error *err);
bool osa_store_stats(osa_db *db, int64_t *files, int64_t *transcriptions, int64_t *translations,
                     int64_t *models, int64_t *seconds_audio);

#endif
