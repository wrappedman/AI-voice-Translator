/* osai_common.h — shared basic types, arena, string utils, logging, hashing.
 * Portable C11. No third-party dependencies. */
#ifndef OSAI_COMMON_H
#define OSAI_COMMON_H

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define OSAI_VERSION        "1.2.0"
#define OSAI_APP_NAME       "OpenSpeech AI"
#define OSAI_APP_ID         "OpenSpeechAI"

/* ---------------- limits ---------------- */
/* Strict C11 does not define M_PI (mingw hides it without _USE_MATH_DEFINES),
 * so the codebase uses its own constant. */
#define OSAI_PI 3.14159265358979323846

#define OSAI_MAX_PATH       1024
#define OSAI_MAX_LINE       4096
#define OSAI_MAX_TEXT       65536
#define OSAI_MAX_MSG        512

/* ---------------- arena ---------------- */
typedef struct osa_arena_chunk osa_arena_chunk;
typedef struct osa_arena {
    osa_arena_chunk *head;
    size_t used, cap;
} osa_arena;

void  osa_arena_init(osa_arena *a);
void *osa_arena_alloc(osa_arena *a, size_t n);          /* 16-byte aligned, zeroed */
void *osa_arena_alloc_raw(osa_arena *a, size_t n);      /* 16-byte aligned, uninitialized */
char *osa_arena_strdup(osa_arena *a, const char *s);
char *osa_arena_strndup(osa_arena *a, const char *s, size_t n);
char *osa_arena_printf(osa_arena *a, const char *fmt, ...);
void  osa_arena_free(osa_arena *a);
size_t osa_arena_bytes(const osa_arena *a);

/* growable byte buffer (heap, reusable across frames) */
typedef struct osa_buf {
    char  *data;
    size_t len, cap;
    bool   oom;
} osa_buf;

void  osa_buf_init(osa_buf *b);
void  osa_buf_grow(osa_buf *b, size_t need);
void  osa_buf_free(osa_buf *b);
void  osa_buf_reset(osa_buf *b);
void  osa_buf_putn(osa_buf *b, const void *p, size_t n);
void  osa_buf_puts(osa_buf *b, const char *s);
void  osa_buf_putc(osa_buf *b, char c);
void  osa_buf_printf(osa_buf *b, const char *fmt, ...);
char *osa_buf_detach(osa_buf *b);                       /* NUL-terminated, caller frees */

/* ---------------- strings ---------------- */
bool osa_streq(const char *a, const char *b);
bool osa_strieq(const char *a, const char *b);
bool osa_starts(const char *s, const char *p);
bool osa_ends(const char *s, const char *p);
const char *osa_strstr_ci(const char *hay, const char *needle);
void osa_strlcpy(char *dst, const char *src, size_t cap);
char *osa_strdup(const char *s);
char *osa_strndup(const char *s, size_t n);
char *osa_trim_inplace(char *s);
int   osa_split(char *s, char sep, char **out, int maxout);   /* destructive, keeps empties */
char *osa_json_escape(osa_arena *a, const char *s);           /* returns "..." incl. quotes */
bool  osa_json_get_string(const char *json, const char *key, char *out, size_t cap);
/* Allocates an exact-size UTF-8 string, including JSON surrogate pairs. free(). */
char *osa_json_string_dup(const char *json, const char *key);
bool  osa_json_get_bool(const char *json, const char *key, bool *out);
bool  osa_json_get_num(const char *json, const char *key, double *out);

/* ---------------- utf-8 ---------------- */
typedef uint32_t osa_rune;
int  osa_utf8_decode(const char *s, size_t len, osa_rune *out);   /* returns bytes consumed, 0 at end */
int  osa_utf8_encode(osa_rune r, char out[4]);
bool osa_utf8_valid(const char *s, size_t len);
size_t osa_utf8_count(const char *s);
bool osa_rune_is_rtl(osa_rune r);        /* Arabic/Hebrew blocks incl. presentation forms */

/* ---------------- hashing / encoding ---------------- */
typedef struct osa_sha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t buflen; } osa_sha256;
void osa_sha256_init(osa_sha256 *c);
void osa_sha256_update(osa_sha256 *c, const void *data, size_t n);
void osa_sha256_final(osa_sha256 *c, uint8_t digest[32]);
void osa_sha256_hex(const void *data, size_t n, char out[65]);
void osa_sha256_file(const char *path, char out[65], uint64_t *size_out);

uint32_t osa_crc32(uint32_t seed, const void *data, size_t n);
uint64_t osa_hash64(const void *data, size_t n);
char *osa_hex(const void *data, size_t n, char *out);   /* out: n*2+1 */

/* base64 encode/decode (used by the Local API for +json bodies) */
size_t osa_base64_encode(const uint8_t *in, size_t n, char *out);
size_t osa_base64_decode(const char *in, size_t n, uint8_t *out);

/* ---------------- time / ids ---------------- */
int64_t osa_now_ms(void);               /* unix epoch ms */
/* Pin the wall clock. Used by the snapshot renderer so screenshots that show a
 * timestamp are reproducible; pass -1 to resume the real clock. */
void    osa_time_freeze(int64_t epoch_ms);
bool    osa_time_is_frozen(void);
int64_t osa_now_us(void);
void    osa_sleep_ms(int ms);
void    osa_iso_time(int64_t epoch_ms, char out[32]);
int64_t osa_iso_parse(const char *s);
void    osa_uuid(char out[37]);
void    osa_short_id(char out[13]);

/* ---------------- misc math ---------------- */
static inline float osa_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int   osa_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float osa_db_to_lin(float db);
float osa_lin_to_db(float lin);

/* ---------------- logging ---------------- */
typedef enum { OSAI_LOG_DEBUG, OSAI_LOG_INFO, OSAI_LOG_WARN, OSAI_LOG_ERROR } osai_log_level;

typedef void (*osa_log_sink)(osai_log_level lvl, const char *msg, void *ud);
void osa_log_set_sink(osa_log_sink sink, void *ud);
void osa_log_set_level(osai_log_level lvl);
void osa_log(osai_log_level lvl, const char *tag, const char *fmt, ...);
/* Privacy rule (§10): logs must never carry transcript content. Callers pass ids only. */
#define OSA_LOGD(tag, ...) osa_log(OSAI_LOG_DEBUG, tag, __VA_ARGS__)
#define OSA_LOGI(tag, ...) osa_log(OSAI_LOG_INFO,  tag, __VA_ARGS__)
#define OSA_LOGW(tag, ...) osa_log(OSAI_LOG_WARN,  tag, __VA_ARGS__)
#define OSA_LOGE(tag, ...) osa_log(OSAI_LOG_ERROR, tag, __VA_ARGS__)

/* ---------------- error containers ---------------- */
typedef struct osa_error {
    bool ok;
    char msg[OSAI_MAX_MSG];
} osa_error;

static inline void osa_err_ok(osa_error *e) { if (e) { e->ok = true; e->msg[0] = 0; } }
static inline void osa_err_set(osa_error *e, const char *fmt, ...);
static inline bool osa_err_fail(osa_error *e) { return e && !e->ok; }
static inline void osa_err_set(osa_error *e, const char *fmt, ...) {
    if (!e) return;
    e->ok = false;
    va_list ap; va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
}

#endif /* OSAI_COMMON_H */
