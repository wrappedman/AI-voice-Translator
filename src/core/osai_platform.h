/* osai_platform.h — filesystem, threads, network, system metrics.
 * Two implementations: platform/win32 (WinHTTP, Win32 threads) and core/posix. */
#ifndef OSAI_PLATFORM_H
#define OSAI_PLATFORM_H

#include "osai_common.h"

/* ---------------- paths ---------------- */
typedef enum {
    OSAI_DIR_ROOT = 0, OSAI_DIR_MODELS, OSAI_DIR_DATASETS, OSAI_DIR_DOWNLOADS,
    OSAI_DIR_DB, OSAI_DIR_LOGS, OSAI_DIR_LICENSES, OSAI_DIR_BACKUPS, OSAI_DIR_AUDIO,
    OSAI_DIR_EXPORTS, OSAI_DIR_CACHE, OSAI_DIR_CHECKPOINTS, OSAI_DIR_PACKS,
    OSAI_DIR_COUNT
} osa_dir_kind;

bool        osa_paths_init(void);
const char *osa_path(osa_dir_kind k);                    /* absolute, with trailing separator */
const char *osa_exe_dir(void);
void        osa_path_join(char *out, size_t cap, const char *a, const char *b);
const char *osa_path_basename(const char *p);
const char *osa_path_ext(const char *p);                 /* ".wav" or "" */
bool        osa_path_replace_ext(char *out, size_t cap, const char *path, const char *newext);
int64_t     osa_disk_free(const char *path);

/* ---------------- files ---------------- */
bool osa_file_exists(const char *path);
bool osa_dir_exists(const char *path);
int64_t osa_file_size(const char *path);
bool osa_mkdirs(const char *path);
/* Reads a whole file into `out`, which the caller must have initialised with
 * osa_buf_init(). Appends to whatever is already there. */
bool osa_file_read(const char *path, osa_buf *out);
bool osa_file_write(const char *path, const void *data, size_t n);
bool osa_file_write_atomic(const char *path, const void *data, size_t n); /* tmp + rename */
bool osa_file_append(const char *path, const void *data, size_t n);
bool osa_file_remove(const char *path);
bool osa_file_rename(const char *from, const char *to);
bool osa_file_copy(const char *from, const char *to);
int  osa_dir_scan(const char *dir, const char *ext_filter, char ***names, int max_names);
void osa_dir_free_list(char **names, int n);
int64_t osa_dir_size(const char *dir);
bool osa_dir_remove_recursive(const char *dir);
int64_t osa_file_mtime(const char *path);

/* ---------------- threads ---------------- */
typedef struct osa_mutex  { void *h; } osa_mutex;
typedef struct osa_thread { void *h; } osa_thread;
typedef void (*osa_thread_fn)(void *ud);

void osa_mutex_init(osa_mutex *m);
void osa_mutex_destroy(osa_mutex *m);
void osa_mutex_lock(osa_mutex *m);
void osa_mutex_unlock(osa_mutex *m);
bool osa_thread_start(osa_thread *t, osa_thread_fn fn, void *ud);
void osa_thread_join(osa_thread *t);
void osa_thread_detach(osa_thread *t);
void osa_thread_exit(void);
int  osa_cpu_count(void);

/* atomics helpers (C11 _Atomic is enough, but keep a portable pair) */
int  osa_atomic_inc(volatile int *p);
int  osa_atomic_load(const volatile int *p);

/* ---------------- network ---------------- */
typedef void (*osa_download_progress)(int64_t received, int64_t total, void *ud);

/* Resumable download. Writes to dest_path (appending when resume==true and a
 * .part file exists). Returns 0 on success, -1 on failure (err has details),
 * -2 when cancelled. */
int  osa_http_download(const char *url, const char *dest_path, bool resume,
                       osa_download_progress cb, void *ud, volatile int *cancel,
                       osa_error *err);
/* Small text fetch (used by the optional catalog refresh; 200 KB ceiling). */
int  osa_http_get_text(const char *url, osa_buf *out, int timeout_ms, osa_error *err);
/* True when the URL uses https and the platform can validate certificates. */
bool osa_http_has_tls(void);

/* ---------------- system metrics ---------------- */
typedef struct osa_sys_stats {
    float cpu_percent;          /* 0..100 */
    float ram_used_mb, ram_total_mb, ram_percent;
    float gpu_percent;          /* -1 when unknown */
    float vram_used_mb, vram_total_mb;
    char  gpu_name[128];
    float proc_cpu_percent;
    float proc_ram_mb;
} osa_sys_stats;

void osa_sys_stats_init(void);
void osa_sys_stats_sample(osa_sys_stats *out);

/* ---------------- desktop helpers ---------------- */
bool osa_open_in_explorer(const char *path);   /* opens folder/file with the OS shell */
bool osa_clipboard_set(const char *utf8);

/* Plays a short WAV file through the default output device. Playback is
 * asynchronous and a new call stops the previous one, so the UI thread never
 * waits for the sound. Returns false when playback is unavailable. */
bool osa_audio_play_file(const char *wav_path);
/* True while a file started by osa_audio_play_file is still playing. */
bool osa_audio_is_playing(void);
/* Stops the current playback, if any. */
void osa_audio_stop(void);
char *osa_clipboard_get(osa_arena *a);         /* NULL when empty/unavailable */

#endif
