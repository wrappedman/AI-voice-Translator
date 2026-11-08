/* =====================================================================
 *  OpenSpeech AI — JSON helpers and the history table
 * ===================================================================== */
#include "shell_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ---- JSON ---------------------------------------------------------- */

const char *shell_json_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Finds "key":value inside `obj` (bounded by `end`) and copies the value into
 * `out`. Strings are unescaped; numbers and booleans are copied verbatim. */
bool shell_json_str(const char *obj, const char *end, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    const char *p = obj;
    if (!cap) return false;
    out[0] = 0;
    while (p && p < end) {
        p = (const char *)strstr(p, key);
        if (!p || p >= end) return false;
        /* the match has to be a whole key: "key" must be followed by ':' */
        const char *q = shell_json_skip_ws(p + klen);
        if (*q != ':') { p += klen; continue; }
        q = shell_json_skip_ws(q + 1);
        size_t n = 0;
        if (*q != '"') {
            while (*q && *q != ',' && *q != '}' && *q != ']' && q < end && n + 1 < cap)
                out[n++] = *q++;
            while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\n')) n--;
            out[n] = 0;
            return n > 0;
        }
        q++;
        while (*q && *q != '"' && q < end) {
            if (*q == '\\' && q + 1 < end) {
                q++;
                char e = *q++;
                char c = e;
                if (e == 'n') c = '\n';
                else if (e == 't') c = '\t';
                else if (e == 'r') c = '\r';
                if (n + 1 < cap) out[n++] = c;
            } else if (n + 1 < cap) {
                out[n++] = *q++;
            } else {
                q++;
            }
        }
        out[n] = 0;
        return true;
    }
    return false;
}

bool shell_json_num(const char *obj, const char *end, const char *key, double *out) {
    char buf[64];
    if (!shell_json_str(obj, end, key, buf, sizeof buf)) return false;
    *out = atof(buf);
    return true;
}

/* Calls `visit` for every object in a top-level JSON array. String state is
 * tracked, so a brace inside a transcript cannot end the object early. */
int shell_json_walk(const char *json,
                    void (*visit)(void *ud, const char *obj, const char *end),
                    void *ud) {
    if (!json) return 0;
    const char *p = strchr(json, '[');
    if (!p) return 0;
    p++;
    int count = 0;
    while (*p) {
        p = shell_json_skip_ws(p);
        if (*p != '{' || !*p) break;
        const char *start = p;
        int depth = 0;
        bool in_str = false;
        while (*p) {
            char ch = *p;
            if (in_str) {
                if (ch == '\\') { p += 2; continue; }
                if (ch == '"') in_str = false;
            } else if (ch == '"') {
                in_str = true;
            } else if (ch == '{') {
                depth++;
            } else if (ch == '}') {
                depth--;
                if (depth == 0) { p++; break; }
            }
            p++;
        }
        visit(ud, start, p);
        count++;
        p = shell_json_skip_ws(p);
        if (*p == ',') p++;
    }
    return count;
}

/* The row table is shared with shell.c (the Transcribe screen's recent strip),
 * so it is defined here and declared in shell_internal.h. A fixed table keeps
 * the per-frame cost at zero: parsing a hundred rows allocates nothing. */
hist_row g_hist_rows[HIST_MAX];
int      g_hist_n;

static void hist_visit(void *ud, const char *o, const char *e) {
    hist_ud *hx = (hist_ud *)ud;
    if (hx->n >= HIST_MAX) return;
    hist_row *r = &g_hist_rows[hx->n++];
    memset(r, 0, sizeof *r);
    shell_json_str(o, e, "\"id\"", r->id, sizeof r->id);
    shell_json_str(o, e, "\"text\"", r->text, sizeof r->text);
    shell_json_str(o, e, "\"language\"", r->lang, sizeof r->lang);
    char iso[48] = {0};
    shell_json_str(o, e, "\"created_at\"", iso, sizeof iso);
    /* ISO-8601 "2026-01-01T10:00:00Z" → "01-01 10:00" */
    if (strlen(iso) >= 16) snprintf(r->when, sizeof r->when, "%.5s %.5s", iso + 5, iso + 11);
    else osa_strlcpy(r->when, iso, sizeof r->when);
    shell_json_num(o, e, "\"confidence\"", &r->conf);
    shell_json_num(o, e, "\"duration_s\"", &r->dur);
}


/* Fills the shared row table from a history array. */
int shell_parse_history(const char *json) {
    hist_ud hud = { 0 };
    g_hist_n = shell_json_walk(json, hist_visit, &hud);
    return g_hist_n;
}
