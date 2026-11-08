/* =====================================================================
 *  OpenSpeech AI — shell internals shared by shell.c and shell_screens.c
 *
 *  The history table is parsed by both the Transcribe screen's "recent" strip
 *  and the History screen, so the row table and its JSON visitor live here
 *  rather than being duplicated in the two translation units.
 * ===================================================================== */
#ifndef OSAI_SHELL_INTERNAL_H
#define OSAI_SHELL_INTERNAL_H

#include "shell.h"

typedef struct {
    char   id[40];
    char   text[512];
    char   lang[8];
    char   when[48];
    double conf;
    double dur;
} hist_row;

#define HIST_MAX 100
extern hist_row g_hist_rows[HIST_MAX];
extern int      g_hist_n;

typedef struct { int n; } hist_ud;

/* A JSON reader good enough for the arrays the app API returns. */
const char *shell_json_skip_ws(const char *p);
bool  shell_json_str(const char *obj, const char *end, const char *key,
                     char *out, size_t cap);
bool  shell_json_num(const char *obj, const char *end, const char *key, double *out);
int   shell_json_walk(const char *json,
                      void (*visit)(void *ud, const char *obj, const char *end),
                      void *ud);

/* Fills g_hist_rows from a history JSON array. Returns the row count. */
int   shell_parse_history(const char *json);

#endif /* OSAI_SHELL_INTERNAL_H */
