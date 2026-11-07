/* =====================================================================
 *  OpenSpeech AI — desktop shell: core, chrome and the transcribe screen
 * ===================================================================== */
#include "shell.h"
#include "../core/osai_platform.h"
#include "../core/tts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#ifndef MAXF
#define MAXF(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MINF
#define MINF(a,b) ((a) < (b) ? (a) : (b))
#endif

#define APP_VERSION "1.0.0"

/* ---- palette accents used by the sidebar icons -------------------- */
/* The shipped build has no icon font, so the "icons" are single geometric
 * glyphs from Vazirmatn plus small drawn shapes. Using text keeps the binary
 * free of image assets. */
static const char *nav_icon(shell_screen s) {
    switch (s) {
        /* Only glyphs that ship inside Vazirmatn are used here; anything else
         * renders as a tofu box, which is exactly the kind of detail that makes
         * a desktop app look unfinished. */
        case SCREEN_TRANSCRIBE: return "■";
        case SCREEN_LIVE:       return "●";
        case SCREEN_TRANSLATE:  return "◐";
        case SCREEN_HISTORY:    return "◆";
        case SCREEN_MODELS:     return "▲";
        case SCREEN_TRAINING:   return "◈";
        case SCREEN_EVALUATION: return "◇";
        case SCREEN_SETTINGS:   return "▪";
        case SCREEN_ABOUT:      return "○";
        default:                return "•";
    }
}

static const char *nav_label_key(shell_screen s) {
    switch (s) {
        case SCREEN_TRANSCRIBE: return "nav.transcribe";
        case SCREEN_LIVE:       return "nav.live";
        case SCREEN_TRANSLATE:  return "nav.translate";
        case SCREEN_HISTORY:    return "nav.history";
        case SCREEN_MODELS:     return "nav.models";
        case SCREEN_TRAINING:   return "nav.training";
        case SCREEN_EVALUATION: return "nav.evaluation";
        case SCREEN_SETTINGS:   return "nav.settings";
        case SCREEN_ABOUT:      return "nav.about";
        default:                return "";
    }
}

/* ---- preferences -------------------------------------------------- */
static void prefs_path(char *out, size_t cap) {
    snprintf(out, cap, "%ssettings.ini", osa_path(OSAI_DIR_ROOT));
}

void shell_load_prefs(shell_state *s) {
    s->lang = UI_FA;
    s->dark = true;
    s->scale = 1.0f;
    s->auto_detect = true;
    s->save_audio = true;
    s->privacy_logs = true;
    s->vad_sensitivity = 0.5f;
    s->asr_threads = osa_cpu_count();
    if (s->asr_threads < 1) s->asr_threads = 1;
    if (s->asr_threads > 16) s->asr_threads = 16;

    char path[OSAI_MAX_PATH];
    prefs_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char key[64];
        char val[440];
        if (sscanf(line, "%63[^=]=%439[^\r\n]", key, val) != 2) continue;
        if (osa_streq(key, "lang")) s->lang = osa_starts(val, "en") ? UI_EN : UI_FA;
        else if (osa_streq(key, "dark")) s->dark = osa_starts(val, "1");
        else if (osa_streq(key, "scale")) s->scale = (float)atof(val);
        else if (osa_streq(key, "autodetect")) s->auto_detect = osa_starts(val, "1");
        else if (osa_streq(key, "saveaudio")) s->save_audio = osa_starts(val, "1");
        else if (osa_streq(key, "privacylogs")) s->privacy_logs = osa_starts(val, "1");
        else if (osa_streq(key, "threads")) s->asr_threads = atoi(val);
    }
    fclose(f);
    if (s->scale < 0.75f || s->scale > 3.0f) s->scale = 1.0f;
}

