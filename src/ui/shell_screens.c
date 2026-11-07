/* =====================================================================
 *  OpenSpeech AI — the remaining screens
 *
 *  Live, Translate, History, Models, Training, Evaluation, Settings, About.
 *  Each screen is a plain function that owns its own layout; the shell picks
 *  one per frame. Keeping them in a separate translation unit makes it easy to
 *  see which screen owns which state.
 * ===================================================================== */
#include "shell.h"
#include "../core/osai_platform.h"
#include "../platform/mic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef MAXF
#define MAXF(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MINF
#define MINF(a,b) ((a) < (b) ? (a) : (b))
#endif

#define APP_VERSION "1.0.0"
osa_rect shell_draw_chrome(shell_state *s, ui_context *u, osa_rect content,
                           const char *title_key, const char *subtitle_key);
void shell_draw_toast(shell_state *s, ui_context *u, osa_rect body);

/* ---- helpers ------------------------------------------------------ */

/* A tiny JSON reader good enough for the arrays the app API returns. It walks
 * the object array and hands each object's raw text to a visitor. */
static const char *json_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Finds "key":"value" inside `obj` (bounded by `end`) and copies the unescaped
 * value into `out`. Returns false when the key is absent. */
static bool json_str(const char *obj, const char *end, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    const char *p = obj;
    while (p && p < end) {
        p = (const char *)strstr(p, key);
        if (!p || p >= end) return false;
        /* reject matches that are not a whole key: "key" must be followed by : */
        const char *q = json_skip_ws(p + kl);
        if (*q != ':') { p += kl; continue; }
        q = json_skip_ws(q + 1);
        size_t n = 0;
        if (*q != '"') {
            /* bare literal: true / false / null / a number */
            while (*q && *q != ',' && *q != '}' && *q != ']' && q < end && n + 1 < cap)
                out[n++] = *q++;
            while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\n')) n--;
            out[n] = 0;
            return n > 0;
        }
        q++;
        while (*q && *q != '"' && q < end) {
            if (*q == '\\' && q[1]) {
                q++;
                char ch = *q++;
                if (ch == 'n') ch = '\n';
                else if (ch == 't') ch = '\t';
                else if (ch == 'r') { ch = '\r'; }
                else if (ch == 'u') { q += 4; ch = '?'; }
                if (n + 1 < cap) out[n++] = ch;
            } else {
                if (n + 1 < cap) out[n++] = *q;
                q++;
            }
        }
        out[n] = 0;
        return true;
    }
    return false;
}

static bool json_num(const char *obj, const char *end, const char *key, double *out) {
    char buf[64];
    if (!json_str(obj, end, key, buf, sizeof buf)) {
        /* numeric values are unquoted; retry without the quote requirement */
        size_t kl = strlen(key);
        const char *p = obj;
        while (p && p < end) {
            p = (const char *)strstr(p, key);
            if (!p || p >= end) return false;
            const char *q = json_skip_ws(p + kl);
            if (*q != ':') { p += kl; continue; }
            q = json_skip_ws(q + 1);
            *out = atof(q);
            return true;
        }
        return false;
    }
    *out = atof(buf);
    return true;
}

/* Iterates the objects inside the top-level array of `json`. `visit` receives
 * the object's start and one-past-end pointers. */
static int json_each_object(const char *json, void (*visit)(void *ud, const char *obj,
                                                            const char *end), void *ud) {
    if (!json) return 0;
    const char *p = strchr(json, '[');
    if (!p) return 0;
    int depth = 1;
    const char *obj_start = NULL;
    int obj_depth = 0;
    int n = 0;
    for (p++; *p; p++) {
        if (*p == '"') {                       /* skip strings so brackets inside them don't count */
            p++;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
            if (!*p) break;
            continue;
        }
        if (*p == '[' || *p == '{') {
            depth++;
            if (*p == '{' && obj_depth == 0 && depth == 2) { obj_start = p; obj_depth = depth; }
        } else if (*p == ']' || *p == '}') {
            if (*p == '}' && obj_start && depth == obj_depth) {
                if (visit) visit(ud, obj_start, p + 1);
                n++;
                obj_start = NULL;
                obj_depth = 0;
            }
            depth--;
            if (depth == 0) break;
        }
    }
    return n;
}

/* ---- shared: stat row -------------------------------------------- */