void shell_save_prefs(shell_state *s) {
    char path[OSAI_MAX_PATH];
    prefs_path(path, sizeof path);
    char tmp[OSAI_MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fprintf(f, "lang=%s\n", s->lang == UI_EN ? "en" : "fa");
    fprintf(f, "dark=%d\n", s->dark ? 1 : 0);
    fprintf(f, "scale=%.2f\n", (double)s->scale);
    fprintf(f, "autodetect=%d\n", s->auto_detect ? 1 : 0);
    fprintf(f, "saveaudio=%d\n", s->save_audio ? 1 : 0);
    fprintf(f, "privacylogs=%d\n", s->privacy_logs ? 1 : 0);
    fprintf(f, "threads=%d\n", s->asr_threads);
    fflush(f);
    fclose(f);
    osa_file_rename(tmp, path);
}

/* ---- lifecycle ---------------------------------------------------- */

bool shell_init(shell_state *s, osa_app *app) {
    if (!s) return false;
    memset(s, 0, sizeof *s);
    s->app = app;
    s->screen = SCREEN_TRANSCRIBE;
    shell_load_prefs(s);
    return true;
}

void shell_shutdown(shell_state *s) {
    if (!s) return;
    free(s->peaks);
    free(s->history_json);
    free(s->models_json);
    free(s->eval_json);
    s->peaks = NULL;
    s->history_json = s->models_json = s->eval_json = NULL;
}

/* ---- waveform ----------------------------------------------------- */

bool shell_load_waveform(shell_state *s, const char *path) {
    free(s->peaks);
    s->peaks = NULL;
    s->n_peaks = 0;
    if (!path || !*path) return false;
    osa_audio au;
    if (!osa_audio_load(path, &au, NULL)) return false;
    enum { NBINS = 260 };
    float *p = (float *)calloc(NBINS, sizeof(float));
    if (!p) { osa_audio_free(&au); return false; }
    int64_t per = au.n / NBINS;
    if (per < 1) per = 1;
    for (int i = 0; i < NBINS; i++) {
        int64_t s0 = (int64_t)i * per;
        int64_t s1 = MINF(s0 + per, au.n);
        float peak = 0;
        for (int64_t k = s0; k < s1; k++) {
            float a = fabsf(au.samples[k]);
            if (a > peak) peak = a;
        }
        p[i] = peak;
    }
    /* normalise so quiet recordings are still visible */
    float peak = 0;
    for (int i = 0; i < NBINS; i++) if (p[i] > peak) peak = p[i];
    if (peak > 1e-5f) for (int i = 0; i < NBINS; i++) p[i] /= peak;
    s->peaks = p;
    s->n_peaks = NBINS;
    s->playhead = 0;
    s->playing = false;
    osa_audio_free(&au);
    return true;
}

/* ---- actions ------------------------------------------------------ */

void shell_set_message(shell_state *s, bool err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->message, sizeof s->message, fmt, ap);
    va_end(ap);
    s->message_is_error = err;
}

bool shell_run_transcribe(shell_state *s) {
    if (!s->app || !s->audio_path[0]) {
        shell_set_message(s, true, "%s", tt(s->lang, "msg.nofile"));
        return false;
    }
    s->working = true;
    s->message[0] = 0;
    osa_error err;
    osa_app_transcript tr;
    osa_app_translation mt;
    bool ok = osa_app_transcribe_and_translate(s->app, s->audio_path, &tr, &mt, &err);
    s->working = false;
    if (!ok) {
        shell_set_message(s, true, "%s: %s", tt(s->lang, "msg.failed"), err.msg);
        return false;
    }
    osa_strlcpy(s->transcript, tr.text, sizeof s->transcript);
    osa_strlcpy(s->translation, mt.text, sizeof s->translation);
    osa_strlcpy(s->detected_lang, tr.language, sizeof s->detected_lang);
    osa_strlcpy(s->transcript_id, tr.id, sizeof s->transcript_id);
    s->transcript_conf = (float)tr.confidence;
    s->transcript_rtf = tr.rtf;
    s->transcript_dur = tr.duration_s;
    shell_set_message(s, false, "%s", tt(s->lang, "msg.ok"));
    /* refresh the history cache so the history screen is current */
    free(s->history_json);
    s->history_json = osa_app_history_json(s->app, 100, NULL);
    return true;
}

/* ---- chrome ------------------------------------------------------- */