static void draw_stat_card(ui_context *u, osa_rect r, const char *label, const char *value,
                           osa_color value_col) {
    ui_card(u, r);
    ui_label(u, rectf(r.x + 14 * u->L.scale, r.y + 12 * u->L.scale,
                      r.w - 28 * u->L.scale, 16 * u->L.scale),
             label, 10.5f * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
    ui_label(u, rectf(r.x + 14 * u->L.scale, r.y + 30 * u->L.scale,
                      r.w - 28 * u->L.scale, 26 * u->L.scale),
             value, 17 * u->L.scale, value_col, u->L.rtl ? 2 : 0);
}

/* ---- Live --------------------------------------------------------- */

typedef struct live_state {
    osa_mic  *mic;
    asr_stream *stream;
    float    *ring;
    int       ring_n;
    int       ring_cap;
    char      partial[4096];
    char      final_text[8192];
    char      translation[8192];
    double    started;
} live_state;

static live_state g_live;

static void live_stop(shell_state *s) {
    if (g_live.stream) {
        asr_result r;
        if (asr_stream_finish(g_live.stream, &r)) {
            osa_strlcpy(g_live.final_text, r.text, sizeof g_live.final_text);
            if (g_live.final_text[0]) {
                osa_error err;
                osa_app_translation mt;
                if (osa_app_translate(s->app, g_live.final_text, NULL, &mt, &err))
                    osa_strlcpy(g_live.translation, mt.text, sizeof g_live.translation);
                /* persist the live transcript so it shows up in the history */
                osa_app_transcript tr;
                osa_app_transcribe_and_translate(s->app, NULL, &tr, &mt, &err);
            }
        }
        asr_stream_free(g_live.stream);
        g_live.stream = NULL;
    }
    if (g_live.mic) { osa_mic_close(g_live.mic); g_live.mic = NULL; }
    s->live = false;
}

static void draw_live(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 20 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);

    /* hero: big record button + level ring. The ring's outer dot reaches
     * R*1.28+4.2 below the centre, so the caption needs its own band. */
    float hero_h = 248 * u->L.scale;
    osa_rect hero = rectf(inner.x, inner.y, inner.w, hero_h);
    ui_card(u, hero);
    float cx = hero.x + hero.w * 0.5f;
    float cy = hero.y + hero_h * 0.5f - 6 * u->L.scale;
    float R = 52 * u->L.scale;

    if (s->live) {
        /* concentric pulse rings driven by the frame counter rather than time,
         * so snapshots stay deterministic */
        for (int i = 0; i < 3; i++) {
            float t = fmodf((float)(s->frame * 0.02f) + i / 3.0f, 1.0f);
            float rr = R * (1.0f + t * 0.75f);
            c_stroke_round(u->c, rectf(cx - rr, cy - rr, rr * 2, rr * 2), rr,
                           2.0f * (1.0f - t) + 0.5f, col_alpha(u->t.danger, 0.35f * (1.0f - t)));
        }
    }
    c_circle(u->c, cx, cy, R, s->live ? col_alpha(u->t.danger, 0.20f) : u->t.panel_alt);
    c_circle(u->c, cx, cy, R * 0.66f, s->live ? u->t.danger : u->t.accent);

    /* level meter around the button */
    int nseg = 28;
    for (int i = 0; i < nseg; i++) {
        float a = (float)i / nseg * 6.2831853f - 1.5707963f;
        bool on = s->live && (s->live_level > (float)i / nseg);
        float rr = R * 1.28f;
        float x = cx + cosf(a) * rr, y = cy + sinf(a) * rr;
        c_circle(u->c, x, y, on ? 4.2f * u->L.scale : 3.0f * u->L.scale,
                 on ? u->t.success : col_alpha(u->t.border, 1.0f));
    }
    osa_rect btn = rectf(cx - R, cy - R, R * 2, R * 2);
    if (ui_hit(u, btn) && u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        if (!s->live) {
            osa_error err;
            if (!osa_mic_available()) {
                shell_set_message(s, true, tt(s->lang, "msg.nomic"));
            } else {
                g_live.mic = osa_mic_open(16000, &err);
                if (!g_live.mic) {
                    shell_set_message(s, true, "%s", err.msg);
                } else {
                    g_live.stream = asr_stream_start(NULL, NULL, &err);
                    g_live.final_text[0] = g_live.translation[0] = g_live.partial[0] = 0;
                    s->live = true;
                    shell_set_message(s, false, "%s", tt(s->lang, "live.silence"));
                }
            }
        } else {
            live_stop(s);
            shell_set_message(s, false, "%s", tt(s->lang, "msg.ok"));
        }
    }

    ui_label(u, rectf(hero.x + 20 * u->L.scale, hero.y + hero_h - 54 * u->L.scale,
                      hero.w - 40 * u->L.scale, 22 * u->L.scale),
             s->live ? tt(s->lang, "btn.stop") : tt(s->lang, "btn.start"),
             13 * u->L.scale, s->live ? u->t.danger : u->t.text, 1);
    char tm[128];
    char raw[48];
    snprintf(raw, sizeof raw, "%.1f s", s->live_time);
    text_localize_digits(raw, s->lang == UI_FA ? "fa" : "en", tm, sizeof tm);
    ui_label(u, rectf(hero.x + 20 * u->L.scale, hero.y + hero_h - 30 * u->L.scale,
                      hero.w - 40 * u->L.scale, 18 * u->L.scale),
             tm, 11 * u->L.scale, u->t.text_faint, 1);

    /* text panes */
    float top = hero.y + hero_h + 14 * u->L.scale;
    float h = inner.y + inner.h - top;
    float col_w = (inner.w - 14 * u->L.scale) * 0.5f;
    osa_rect a = rectf(u->L.rtl ? inner.x + col_w + 14 * u->L.scale : inner.x, top, col_w, h);
    osa_rect b = rectf(u->L.rtl ? inner.x : inner.x + col_w + 14 * u->L.scale, top, col_w, h);
    const char *txt[2] = { g_live.final_text[0] ? g_live.final_text : g_live.partial,
                           g_live.translation };
    const char *lbl[2] = { "transcribe.source", "transcribe.result" };
    osa_rect *pane[2] = { &a, &b };
    for (int i = 0; i < 2; i++) {
        ui_card(u, *pane[i]);
        ui_label(u, rectf(pane[i]->x + 14 * u->L.scale, pane[i]->y + 12 * u->L.scale,
                          pane[i]->w - 28 * u->L.scale, 18 * u->L.scale),
                 tt(s->lang, lbl[i]), 11 * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
        osa_rect tr = rectf(pane[i]->x + 14 * u->L.scale, pane[i]->y + 38 * u->L.scale,
                            pane[i]->w - 28 * u->L.scale, pane[i]->h - 52 * u->L.scale);
        if (txt[i][0]) {
            text_opts o = text_default_opts(15 * u->L.scale);
            o.max_width = tr.w;
            o.line_height = 15 * u->L.scale * 1.7f;
            o.rtl = (i == 0);
            text_draw_box(u->c, u->reg, 15 * u->L.scale, u->t.text, tr, txt[i], o);
        } else {
            ui_label(u, rectf(tr.x, tr.y, tr.w, 20 * u->L.scale), "—", 13 * u->L.scale,
                     u->t.text_faint, 1);
        }
    }
}

void shell_live_tick(shell_state *s, double dt) {
    if (!s->live || !g_live.mic) return;
    s->live_time += dt;
    float buf[1600];
    int n = osa_mic_read(g_live.mic, buf, 1600);
    s->live_level = osa_mic_level(g_live.mic);
    if (n > 0 && g_live.stream) {
        asr_stream_feed(g_live.stream, buf, n);
        const char *p = asr_stream_partial(g_live.stream);
        if (p) osa_strlcpy(g_live.partial, p, sizeof g_live.partial);
    }
}

/* ---- Translate ---------------------------------------------------- */

static char g_tr_in[4096] = "سلام، حال شما خوب است؟";
static char g_tr_out[4096];
static char g_tr_lang[8] = "fa";
static double g_tr_score;

static void draw_translate(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 20 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);
    float col_gap = 14 * u->L.scale;
    float top_h = inner.h * 0.5f - col_gap * 0.5f;
    float col_w = (inner.w - col_gap) * 0.5f;

    osa_rect in_r = rectf(u->L.rtl ? inner.x + col_w + col_gap : inner.x, inner.y, col_w, top_h);
    osa_rect out_r = rectf(u->L.rtl ? inner.x : inner.x + col_w + col_gap, inner.y, col_w, top_h);

    ui_card(u, in_r);
    ui_label(u, rectf(in_r.x + 14 * u->L.scale, in_r.y + 12 * u->L.scale,
                      in_r.w - 28 * u->L.scale, 18 * u->L.scale),
             tt(s->lang, "translate.input"), 11 * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
    osa_rect edit = rectf(in_r.x + 14 * u->L.scale, in_r.y + 38 * u->L.scale,
                          in_r.w - 28 * u->L.scale, in_r.h - 100 * u->L.scale);
    c_fill_round(u->c, edit, 8 * u->L.scale, u->t.panel_alt);
    if (ui_text_area(u, 3000, edit, g_tr_in, sizeof g_tr_in, 73)) {
        osa_error err;
        osa_app_translation mt;
        if (osa_app_translate(s->app, g_tr_in, NULL, &mt, &err)) {
            osa_strlcpy(g_tr_out, mt.text, sizeof g_tr_out);
            osa_strlcpy(g_tr_lang, mt.target_lang, sizeof g_tr_lang);
            g_tr_score = mt.confidence;
        }
    }
    /* action row */
    float by = in_r.y + in_r.h - 46 * u->L.scale;
    if (ui_button(u, 3010, rectf(in_r.x + 14 * u->L.scale, by, 110 * u->L.scale,
                                 32 * u->L.scale), tt(s->lang, "btn.run"), true)) {
        osa_error err;
        osa_app_translation mt;
        if (osa_app_translate(s->app, g_tr_in, NULL, &mt, &err)) {
            osa_strlcpy(g_tr_out, mt.text, sizeof g_tr_out);
            osa_strlcpy(g_tr_lang, mt.target_lang, sizeof g_tr_lang);
            g_tr_score = mt.confidence;
        } else {
            shell_set_message(s, true, "%s", err.msg);
        }
    }
    if (ui_button(u, 3011, rectf(in_r.x + 132 * u->L.scale, by, 100 * u->L.scale,
                                 32 * u->L.scale), tt(s->lang, "btn.copy"), false))
        osa_clipboard_set(g_tr_in);

    ui_card(u, out_r);
    ui_label(u, rectf(out_r.x + 14 * u->L.scale, out_r.y + 12 * u->L.scale,
                      out_r.w - 28 * u->L.scale, 18 * u->L.scale),
             tt(s->lang, "translate.output"), 11 * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
    osa_rect orr = rectf(out_r.x + 14 * u->L.scale, out_r.y + 38 * u->L.scale,
                         out_r.w - 28 * u->L.scale, out_r.h - 62 * u->L.scale);
    c_fill_round(u->c, orr, 8 * u->L.scale, col_alpha(u->t.accent, 0.06f));
    if (g_tr_out[0]) {
        text_opts o = text_default_opts(16 * u->L.scale);
        o.max_width = orr.w - 24 * u->L.scale;
        o.line_height = 16 * u->L.scale * 1.7f;
        o.rtl = osa_starts(g_tr_lang, "fa");
        text_draw_box(u->c, u->reg, 16 * u->L.scale, u->t.text,
                      rect_inset(orr, 12 * u->L.scale), g_tr_out, o);
        char meta[96], loc[128];
        snprintf(meta, sizeof meta, "%s · %.0f%%", osa_starts(g_tr_lang, "fa") ? "فارسی" : "English",
                 g_tr_score * 100.0);
        text_localize_digits(meta, s->lang == UI_FA ? "fa" : "en", loc, sizeof loc);
        ui_label(u, rectf(out_r.x + 14 * u->L.scale, out_r.y + out_r.h - 22 * u->L.scale,
                          out_r.w - 28 * u->L.scale, 16 * u->L.scale),
                 loc, 10 * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
    }
    if (ui_button(u, 3012, rectf(out_r.x + 14 * u->L.scale, out_r.y + out_r.h - 46 * u->L.scale,
                                 100 * u->L.scale, 32 * u->L.scale),
                  tt(s->lang, "btn.copy"), false))
        osa_clipboard_set(g_tr_out);

    /* bottom: phrasebook coverage note */
    osa_rect note = rectf(inner.x, inner.y + top_h + col_gap * 2, inner.w,
                          inner.h - top_h - col_gap * 2);
    if (note.h > 60 * u->L.scale) {
        ui_card(u, note);
        ui_label(u, rectf(note.x + 16 * u->L.scale, note.y + 14 * u->L.scale,
                          note.w - 32 * u->L.scale, 20 * u->L.scale),
                 tt(s->lang, "translate.note"), 11.5f * u->L.scale, u->t.text_dim,
                 u->L.rtl ? 2 : 0);
        ui_label(u, rectf(note.x + 16 * u->L.scale, note.y + 36 * u->L.scale,
                          note.w - 32 * u->L.scale, 18 * u->L.scale),
                 tt(s->lang, "about.note"), 10.5f * u->L.scale, u->t.text_faint,
                 u->L.rtl ? 2 : 0);
    }
}

/* ---- History ------------------------------------------------------ */

typedef struct hist_rows {
    char text[200][256];
    char lang[200][8];
    char when[200][32];
    char id[200][40];
    char audio[200][OSAI_MAX_PATH];
    double conf[200];
    double dur[200];
    int n;
} hist_rows;

static void hist_visit(void *ud, const char *o, const char *e) {
    hist_rows *h = (hist_rows *)ud;
    if (h->n >= 200) return;
    int i = h->n;
    if (!json_str(o, e, "\"text\"", h->text[i], sizeof h->text[i])) h->text[i][0] = 0;
    if (!json_str(o, e, "\"language\"", h->lang[i], sizeof h->lang[i])) h->lang[i][0] = 0;
    if (!json_str(o, e, "\"created_at\"", h->when[i], sizeof h->when[i])) h->when[i][0] = 0;
    if (!json_str(o, e, "\"id\"", h->id[i], sizeof h->id[i])) h->id[i][0] = 0;
    if (!json_str(o, e, "\"audio\"", h->audio[i], sizeof h->audio[i])) h->audio[i][0] = 0;
    json_num(o, e, "\"confidence\"", &h->conf[i]);
    json_num(o, e, "\"duration_ms\"", &h->dur[i]);
    h->n++;
}

/* Short form of a stored ISO timestamp for list rows: "11-07 13:29".
 * A full "2026-11-07 13:29:43" does not fit in a 62 px row. Anything that does
 * not look like an ISO timestamp is copied through unchanged. */
static void ui_fmt_when(const char *iso, char *out, size_t cap) {
    size_t n = iso ? strlen(iso) : 0;
    if (n >= 16 && iso[4] == '-' && iso[7] == '-' && (iso[10] == ' ' || iso[10] == 'T'))
        snprintf(out, cap, "%.5s %.5s", iso + 5, iso + 11);
    else
        snprintf(out, cap, "%s", iso ? iso : "");
}

static void draw_history(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 20 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);
    if (!s->history_json) s->history_json = osa_app_history_json(s->app, 100, NULL);

    static hist_rows hr;
    memset(&hr, 0, sizeof hr);
    json_each_object(s->history_json, hist_visit, &hr);

    bool wide = u->L.win_w > 1100 * u->L.scale;
    float list_w = wide ? inner.w * 0.58f : inner.w;
    osa_rect list = rectf(inner.x, inner.y, list_w, inner.h);
    ui_card(u, list);
    if (hr.n == 0) {
        ui_label(u, rect_inset(list, 24 * u->L.scale), tt(s->lang, "history.empty"),
                 13 * u->L.scale, u->t.text_faint, 1);
        return;
    }
    float row_h = 62 * u->L.scale;
    float y = list.y + 10 * u->L.scale + s->history_scroll * row_h;
    for (int i = 0; i < hr.n; i++) {
        osa_rect row = rectf(list.x + 8 * u->L.scale, y, list.w - 16 * u->L.scale, row_h - 4);
        if (row.y + row.h > list.y + list.h) break;
        if (row.y + row.h < list.y) { y += row_h; continue; }
        if (ui_row(u, 4200 + i, row, s->history_sel == i)) {
            s->history_sel = i;
            osa_strlcpy(s->audio_path, hr.audio[i], sizeof s->audio_path);
            osa_strlcpy(s->transcript, hr.text[i], sizeof s->transcript);
            osa_strlcpy(s->detected_lang, hr.lang[i], sizeof s->detected_lang);
            s->transcript_conf = (float)hr.conf[i];
            s->transcript_dur = hr.dur[i] / 1000.0;
            snprintf(s->translation, sizeof s->translation, "%s",
                     osa_app_translate_preview(s->app, hr.text[i]));
            shell_load_waveform(s, s->audio_path);
        }
        osa_rect tr = rectf(row.x + 12 * u->L.scale, row.y + 9 * u->L.scale,
                            row.w - 24 * u->L.scale, 20 * u->L.scale);
        ui_label(u, tr, hr.text[i], 12.5f * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
        char meta[128], loc[160];
        char durs[32];
        char when[32];
        ui_fmt_duration(hr.dur[i] / 1000.0, durs, sizeof durs);
        ui_fmt_when(hr.when[i], when, sizeof when);
        snprintf(meta, sizeof meta, "%s · %s · %.0f%%", when, durs, hr.conf[i] * 100.0);
        text_localize_digits(meta, s->lang == UI_FA ? "fa" : "en", loc, sizeof loc);
        ui_label(u, rectf(row.x + 12 * u->L.scale, row.y + 31 * u->L.scale,
                          row.w - 24 * u->L.scale, 18 * u->L.scale),
                 loc, 10 * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
        y += row_h;
    }
    ui_scrollbar(u, rectf(list.x + list.w - 6 * u->L.scale, list.y + 6 * u->L.scale,
                          4 * u->L.scale, list.h - 12 * u->L.scale),
                 (float)hr.n * row_h, (float)list.h, (float)s->history_scroll * row_h);

    /* detail pane on the right */
    if (wide) {
        osa_rect det = rectf(list.x + list.w + 14 * u->L.scale, inner.y,
                             inner.w - list.w - 14 * u->L.scale, inner.h);
        ui_card(u, det);
        int i = s->history_sel < 0 ? 0 : s->history_sel;
        if (i < hr.n) {
            ui_label(u, rectf(det.x + 16 * u->L.scale, det.y + 12 * u->L.scale,
                              det.w - 32 * u->L.scale, 20 * u->L.scale),
                     tt(s->lang, "transcribe.source"), 11 * u->L.scale, u->t.text_dim,
                     u->L.rtl ? 2 : 0);
            float pane_h = det.h - 170 * u->L.scale;
            if (pane_h < 120 * u->L.scale) pane_h = 120 * u->L.scale;
            text_opts o = text_default_opts(14 * u->L.scale);
            o.max_width = det.w - 32 * u->L.scale;
            o.line_height = 14 * u->L.scale * 1.8f;
            o.rtl = osa_starts(hr.lang[i], "fa");
            text_draw_box(u->c, u->reg, 14 * u->L.scale, u->t.text,
                          rectf(det.x + 16 * u->L.scale, det.y + 38 * u->L.scale,
                                det.w - 32 * u->L.scale, pane_h * 0.5f - 46 * u->L.scale),
                          hr.text[i], o);

            float second = det.y + 54 * u->L.scale + pane_h * 0.5f;
            c_hline(u->c, det.x + 16 * u->L.scale, det.x + det.w - 16 * u->L.scale,
                    second - 10 * u->L.scale, 1, u->t.border_soft);
            ui_label(u, rectf(det.x + 16 * u->L.scale, second,
                              det.w - 32 * u->L.scale, 20 * u->L.scale),
                     tt(s->lang, "transcribe.result"), 11 * u->L.scale, u->t.text_dim,
                     u->L.rtl ? 2 : 0);
            const char *tr = osa_app_translate_preview(s->app, hr.text[i]);
            text_opts o2 = text_default_opts(14 * u->L.scale);
            o2.max_width = det.w - 32 * u->L.scale;
            o2.line_height = 14 * u->L.scale * 1.8f;
            o2.rtl = osa_starts(hr.lang[i], "en");
            text_draw_box(u->c, u->reg, 14 * u->L.scale, u->t.accent,
                          rectf(det.x + 16 * u->L.scale, second + 26 * u->L.scale,
                                det.w - 32 * u->L.scale, pane_h * 0.5f - 60), tr, o2);

            float by = det.y + det.h - 48 * u->L.scale;
            if (ui_button(u, 4000, rectf(det.x + 16 * u->L.scale, by, 100 * u->L.scale,
                                         32 * u->L.scale), tt(s->lang, "btn.delete"), false)) {
                osa_error err;
                if (osa_app_delete_transcript(s->app, hr.id[i], &err)) {
                    free(s->history_json);
                    s->history_json = osa_app_history_json(s->app, 100, NULL);
                    shell_set_message(s, false, "%s", tt(s->lang, "msg.ok"));
                }
            }
            if (ui_button(u, 4001, rectf(det.x + 124 * u->L.scale, by, 100 * u->L.scale,
                                         32 * u->L.scale), tt(s->lang, "btn.copy"), false))
                osa_clipboard_set(hr.text[i]);
        }
    }
    /* wheel scroll */
    if (rect_has(rect_inset(body, 0), u->in.mx, u->in.my) && u->in.wheel) {
        s->history_scroll -= u->in.wheel;
        int maxs = hr.n - (int)(list.h / (62 * u->L.scale));
        if (s->history_scroll < 0) s->history_scroll = 0;
        if (maxs < 0) maxs = 0;
        if (s->history_scroll > maxs) s->history_scroll = maxs;
    }
}

/* ---- Models ------------------------------------------------------- */

typedef struct model_row {
    char id[64];
    char name[160];
    char kind[8];
    char engine[32];
    double bytes;
    bool active;
} model_row;

typedef struct model_rows {
    model_row row[16];
    int n;
} model_rows;

static void model_visit(void *ud, const char *o, const char *e) {
    model_rows *mr = (model_rows *)ud;
    if (mr->n >= 16) return;
    model_row *r = &mr->row[mr->n];
    memset(r, 0, sizeof *r);
    json_str(o, e, "\"id\"", r->id, sizeof r->id);
    json_str(o, e, "\"name\"", r->name, sizeof r->name);
    json_str(o, e, "\"kind\"", r->kind, sizeof r->kind);
    json_str(o, e, "\"engine\"", r->engine, sizeof r->engine);
    json_num(o, e, "\"bytes\"", &r->bytes);
    char act[16] = {0};
    if (json_str(o, e, "\"active\"", act, sizeof act)) r->active = osa_starts(act, "true");
    mr->n++;
}

static void draw_models(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 20 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);
    if (!s->models_json) s->models_json = osa_app_models_json(s->app, NULL);

    static model_rows mr;
    memset(&mr, 0, sizeof mr);
    json_each_object(s->models_json, model_visit, &mr);
    if (mr.n == 0) {
        ui_card(u, inner);
        ui_label(u, rect_inset(inner, 24 * u->L.scale), tt(s->lang, "models.empty"),
                 12 * u->L.scale, u->t.text_faint, 1);
        return;
    }

    float card_h = 100 * u->L.scale;
    float gap = 12 * u->L.scale;
    for (int i = 0; i < mr.n; i++) {
        model_row *m = &mr.row[i];
        osa_rect r = rectf(inner.x, inner.y + i * (card_h + gap), inner.w, card_h);
        if (r.y + r.h > inner.y + inner.h - 50 * u->L.scale) break;
        ui_card(u, r);

        float badge = 42 * u->L.scale;
        float bx = u->L.rtl ? r.x + r.w - badge - 18 * u->L.scale : r.x + 18 * u->L.scale;
        osa_rect br = rectf(bx, r.y + (card_h - badge) * 0.5f, badge, badge);
        osa_color tint = osa_streq(m->kind, "mt") ? u->t.accent2 : u->t.accent;
        c_fill_round(u->c, br, badge * 0.28f, col_alpha(tint, 0.15f));
        c_circle(u->c, br.x + badge * 0.5f, br.y + badge * 0.5f, badge * 0.19f, tint);

        float tw = r.w - badge - 224 * u->L.scale;
        float tx = u->L.rtl ? br.x - 14 * u->L.scale - tw : br.x + badge + 14 * u->L.scale;
        ui_label(u, rectf(tx, r.y + 16 * u->L.scale, tw, 22 * u->L.scale),
                 m->name, 14 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);

        char sz[40];
        ui_fmt_bytes(m->bytes, sz, sizeof sz);
        char sub[220];
        snprintf(sub, sizeof sub, "%s · %s · %s · %s", m->kind[0] ? m->kind : "model",
                 m->engine[0] ? m->engine : "builtin", m->id, sz);
        ui_label(u, rectf(tx, r.y + 40 * u->L.scale, tw, 18 * u->L.scale),
                 sub, 10.5f * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);

        if (m->active) {
            osa_rect chip = rectf(u->L.rtl ? r.x + 18 * u->L.scale
                                           : r.x + r.w - 128 * u->L.scale,
                                  r.y + (card_h - 26 * u->L.scale) * 0.5f,
                                  110 * u->L.scale, 26 * u->L.scale);
            ui_status_chip(u, chip, tt(s->lang, "models.active"), u->t.success);
        } else {
            osa_rect btn = rectf(u->L.rtl ? r.x + 18 * u->L.scale
                                          : r.x + r.w - 128 * u->L.scale,
                                 r.y + (card_h - 30 * u->L.scale) * 0.5f,
                                 110 * u->L.scale, 30 * u->L.scale);
            if (ui_button(u, 5100 + i, btn, tt(s->lang, "btn.activate"), false)) {
                osa_error err;
                if (osa_app_activate_model(s->app, m->id, &err)) {
                    free(s->models_json);
                    s->models_json = osa_app_models_json(s->app, NULL);
                    shell_set_message(s, false, "%s", tt(s->lang, "msg.ok"));
                } else {
                    shell_set_message(s, true, "%s: %s", tt(s->lang, "msg.failed"), err.msg);
                }
            }
        }
    }

    float fy = inner.y + inner.h - 38 * u->L.scale;
    if (ui_button(u, 5000, rectf(inner.x, fy, 152 * u->L.scale, 34 * u->L.scale),
                  tt(s->lang, "models.import"), false)) {
        s->request = 2;
    }
    ui_label(u, rectf(inner.x + 168 * u->L.scale, fy + 6 * u->L.scale,
                      inner.w - 168 * u->L.scale, 22 * u->L.scale),
             tt(s->lang, "models.note"), 10.5f * u->L.scale, u->t.text_faint,
             u->L.rtl ? 2 : 0);
}

/* ---- Training ----------------------------------------------------- */

static int g_epochs = 8;
static int g_train_lang = 0;   /* 0 = fa, 1 = en */
static bool g_use_gpu = false;

static void draw_training(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 20 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);
    float card_h = MINF(inner.h * 0.56f, 340 * u->L.scale);
    osa_rect card = rectf(inner.x, inner.y, inner.w, card_h);
    ui_card(u, card);

    float lx = card.x + 22 * u->L.scale;                 /* label edge   */
    float rx = card.x + card.w * 0.46f;                  /* control edge */
    float ctl_w = card.w * 0.40f;
    float y = card.y + 20 * u->L.scale;
    float row_h = 52 * u->L.scale;

    osa_app_status st;
    osa_app_get_status(s->app, &st);
    char line[256];
    snprintf(line, sizeof line, "%s: %d   ·   %s: %d",
             tt(s->lang, "nav.history"), st.n_transcripts,
             tt(s->lang, "nav.models"), st.n_models);
    ui_label(u, rectf(lx, y, card.w - 44 * u->L.scale, 22 * u->L.scale), line,
             12.5f * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
    y += 40 * u->L.scale;

    /* language */
    ui_label(u, rectf(lx, y, card.w * 0.40f, 32 * u->L.scale), tt(s->lang, "training.language"),
             11.5f * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
    ui_segmented(u, 6000, rectf(rx, y, ctl_w, 32 * u->L.scale), "فارسی|English", &g_train_lang, 2);
    y += row_h;

    /* epochs */
    ui_label(u, rectf(lx, y, card.w * 0.40f, 32 * u->L.scale), tt(s->lang, "training.epochs"),
             11.5f * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
    ui_stepper(u, 6100, rectf(rx, y, ctl_w * 0.55f, 32 * u->L.scale), &g_epochs, 1, 64);
    y += row_h;

    /* compute device */
    ui_label(u, rectf(lx, y, card.w * 0.40f, 32 * u->L.scale), tt(s->lang, "training.backend"),
             11.5f * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
    osa_rect tg = rectf(rx, y, 44 * u->L.scale, 24 * u->L.scale);
    ui_toggle_switch(u, 6200, tg, &g_use_gpu);
    osa_rect dl = rectf(u->L.rtl ? rx + 54 * u->L.scale : rx - 104 * u->L.scale,
                        y, 96 * u->L.scale, 24 * u->L.scale);
    ui_label(u, dl, tt(s->lang, g_use_gpu ? "training.gpu" : "training.cpu"),
             11 * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
    y += row_h;

    /* run */
    float by = y + 12 * u->L.scale;
    if (ui_button(u, 6300, rectf(lx, by, 150 * u->L.scale, 36 * u->L.scale),
                  tt(s->lang, "btn.run"), true))
        shell_set_message(s, true, "%s", tt(s->lang, "msg.trainunavail"));
    ui_label(u, rectf(lx + 168 * u->L.scale, by + 8 * u->L.scale,
                      card.w - 200 * u->L.scale, 22 * u->L.scale),
             tt(s->lang, "training.note"), 10.5f * u->L.scale, u->t.text_faint,
             u->L.rtl ? 2 : 0);

    /* preparation checklist */
    osa_rect prog = rectf(inner.x, inner.y + card_h + 14 * u->L.scale, inner.w,
                          inner.h - card_h - 14 * u->L.scale);
    if (prog.h > 70 * u->L.scale) {
        ui_card(u, prog);
        ui_label(u, rectf(prog.x + 18 * u->L.scale, prog.y + 12 * u->L.scale,
                          prog.w - 36 * u->L.scale, 20 * u->L.scale),
                 tt(s->lang, "training.plan"), 11.5f * u->L.scale, u->t.text_dim,
                 u->L.rtl ? 2 : 0);
        float ly = prog.y + 40 * u->L.scale;
        for (int i = 0; i < 3 && ly + 24 * u->L.scale < prog.y + prog.h; i++) {
            osa_rect row = rectf(prog.x + 18 * u->L.scale, ly, prog.w - 36 * u->L.scale,
                                 22 * u->L.scale);
            float dx = u->L.rtl ? row.x + row.w - 5 * u->L.scale : row.x + 5 * u->L.scale;
            c_circle(u->c, dx, row.y + 10 * u->L.scale, 4 * u->L.scale,
                     i == 0 ? u->t.success : u->t.border);
            ui_label(u, rectf(row.x + 20 * u->L.scale, row.y, row.w - 40 * u->L.scale,
                              22 * u->L.scale),
                     tt(s->lang, i == 0 ? "training.s1" : i == 1 ? "training.s2" : "training.s3"),
                     11 * u->L.scale, i == 0 ? u->t.text : u->t.text_dim, u->L.rtl ? 2 : 0);
            ly += 26 * u->L.scale;
        }
    }
}

/* ---- Evaluation --------------------------------------------------- */

static void draw_evaluation(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 20 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);

    if (!s->eval_json) {
        osa_rect card = rectf(inner.x, inner.y, inner.w, inner.h);
        ui_card(u, card);
        osa_rect run_r = rectf(card.x + card.w - 150 * u->L.scale,
                               card.y + 16 * u->L.scale, 130 * u->L.scale, 34 * u->L.scale);
        if (ui_button(u, 7000, run_r, tt(s->lang, "btn.run"), true))
            s->eval_json = osa_app_evaluate(s->app, NULL);
        osa_rect msg = rectf(card.x + 24 * u->L.scale, card.y + 24 * u->L.scale,
                             card.w - 200 * u->L.scale, 22 * u->L.scale);
        ui_label(u, msg, tt(s->lang, "eval.sub"), 12 * u->L.scale, u->t.text_dim,
                 u->L.rtl ? 2 : 0);
        ui_label(u, rect_inset(rectf(card.x, card.y + 80 * u->L.scale, card.w,
                                     card.h - 80 * u->L.scale), 24 * u->L.scale),
                 tt(s->lang, "eval.emptyhint"), 12 * u->L.scale, u->t.text_faint, 1);
        return;
    }

    /* Aggregates live *after* the items array; scanning forward from
     * "languages" would find a per-sentence value first. Take the last
     * occurrence of each key in the language block instead. */
    double wer = 0, cer = 0;
    const char *eval_end = s->eval_json + strlen(s->eval_json);
    const char *p = s->eval_json;
    for (;;) {
        const char *hit = strstr(p, "\"wer\"");
        if (!hit) break;
        double v = 0;
        if (json_num(hit, eval_end, "\"wer\"", &v)) wer = v;
        p = hit + 5;
    }
    p = s->eval_json;
    for (;;) {
        const char *hit = strstr(p, "\"cer\"");
        if (!hit) break;
        double v = 0;
        if (json_num(hit, eval_end, "\"cer\"", &v)) cer = v;
        p = hit + 5;
    }
    float cw = (inner.w - 28 * u->L.scale) / 3.0f;
    char v[48], loc[64];
    snprintf(v, sizeof v, "%.1f%%", wer * 100.0);
    text_localize_digits(v, s->lang == UI_FA ? "fa" : "en", loc, sizeof loc);
    draw_stat_card(u, rectf(inner.x, inner.y, cw, 76 * u->L.scale), "WER", loc, u->t.accent);
    snprintf(v, sizeof v, "%.1f%%", cer * 100.0);
    text_localize_digits(v, s->lang == UI_FA ? "fa" : "en", loc, sizeof loc);
    draw_stat_card(u, rectf(inner.x + cw + 14 * u->L.scale, inner.y, cw, 76 * u->L.scale),
                   "CER", loc, u->t.warning);
    snprintf(v, sizeof v, "%d", osa_cpu_count());
    text_localize_digits(v, s->lang == UI_FA ? "fa" : "en", loc, sizeof loc);
    draw_stat_card(u, rectf(inner.x + (cw + 14 * u->L.scale) * 2, inner.y, cw, 76 * u->L.scale),
                   tt(s->lang, "settings.threads"), loc, u->t.success);

    osa_rect table = rectf(inner.x, inner.y + 90 * u->L.scale, inner.w,
                           inner.h - 90 * u->L.scale);
    ui_card(u, table);
    ui_label(u, rectf(table.x + 18 * u->L.scale, table.y + 14 * u->L.scale,
                      table.w - 36 * u->L.scale, 18 * u->L.scale),
             tt(s->lang, "eval.samples"), 11 * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
    float ly = table.y + 40 * u->L.scale;
    int shown = 0;
    const char *sp = s->eval_json;
    const char *end = s->eval_json + strlen(s->eval_json);
    while (shown < 10 && sp && (sp = strstr(sp, "\"ref\"")) != NULL) {
        char ref[300], hyp[300];
        if (!json_str(sp, end, "\"ref\"", ref, sizeof ref)) break;
        const char *hp = strstr(sp, "\"hyp\"");
        if (!hp) break;
        if (!json_str(hp, end, "\"hyp\"", hyp, sizeof hyp)) break;
        if (ly + 26 * u->L.scale > table.y + table.h - 8) break;
        osa_color c = osa_streq(ref, hyp) ? u->t.success : u->t.warning;
        float half = (table.w - 36 * u->L.scale) * 0.47f;
        float cref = u->L.rtl ? table.x + 18 * u->L.scale + half + 12 * u->L.scale
                              : table.x + 18 * u->L.scale;
        ui_label(u, rectf(cref, ly, half, 20 * u->L.scale), ref, 10.5f * u->L.scale,
                 u->t.text_dim, u->L.rtl ? 2 : 0);
        float chyp = u->L.rtl ? table.x + 18 * u->L.scale : cref + half + 12 * u->L.scale;
        ui_label(u, rectf(chyp, ly, half, 20 * u->L.scale), hyp, 10.5f * u->L.scale, c,
                 u->L.rtl ? 2 : 0);
        ly += 24 * u->L.scale;
        shown++;
        sp = hp + 5;
    }
}

/* ---- Settings ----------------------------------------------------- */

static void draw_settings(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 20 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);
    float cw = inner.w > 760 * u->L.scale ? inner.w * 0.58f : inner.w;
    osa_rect card = rectf(u->L.rtl ? inner.x + inner.w - cw : inner.x, inner.y, cw, inner.h);
    ui_card(u, card);

    float y = card.y + 16 * u->L.scale;
    float row_h = 46 * u->L.scale;
    float lx = card.x + 18 * u->L.scale;
    float lw = card.w - 36 * u->L.scale;
    float ctrl_w = 150 * u->L.scale;

    struct { const char *key; bool *val; } toggles[] = {
        { "settings.dark", &s->dark },
        { "settings.rtl", NULL },
        { "settings.autodetect", &s->auto_detect },
        { "settings.saveaudio", &s->save_audio },
        { "settings.privacy", &s->privacy_logs },
    };
    for (int i = 0; i < 5; i++) {
        osa_rect row = rectf(lx, y, lw, row_h);
        ui_label(u, rectf(row.x, row.y + 12 * u->L.scale, row.w - ctrl_w - 10 * u->L.scale,
                          22 * u->L.scale),
                 tt(s->lang, toggles[i].key), 12 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
        osa_rect tg = rectf(u->L.rtl ? row.x : row.x + row.w - 44 * u->L.scale,
                            row.y + 12 * u->L.scale, 44 * u->L.scale, 24 * u->L.scale);
        bool changed = false;
        if (toggles[i].val) {
            bool before = *toggles[i].val;
            if (ui_toggle_switch(u, 8000 + i, tg, toggles[i].val)) changed = true;
            if (*toggles[i].val != before) changed = true;
        } else {
            bool rtl_on = s->lang == UI_FA;
            if (ui_toggle_switch(u, 8000 + i, tg, &rtl_on)) {
                s->lang = rtl_on ? UI_FA : UI_EN;
                changed = true;
            }
        }
        if (changed) {
            if (i == 0) ui_set_theme(u, s->dark);
            shell_save_prefs(s);
        }
        y += row_h;
        if (i < 4) c_hline(u->c, row.x, row.x + row.w, row.y + row_h - 0.5f, 1, u->t.border_soft);
    }

    /* scale slider */
    osa_rect srow = rectf(lx, y + 6 * u->L.scale, lw, 56 * u->L.scale);
    ui_label(u, rectf(srow.x, srow.y, srow.w, 20 * u->L.scale),
             tt(s->lang, "settings.scale"), 12 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    osa_rect sl = rectf(srow.x, srow.y + 26 * u->L.scale, srow.w, 22 * u->L.scale);
    if (ui_slider(u, 8100, sl, &s->scale, 0.8f, 1.6f)) shell_save_prefs(s);
    y += 66 * u->L.scale;

    /* threads */
    osa_rect trow = rectf(lx, y, lw, 44 * u->L.scale);
    ui_label(u, rectf(trow.x, trow.y + 10 * u->L.scale, trow.w - ctrl_w, 22 * u->L.scale),
             tt(s->lang, "settings.threads"), 12 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    osa_rect st = rectf(trow.x + (u->L.rtl ? 0 : trow.w - 130 * u->L.scale),
                        trow.y + 8 * u->L.scale, 130 * u->L.scale, 28 * u->L.scale);
    if (ui_stepper(u, 8200, st, &s->asr_threads, 1, osa_cpu_count() * 2)) shell_save_prefs(s);

    /* data location footer */
    osa_app_status stat;
    osa_app_get_status(s->app, &stat);
    char data[OSAI_MAX_PATH + 32];
    snprintf(data, sizeof data, "%s: %s", tt(s->lang, "settings.datalocation"), stat.data_root);
    ui_label(u, rectf(card.x + 18 * u->L.scale, card.y + card.h - 30 * u->L.scale,
                      card.w - 36 * u->L.scale, 18 * u->L.scale),
             data, 9.5f * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
    if (ui_button(u, 8300, rectf(u->L.rtl ? card.x + 18 * u->L.scale
                                          : card.x + card.w - 118 * u->L.scale,
                                 card.y + card.h - 62 * u->L.scale, 100 * u->L.scale,
                                 28 * u->L.scale), tt(s->lang, "btn.openfolder"), false))
        osa_open_in_explorer(stat.data_root);

    /* privacy note card on the left */
    if (inner.w > 760 * u->L.scale) {
        osa_rect side = rectf(u->L.rtl ? inner.x : inner.x + cw + 14 * u->L.scale,
                              inner.y, inner.w - cw - 14 * u->L.scale, inner.h);
        ui_card(u, side);
        text_opts o = text_default_opts(12 * u->L.scale);
        o.max_width = side.w - 40 * u->L.scale;
        o.line_height = 12 * u->L.scale * 1.9f;
        o.rtl = u->L.rtl;
        o.align = u->L.rtl ? 2 : 0;
        text_draw_box(u->c, u->reg, 12 * u->L.scale, u->t.text_dim,
                      rectf(side.x + 20 * u->L.scale, side.y + 24 * u->L.scale,
                            side.w - 40 * u->L.scale, side.h - 48 * u->L.scale),
                      tt(s->lang, "settings.privacynote"), o);
    }
}

/* ---- About -------------------------------------------------------- */

static void draw_about(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 20 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);
    osa_rect card = rectf(inner.x, inner.y, inner.w, MINF(inner.h, 320 * u->L.scale));
    ui_card(u, card);
    float cx = card.x + card.w * 0.5f;
    c_fill_round(u->c, rectf(cx - 34 * u->L.scale, card.y + 28 * u->L.scale,
                             68 * u->L.scale, 68 * u->L.scale),
                 20 * u->L.scale, u->t.accent);
    c_circle(u->c, cx, card.y + 62 * u->L.scale, 13 * u->L.scale, rgba(255,255,255,235));
    ui_label(u, rectf(card.x + 20 * u->L.scale, card.y + 110 * u->L.scale,
                      card.w - 40 * u->L.scale, 30 * u->L.scale),
             "OpenSpeech AI", 22 * u->L.scale, u->t.text, 1);
    ui_label(u, rectf(card.x + 20 * u->L.scale, card.y + 142 * u->L.scale,
                      card.w - 40 * u->L.scale, 20 * u->L.scale),
             tt(s->lang, "app.tagline"), 12 * u->L.scale, u->t.text_dim, 1);
    char ver[96];
    snprintf(ver, sizeof ver, "%s %s", tt(s->lang, "about.version"), APP_VERSION);
    ui_label(u, rectf(card.x + 20 * u->L.scale, card.y + 176 * u->L.scale,
                      card.w - 40 * u->L.scale, 20 * u->L.scale),
             ver, 11 * u->L.scale, u->t.text_faint, 1);

    float fy = card.y + 214 * u->L.scale;
    const char *notes[3] = { "about.note", "about.offline", "about.models" };
    for (int i = 0; i < 3; i++) {
        if (fy + 24 * u->L.scale > card.y + card.h) break;
        /* The note lines are centred, so a bullet at a fixed offset from the
         * card centre lands inside the sentence. Measure the line first and
         * put the dot just outside its leading edge. */
        const char *line = tt(s->lang, notes[i]);
        float fs = 11 * u->L.scale;
        float tw = text_measure(u->reg, fs, line);
        float avail = card.w - 44 * u->L.scale;
        if (tw > avail) tw = avail;
        float lx0 = u->L.rtl ? cx + tw * 0.5f : cx - tw * 0.5f;
        float dx = u->L.rtl ? lx0 + 10 * u->L.scale : lx0 - 10 * u->L.scale;
        c_circle(u->c, dx, fy + 9 * u->L.scale, 3 * u->L.scale, u->t.success);
        float lx = u->L.rtl ? cx + tw * 0.5f - avail : cx - tw * 0.5f;
        ui_label(u, rectf(lx, fy, avail, 20 * u->L.scale), line, fs,
                 u->t.text_dim, u->L.rtl ? 2 : 0);
        fy += 24 * u->L.scale;
    }
}

/* ---- dispatcher --------------------------------------------------- */

static const char *screen_title_key(shell_screen sc) {
    switch (sc) {
        case SCREEN_TRANSCRIBE: return "transcribe.title";
        case SCREEN_LIVE:       return "live.title";
        case SCREEN_TRANSLATE:  return "translate.title";
        case SCREEN_HISTORY:    return "history.title";
        case SCREEN_MODELS:     return "models.title";
        case SCREEN_TRAINING:   return "training.title";
        case SCREEN_EVALUATION: return "eval.title";
        case SCREEN_SETTINGS:   return "settings.title";
        case SCREEN_ABOUT:      return "about.title";
        default:                return "app.name";
    }
}

static const char *screen_sub_key(shell_screen sc) {
    switch (sc) {
        case SCREEN_TRANSCRIBE: return "transcribe.sub";
        case SCREEN_LIVE:       return "live.sub";
        case SCREEN_TRANSLATE:  return "translate.sub";
        case SCREEN_HISTORY:    return "history.sub";
        case SCREEN_MODELS:     return "models.sub";
        case SCREEN_TRAINING:   return "training.sub";
        case SCREEN_EVALUATION: return "eval.sub";
        case SCREEN_SETTINGS:   return "settings.sub";
        case SCREEN_ABOUT:      return "app.tagline";
        default:                return "";
    }
}

/* Draws the title bar for `sc` and returns the content rect below it. The
 * transcribe screen keeps its own title bar in shell.c, but going through one
 * function keeps the chrome identical on every screen. */
osa_rect shell_screen_chrome(shell_state *s, ui_context *u, osa_rect content,
                             shell_screen sc) {
    osa_rect rest = content;
    float top_h = 62 * u->L.scale;
    osa_rect topbar = rect_split_top(&rest, top_h);
    shell_draw_chrome(s, u, topbar, screen_title_key(sc), screen_sub_key(sc));
    return rest;
}

void shell_draw_screen(shell_state *s, ui_context *u, osa_rect body, const ui_input *in) {
    (void)in;
    switch (s->screen) {
        case SCREEN_LIVE:       draw_live(s, u, body); break;
        case SCREEN_TRANSLATE:  draw_translate(s, u, body); break;
        case SCREEN_HISTORY:    draw_history(s, u, body); break;
        case SCREEN_MODELS:     draw_models(s, u, body); break;
        case SCREEN_TRAINING:   draw_training(s, u, body); break;
        case SCREEN_EVALUATION: draw_evaluation(s, u, body); break;
        case SCREEN_SETTINGS:   draw_settings(s, u, body); break;
        case SCREEN_ABOUT:      draw_about(s, u, body); break;
        default: break;
    }
}