/* Sidebar: product mark, navigation, and the offline/privacy badge. */
static void draw_sidebar(shell_state *s, ui_context *u, osa_rect r) {
    c_fill(u->c, r, u->t.sidebar);
    float pad = 12 * u->L.scale;
    float y = r.y + pad;

    /* product mark */
    float box = 34 * u->L.scale;
    osa_rect logo_r = rectf(u->L.rtl ? r.x + r.w - pad - box : r.x + pad, y, box, box);
    c_fill_round(u->c, logo_r, box * 0.3f, u->t.accent);
    c_circle(u->c, logo_r.x + box * 0.5f, logo_r.y + box * 0.5f, box * 0.18f,
             rgba(255, 255, 255, 235));
    osa_rect name_r = rectf(logo_r.x + (u->L.rtl ? -(r.w - box - pad * 2) : box + pad),
                            y - 2 * u->L.scale, r.w - box - pad * 3, 20 * u->L.scale);
    ui_label(u, name_r, "OpenSpeech AI", 15 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    osa_rect tag_r = rectf(name_r.x, y + 19 * u->L.scale, name_r.w, 16 * u->L.scale);
    ui_label(u, tag_r, tt(s->lang, "app.tagline"), 10.5f * u->L.scale, u->t.text_faint,
             u->L.rtl ? 2 : 0);
    y += box + pad * 1.6f;

    ui_separator(u, rectf(r.x + pad, y, r.w - pad * 2, 1));
    y += pad;

    /* navigation */
    float item_h = 38 * u->L.scale;
    for (int i = 0; i < SCREEN_COUNT; i++) {
        osa_rect ir = rectf(r.x + pad * 0.6f, y, r.w - pad * 1.2f, item_h);
        if (y + item_h > r.y + r.h - 96 * u->L.scale) break;
        int id = 1000 + i;
        if (ui_nav_item(u, id, ir, nav_icon((shell_screen)i),
                        tt(s->lang, nav_label_key((shell_screen)i)),
                        s->screen == (shell_screen)i)) {
            s->screen = (shell_screen)i;
            s->message[0] = 0;
            if (s->screen == SCREEN_HISTORY) {
                free(s->history_json);
                s->history_json = osa_app_history_json(s->app, 100, NULL);
            } else if (s->screen == SCREEN_MODELS) {
                free(s->models_json);
                s->models_json = osa_app_models_json(s->app, NULL);
            } else if (s->screen == SCREEN_EVALUATION && !s->eval_json) {
                s->eval_json = osa_app_evaluate(s->app, NULL);
            }
        }
        y += item_h + 2 * u->L.scale;
    }

    /* bottom: privacy badge + status */
    float bottom = r.y + r.h - pad;
    osa_rect badge = rectf(r.x + pad, bottom - 54 * u->L.scale, r.w - pad * 2, 54 * u->L.scale);
    c_fill_round(u->c, badge, u->L.radius * 0.8f, col_alpha(u->t.success, 0.10f));
    c_circle(u->c, u->L.rtl ? badge.x + badge.w - 16 * u->L.scale : badge.x + 16 * u->L.scale,
             badge.y + 18 * u->L.scale, 4 * u->L.scale, u->t.success);
    ui_label(u, rectf(badge.x + (u->L.rtl ? 10 : 28) * u->L.scale, badge.y + 8 * u->L.scale,
                      badge.w - 38 * u->L.scale, 18 * u->L.scale),
             tt(s->lang, "status.offline"), 11.5f * u->L.scale, u->t.success,
             u->L.rtl ? 2 : 0);
    ui_label(u, rectf(badge.x + (u->L.rtl ? 10 : 28) * u->L.scale, badge.y + 26 * u->L.scale,
                      badge.w - 38 * u->L.scale, 18 * u->L.scale),
             "0 KB uploaded", 10 * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
}

/* Top bar: screen title + status chips + quick actions. */
osa_rect shell_draw_chrome(shell_state *s, ui_context *u, osa_rect r, const char *title_key,
                           const char *subtitle_key) {
    c_fill(u->c, r, u->t.bg);
    float pad = 20 * u->L.scale;
    osa_rect tr = rectf(r.x + pad, r.y + 10 * u->L.scale, r.w * 0.55f, 24 * u->L.scale);
    ui_label(u, tr, tt(s->lang, title_key), 18 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    osa_rect sr = rectf(tr.x, r.y + 32 * u->L.scale, tr.w, 17 * u->L.scale);
    ui_label(u, sr, tt(s->lang, subtitle_key), 11.5f * u->L.scale, u->t.text_dim,
             u->L.rtl ? 2 : 0);

    /* right side: language switch + theme + model chip */
    float right = u->L.rtl ? r.x + pad : r.x + r.w - pad;
    float chip_h = 26 * u->L.scale;
    char model[64];
    osa_app_status st;
    osa_app_get_status(s->app, &st);
    osa_strlcpy(model, st.asr_model[0] ? "Model ready" : "No model", sizeof model);
    float cw = 130 * u->L.scale;
    osa_rect chip = rectf(u->L.rtl ? right : right - cw, r.y + 14 * u->L.scale, cw, chip_h);
    ui_status_chip(u, chip, model, st.asr_ready ? u->t.success : u->t.danger);
    right += u->L.rtl ? cw + 8 * u->L.scale : -(cw + 8 * u->L.scale);

    /* language toggle */
    float lw = 84 * u->L.scale;
    osa_rect lr = rectf(u->L.rtl ? right : right - lw, r.y + 14 * u->L.scale, lw, chip_h);
    c_fill_round(u->c, lr, chip_h * 0.5f, u->t.panel_alt);
    float half = (lw - 4) * 0.5f;
    osa_rect fa_r = rectf(lr.x + 2, lr.y + 2, half, chip_h - 4);
    osa_rect en_r = rectf(lr.x + 2 + half, lr.y + 2, half, chip_h - 4);
    if (s->lang == UI_FA) c_fill_round(u->c, fa_r, (chip_h - 4) * 0.5f, u->t.accent);
    else c_fill_round(u->c, en_r, (chip_h - 4) * 0.5f, u->t.accent);
    ui_label(u, fa_r, "FA", 11 * u->L.scale,
             s->lang == UI_FA ? rgba(255,255,255,255) : u->t.text_dim, 1);
    ui_label(u, en_r, "EN", 11 * u->L.scale,
             s->lang == UI_EN ? rgba(255,255,255,255) : u->t.text_dim, 1);
    if (rect_has(fa_r, u->in.mx, u->in.my) && u->in.clicked) {
        s->lang = UI_FA; shell_save_prefs(s); u->consumed_click = true;
    }
    if (rect_has(en_r, u->in.mx, u->in.my) && u->in.clicked) {
        s->lang = UI_EN; shell_save_prefs(s); u->consumed_click = true;
    }
    right += u->L.rtl ? lw + 8 * u->L.scale : -(lw + 8 * u->L.scale);

    /* theme toggle */
    float tw = 34 * u->L.scale;
    osa_rect th = rectf(u->L.rtl ? right : right - tw, r.y + 14 * u->L.scale, tw, chip_h);
    c_fill_round(u->c, th, chip_h * 0.5f, u->t.panel_alt);
    {
        float ccx = th.x + th.w * 0.5f, ccy = th.y + th.h * 0.5f;
        if (s->dark) {
            c_circle(u->c, ccx, ccy, 6 * u->L.scale, u->t.text_dim);
            c_circle(u->c, ccx + 3 * u->L.scale, ccy - 3 * u->L.scale,
                     5 * u->L.scale, u->t.panel_alt);
        } else {
            c_circle(u->c, ccx, ccy, 5.0f * u->L.scale, u->t.warning);
            for (int k = 0; k < 8; k++) {
                float a = (float)k * 0.7853982f;
                for (int seg = 0; seg < 3; seg++) {
                    float rr = (8.0f + (float)seg * 1.1f) * u->L.scale;
                    c_circle(u->c, ccx + cosf(a) * rr, ccy + sinf(a) * rr,
                             1.1f * u->L.scale, u->t.warning);
                }
            }
        }
    }
    if (ui_hit(u, th) && u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        s->dark = !s->dark;
        ui_set_theme(u, s->dark);
        shell_save_prefs(s);
    }

    c_hline(u->c, r.x, r.x + r.w, r.y + r.h - 0.5f, 1, u->t.border_soft);
    osa_rect body = rectf(r.x, r.y + r.h, r.w, u->L.win_h - (r.y + r.h));
    return body;
}

/* Toast at the bottom of the content area. */
void shell_draw_toast(shell_state *s, ui_context *u, osa_rect area) {
    if (!s->message[0]) return;
    float pad = 20 * u->L.scale;
    float h = 40 * u->L.scale;
    float w = MINF(area.w - pad * 2, 520 * u->L.scale);
    osa_rect r = rectf(area.x + (area.w - w) * 0.5f, area.y + area.h - h - pad * 0.4f, w, h);
    osa_color base = s->message_is_error ? u->t.danger : u->t.success;
    c_fill_round(u->c, r, h * 0.5f, col_alpha(base, 0.16f));
    c_stroke_round(u->c, r, h * 0.5f, 1, col_alpha(base, 0.45f));
    c_circle(u->c, u->L.rtl ? r.x + r.w - 20 * u->L.scale : r.x + 20 * u->L.scale,
             r.y + h * 0.5f, 4 * u->L.scale, base);
    osa_rect tr = rectf(r.x + 34 * u->L.scale, r.y, r.w - 48 * u->L.scale, h);
    ui_label(u, tr, s->message, 12 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    if (u->in.clicked && !u->consumed_click) {
        /* clicking anywhere dismisses it after the click was handled by widgets */
    }
}

/* ---- transcribe screen -------------------------------------------- */

static void draw_transcribe(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 20 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);
    float gap = 14 * u->L.scale;

    /* --- file card ------------------------------------------------- */
    float card_h = 146 * u->L.scale;
    osa_rect file_card = rectf(inner.x, inner.y, inner.w, card_h);
    ui_card(u, file_card);
    osa_rect fi = rect_inset(file_card, 14 * u->L.scale);

    /* icon tile */
    float bw0 = 132 * u->L.scale;                        /* action button width */
    float tile = 56 * u->L.scale;
    float icon_y = fi.y + (fi.h - 44 * u->L.scale - tile) * 0.5f;
    osa_rect icon_r = rectf(u->L.rtl ? fi.x + bw0 + 18 * u->L.scale : fi.x, icon_y,
                            tile, tile);
    bool have = s->audio_path[0] != 0;
    c_fill_round(u->c, icon_r, tile * 0.28f,
                 have ? col_alpha(u->t.accent, 0.16f) : u->t.panel_alt);
    c_fill_round(u->c, rectf(icon_r.x + tile * 0.3f, icon_r.y + tile * 0.28f,
                             tile * 0.4f, tile * 0.44f), 3 * u->L.scale,
                 have ? u->t.accent : u->t.text_faint);
    c_circle(u->c, icon_r.x + tile * 0.5f, icon_r.y + tile * 0.5f, tile * 0.09f,
             have ? u->t.accent : u->t.text_faint);

    /* text block: between the icon tile and the button column */
    float text_x = u->L.rtl ? fi.x + bw0 + 18 * u->L.scale : fi.x + tile + 14 * u->L.scale;
    float text_w = fi.w - tile - bw0 - 32 * u->L.scale;
    osa_rect name_r = rectf(text_x, fi.y + 8 * u->L.scale, text_w, 22 * u->L.scale);
    if (have) {
        const char *base = strrchr(s->audio_path, '/');
        if (!base) base = strrchr(s->audio_path, '\\');
        ui_label(u, name_r, base ? base + 1 : s->audio_path, 14 * u->L.scale, u->t.text,
                 u->L.rtl ? 2 : 0);
        char dur[32];
        ui_fmt_duration(s->transcript_dur, dur, sizeof dur);
        char dur_loc[48];
        char meta[160];
        text_localize_digits(dur, s->lang == UI_FA ? "fa" : "en", dur_loc, sizeof dur_loc);
        snprintf(meta, sizeof meta, "%s  ·  %s", dur_loc,
                 s->detected_lang[0] ? (osa_starts(s->detected_lang, "fa") ? "فارسی" : "English") : "—");
        ui_label(u, rectf(name_r.x, fi.y + 30 * u->L.scale, text_w, 18 * u->L.scale),
                 meta, 11 * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
    } else {
        ui_label(u, name_r, tt(s->lang, "transcribe.none"), 14 * u->L.scale, u->t.text_dim,
                 u->L.rtl ? 2 : 0);
        ui_label(u, rectf(name_r.x, fi.y + 30 * u->L.scale, text_w, 18 * u->L.scale),
                 tt(s->lang, "transcribe.sub"), 11 * u->L.scale, u->t.text_faint,
                 u->L.rtl ? 2 : 0);
    }

    /* actions, right aligned (left in RTL) */
    float bw = bw0, bh = 34 * u->L.scale;
    float bx = u->L.rtl ? fi.x : fi.x + fi.w - bw;
    float by = fi.y + 6 * u->L.scale;
    if (ui_button(u, 2001, rectf(bx, by, bw, bh), tt(s->lang, "btn.choose"), false)) {
        /* Hand the request to the host: it owns the native file dialog. The
         * headless build answers with a "no file" message instead. */
        s->request = 1;
    }
    if (ui_button(u, 2002, rectf(bx, by + bh + 8 * u->L.scale, bw, bh),
                  tt(s->lang, "btn.transcribe"), true)) {
        shell_run_transcribe(s);
    }

    /* waveform strip inside the file card */
    if (s->n_peaks > 0) {
        osa_rect wr = rectf(fi.x, file_card.y + card_h - 44 * u->L.scale,
                            fi.w, 32 * u->L.scale);
        ui_waveform(u, wr, s->peaks, s->n_peaks, s->playhead, s->playing);
    }

    /* --- results: two columns (source | translation) ---------------- */
    float top = file_card.y + card_h + gap;
    float avail_h = inner.y + inner.h - top - 54 * u->L.scale;
    if (avail_h < 80 * u->L.scale) avail_h = 80 * u->L.scale;
    float col_gap = gap;
    float col_w = (inner.w - col_gap) * 0.5f;
    /* Persian UI: source on the right, translation on the left, matching the
     * reading order of the interface itself. */
    osa_rect col_a = rectf(u->L.rtl ? inner.x + col_w + col_gap : inner.x, top, col_w, avail_h);
    osa_rect col_b = rectf(u->L.rtl ? inner.x : inner.x + col_w + col_gap, top, col_w, avail_h);

    static const char *labels[2] = { "transcribe.source", "transcribe.result" };
    osa_rect *cols[2] = { &col_a, &col_b };
    for (int i = 0; i < 2; i++) {
        /* read the live buffer every iteration: the transcribe button above may
         * have just replaced its contents in this same frame */
        const char *texts[2] = { s->transcript, s->translation };
        ui_card(u, *cols[i]);
        bool has_text = texts[i][0] != 0;
        float copy_w = has_text ? 52 * u->L.scale : 0;
        /* header: title on the reading-start side, copy action on the other */
        osa_rect head = rectf(u->L.rtl ? cols[i]->x + 14 * u->L.scale + copy_w + 6 * u->L.scale
                                       : cols[i]->x + 14 * u->L.scale,
                              cols[i]->y + 12 * u->L.scale,
                              cols[i]->w - 28 * u->L.scale - copy_w - 6 * u->L.scale,
                              20 * u->L.scale);
        ui_label(u, head, tt(s->lang, labels[i]), 12 * u->L.scale, u->t.text_dim,
                 u->L.rtl ? 2 : 0);
        if (has_text) {
            osa_rect copy_r = rectf(u->L.rtl ? cols[i]->x + 14 * u->L.scale
                                             : cols[i]->x + cols[i]->w - 14 * u->L.scale - copy_w,
                                    cols[i]->y + 9 * u->L.scale, copy_w, 24 * u->L.scale);
            if (ui_button(u, 2100 + i, copy_r, tt(s->lang, "btn.copy"), false))
                osa_clipboard_set(texts[i]);
        }
        c_hline(u->c, cols[i]->x + 14 * u->L.scale, cols[i]->x + cols[i]->w - 14 * u->L.scale,
                cols[i]->y + 40 * u->L.scale, 1, u->t.border_soft);
        osa_rect tr = rectf(cols[i]->x + 14 * u->L.scale, cols[i]->y + 50 * u->L.scale,
                            cols[i]->w - 28 * u->L.scale, cols[i]->h - 84 * u->L.scale);
        if (texts[i][0]) {
            /* source is Persian or English depending on detection; the shell draws
         * it in the language's own direction */
            text_opts o = text_default_opts(15 * u->L.scale);
            o.max_width = tr.w;
            o.line_height = 15 * u->L.scale * 1.7f;
            bool rtl_text = (i == 0 && s->detected_lang[0] && osa_starts(s->detected_lang, "fa"));
            if (i == 1) rtl_text = osa_starts(s->detected_lang, "en");
            o.rtl = rtl_text;
            text_draw_box(u->c, u->reg, 15 * u->L.scale, u->t.text, tr, texts[i], o);
        } else {
            ui_label(u, rectf(tr.x, tr.y + tr.h * 0.4f, tr.w, 20 * u->L.scale),
                     tt(s->lang, "transcribe.empty"), 12 * u->L.scale, u->t.text_faint, 1);
        }
        /* footer metric: a dedicated strip, so the numbers are readable */
        if (i == 0 && s->transcript[0]) {
            char pct[32], ms[32], conf_loc[48], proc_loc[48];
            char foot[200];
            snprintf(pct, sizeof pct, "%.0f%%", s->transcript_conf * 100.0f);
            snprintf(ms, sizeof ms, "%.0f ms", s->transcript_rtf * 1000.0);
            text_localize_digits(pct, s->lang == UI_FA ? "fa" : "en", conf_loc, sizeof conf_loc);
            text_localize_digits(ms, s->lang == UI_FA ? "fa" : "en", proc_loc, sizeof proc_loc);
            snprintf(foot, sizeof foot, "%s  ·  %s", conf_loc, proc_loc);
            float fy = cols[i]->y + cols[i]->h - 26 * u->L.scale;
            c_fill_round(u->c, rectf(cols[i]->x + 10 * u->L.scale, fy,
                                     cols[i]->w - 20 * u->L.scale, 20 * u->L.scale),
                         6 * u->L.scale, col_alpha(u->t.success, 0.10f));
            ui_label(u, rectf(cols[i]->x + 18 * u->L.scale, fy,
                              cols[i]->w - 36 * u->L.scale, 20 * u->L.scale),
                     foot, 10.5f * u->L.scale, u->t.success, u->L.rtl ? 2 : 0);
        }
    }
}

/* ---- public draw entry point -------------------------------------- */
void shell_draw(shell_state *s, osa_canvas *c, ui_context *u, const ui_input *in) {
    if (!s || !c || !u) return;
    s->frame++;
    if (in) u->in = *in;
    else memset(&u->in, 0, sizeof u->in);
    u->L.win_w = c->w;
    u->L.win_h = c->h;
    u->next_id = 1;
    u->consumed_click = false;

    ui_set_theme(u, s->dark);
    ui_set_rtl(u, s->lang == UI_FA);
    ui_set_scale(u, s->scale);

    canvas_clear(c, u->t.bg);

    /* sidebar + content split, mirrored for RTL */
    float sb_w = 226 * u->L.scale;
    osa_rect full = rectf(0, 0, (float)c->w, (float)c->h);
    /* The rail sits on the reading-start edge: right in Persian, left in
     * English. ui_split_leading does the mirroring, so pass the direction. */
    osa_rect sidebar = ui_split_leading(&full, sb_w, u->L.rtl);
    draw_sidebar(s, u, sidebar);

    osa_rect content = full;
    osa_rect body = content;   /* chrome trims the title bar off below */

    body = shell_screen_chrome(s, u, content, s->screen);
    if (s->screen == SCREEN_TRANSCRIBE) draw_transcribe(s, u, body);
    else shell_draw_screen(s, u, body, in);
    shell_draw_toast(s, u, body);
}
