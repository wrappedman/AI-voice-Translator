/* =====================================================================
 *  OpenSpeech AI — desktop shell: core, chrome and the transcribe screen
 *
 *  Layout (mirrored for Persian):
 *
 *    +-----------+--------------------------------------------------+
 *    | brand     |  title                            chips  FA EN ☾  |
 *    |           +--------------------------------------------------+
 *    | nav items |                                                  |
 *    |  …        |                 screen content                   |
 *    |           |                                                  |
 *    | privacy   |                                                  |
 *    +-----------+--------------------------------------------------+
 *
 *  Every screen draws inside the content rect the chrome returns; the shell
 *  owns the state and the platform owns the window.
 * ===================================================================== */
#include "shell_internal.h"
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


/* ---- navigation --------------------------------------------------- */

/* The shipped build has no icon font, so the "icons" are single geometric
 * glyphs from Vazirmatn plus small drawn shapes. Using text keeps the binary
 * free of image assets. Only glyphs that ship inside Vazirmatn are used here;
 * anything else renders as a tofu box, which is exactly the kind of detail that
 * makes a desktop app look unfinished. */
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

static bool lt_fa(shell_state *s) { return s->lang == UI_FA; }
static bool lang_rtl(const char*c){return c&&(osa_streq(c,"fa")||osa_streq(c,"ar")||osa_streq(c,"he")||osa_streq(c,"ur")||osa_streq(c,"ps")||osa_streq(c,"sd")||osa_streq(c,"yi"));}
static bool screen_allowed(shell_state*s,shell_screen sc){return s->models_ready||sc==SCREEN_MODELS||sc==SCREEN_SETTINGS||sc==SCREEN_ABOUT;}

const char *shell_lang_name(shell_state *s, const char *code) {
    if (!code || !*code) return "—";
    return osa_lang_name(code, s && s->lang == UI_FA);
}

/* ---- preferences -------------------------------------------------- */

static void prefs_path(char *out, size_t cap) {
    snprintf(out, cap, "%ssettings.ini", osa_path(OSAI_DIR_ROOT));
}

void shell_load_prefs(shell_state *s) {
    s->lang = UI_FA;
    s->dark = false;          /* Persian + light is the shipping default */
    s->scale = 1.0f;
    s->auto_detect = true;
    s->save_audio = true;
    s->privacy_logs = true;
    s->show_phonetic = true;
    s->vad_sensitivity = 0.5f;
    /* A fixed default, not the core count: the Settings screen shows this
     * number, so using the host's core count would make the rendered snapshot
     * depend on the machine (2 cores here, 4 in CI, 16 on a workstation) and the
     * visual-regression job would fail on a difference that is not one. */
    s->asr_threads = 4;
    if (s->asr_threads < 1) s->asr_threads = 1;
    if (s->asr_threads > 16) s->asr_threads = 16;
    osa_strlcpy(s->lang_src, "auto", sizeof s->lang_src);
    osa_strlcpy(s->lang_dst, "fa", sizeof s->lang_dst);
    s->style = MT_STYLE_NEUTRAL;
    s->train_epochs = 6;
    s->train_lang = 0;
    s->models_tab = 0;

    char path[OSAI_MAX_PATH];
    prefs_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (f) {
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
            else if (osa_streq(key, "phonetic")) s->show_phonetic = osa_starts(val, "1");
            else if (osa_streq(key, "threads")) s->asr_threads = atoi(val);
            else if (osa_streq(key, "langsrc")) osa_strlcpy(s->lang_src, val, sizeof s->lang_src);
            else if (osa_streq(key, "langdst")) osa_strlcpy(s->lang_dst, val, sizeof s->lang_dst);
            else if (osa_streq(key, "style")) {
                if (osa_starts(val, "formal")) s->style = MT_STYLE_FORMAL;
                else if (osa_starts(val, "casual")) s->style = MT_STYLE_CASUAL;
                else s->style = MT_STYLE_NEUTRAL;
            }
        }
        fclose(f);
    }
    if (s->scale < 0.75f || s->scale > 2.0f) s->scale = 1.0f;
    /* Model-derived pair is restored after the background model check. */
    osa_app_set_style(s->app, s->style);
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
    fprintf(f, "phonetic=%d\n", s->show_phonetic ? 1 : 0);
    fprintf(f, "threads=%d\n", s->asr_threads);
    fprintf(f, "langsrc=%s\n", s->lang_src);
    fprintf(f, "langdst=%s\n", s->lang_dst);
    fprintf(f, "style=%s\n", mt_style_key(s->style));
    fflush(f);
    fclose(f);
    osa_file_rename(tmp, path);
}

/* ---- lifecycle ---------------------------------------------------- */

bool shell_init(shell_state *s, osa_app *app) {
    if (!s) return false;
    memset(s, 0, sizeof *s); s->app = app; s->screen = SCREEN_TRANSCRIBE; s->ds_sel = -1;
    shell_load_prefs(s);
    shell_refresh_models_dir(s);
    osa_strlcpy(s->tr_in, "از آشنایی با شما خوشحالم", sizeof s->tr_in);
    s->restoring = true; s->working = true;
    shell_start_job(s, SHELL_JOB_RESTORE, NULL);
    return true;
}
void shell_shutdown(shell_state *s) {
    if (!s) return;
    shell_cancel_jobs(s); shell_live_shutdown(s);
    free(s->peaks); free(s->history_json); free(s->models_json); free(s->eval_json);
    s->peaks = NULL; s->history_json = s->models_json = s->eval_json = NULL;
}

/* Synthesises `text` with the builtin TTS and plays it. The clip is written to
 * the cache directory, not next to the executable, and the previous clip is
 * replaced: the UI never accumulates audio files without the user asking. */
void shell_speak(shell_state *s, const char *text, const char *lang) {
    if (!s || !text || !*text) return;
    int64_t n = 0;
    float *pcm = tts_speak(text, lang && *lang ? lang : "en", &n);
    if (!pcm || n <= 0) {
        free(pcm);
        shell_set_message(s, true, "%s", tt(s->lang, "msg.tts"));
        return;
    }
    char path[OSAI_MAX_PATH];
    snprintf(path, sizeof path, "%sspeak.wav", osa_path(OSAI_DIR_CACHE));
    if (!osa_audio_save_wav(path, pcm, n, OSAI_SR)) {
        free(pcm);
        shell_set_message(s, true, "%s", tt(s->lang, "msg.tts"));
        return;
    }
    free(pcm);
    if (osa_audio_play_file(path)) shell_set_message(s, false, "%s", tt(s->lang, "msg.playing"));
    else shell_set_message(s, true, "%s", tt(s->lang, "msg.noplay"));
}

/* How long a message stays on screen. Errors are not permanent either: a red
 * band that never goes away is indistinguishable from a broken screen, and the
 * user asked for exactly that behaviour. Everything that matters is written to
 * the fault log / the row it belongs to as well, so the toast is a notification,
 * not the record. */
#define OSAI_TOAST_MS_CLEAR 3500
#define OSAI_TOAST_MS_ERROR 5500

void shell_set_message(shell_state *s, bool err, const char *fmt, ...) {
    if (!s) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->message, sizeof s->message, fmt, ap);
    va_end(ap);
    s->message_is_error = err;
    s->message_at_ms = osa_now_ms();
    s->message_life_ms = err ? OSAI_TOAST_MS_ERROR : OSAI_TOAST_MS_CLEAR;
}

/* Called once per frame, before the screens draw. A message whose time is up is
 * dropped here so every screen gets the same behaviour instead of each one
 * clearing (or not clearing) the band itself. */
void shell_message_tick(shell_state *s) {
    if (!s || !s->message[0]) return;
    if (s->message_life_ms <= 0) s->message_life_ms = OSAI_TOAST_MS_CLEAR;
    if (osa_now_ms() - s->message_at_ms >= s->message_life_ms) {
        s->message[0] = 0;
        s->message_is_error = false;
    }
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
        int64_t start = (int64_t)i * per;
        int64_t end = start + per;
        if (end > au.n) end = au.n;
        float peak = 0;
        for (int64_t k = start; k < end; k++) {
            float a = fabsf(au.samples[k]);
            if (a > peak) peak = a;
        }
        p[i] = peak;
    }
    /* normalise so a quiet recording still shows its shape */
    float mx = 0;
    for (int i = 0; i < NBINS; i++) if (p[i] > mx) mx = p[i];
    if (mx > 1e-5f) for (int i = 0; i < NBINS; i++) p[i] = p[i] / mx;
    s->peaks = p;
    s->n_peaks = NBINS;
    s->playhead = s->playing ? s->playhead : 0.0;
    osa_audio_free(&au);
    return true;
}

/* ---- transcription ------------------------------------------------ */

void shell_refresh_phonetic(shell_state *s) {
    s->phonetic[0] = 0;
    if (!s->show_phonetic) return;
    if (!s->translation[0]) return;
    osa_app_pronounce(s->translation, s->lang_dst, s->phonetic, sizeof s->phonetic);
}

bool shell_run_transcribe(shell_state *s) {
    if (!s || !s->audio_path[0]) { shell_set_message(s,true,"%s",tt(s->lang,"msg.nofile")); return false; }
    if (!s->models_ready) { shell_set_message(s,true,"%s",tt(s->lang,"models.required")); return false; }
    return shell_start_job(s,SHELL_JOB_TRANSCRIBE,s->audio_path);
}
bool shell_select_audio(shell_state *s,const char *path){
    if(!s||!path||!*path)return false;
    if(!s->models_ready){shell_set_message(s,true,"%s",tt(s->lang,"models.required"));return false;}
    osa_strlcpy(s->audio_path,path,sizeof s->audio_path);
    s->transcript[0]=s->translation[0]=s->correction[0]=s->transcript_id[0]=0;free(s->peaks);s->peaks=NULL;s->n_peaks=0;
    return shell_start_job(s,SHELL_JOB_LOAD_AUDIO,path);
}
bool shell_delete_model(shell_state *s,const char *id){return shell_start_job(s,SHELL_JOB_DELETE,id);}
bool shell_sync_models(shell_state *s){s->sync_valid=false;s->sync_done=s->sync_total=0;return shell_start_job(s,SHELL_JOB_SYNC,NULL);}
void shell_refresh_models_dir(shell_state *s){if(s&&s->app)osa_app_models_dir(s->app,s->models_dir,sizeof s->models_dir);}
void shell_set_models_dir(shell_state *s,const char *path){
    if(!s||!s->app||!path||!*path)return;
    osa_error err;osa_err_ok(&err);
    if(!osa_app_set_models_dir(s->app,path,&err)){shell_set_message(s,true,"%s",err.msg);return;}
    shell_refresh_models_dir(s);
    shell_set_message(s,false,tt(s->lang,"settings.dirset"),s->models_dir);
}
bool shell_activate_model(shell_state *s,const char *id){return shell_start_job(s,SHELL_JOB_ACTIVATE,id);}
bool shell_load_transcript(shell_state *s, const char *id, const char *text,
                           const char *lang, float confidence, double duration) {
    if (!text || !*text) return false;
    if (id) osa_strlcpy(s->transcript_id, id, sizeof s->transcript_id);
    osa_strlcpy(s->transcript, text, sizeof s->transcript); osa_strlcpy(s->correction, text, sizeof s->correction);
    osa_strlcpy(s->detected_lang, lang ? lang : "", sizeof s->detected_lang);
    s->transcript_conf = confidence; s->transcript_dur = duration; s->transcript_ms = 0; s->translation[0] = 0;
    for(int i=0;i<g_hist_n;i++)if(osa_streq(g_hist_rows[i].id,id)){osa_strlcpy(s->translation,g_hist_rows[i].translation,sizeof s->translation);break;}
    shell_refresh_phonetic(s); return true;
}
static void translate_impl(shell_state *s, bool announce) {
    (void)announce;
    if (!s || !s->app) return;
    if (!s->tr_in[0]) { shell_set_message(s,true,"%s",tt(s->lang,"translate.emptyinput")); return; }
    if (!s->models_ready) { shell_set_message(s,true,"%s",tt(s->lang,"models.required")); return; }
    shell_start_job(s,SHELL_JOB_TRANSLATE,s->auto_detect?"auto":s->lang_src);
}
void shell_run_translate(shell_state *s) { translate_impl(s,true); }
void shell_run_translate_silent(shell_state *s) { (void)s; /* never infer on paint/keystroke/history selection */ }

/* ---- sidebar ------------------------------------------------------ */

/* Nav icons are drawn as small shapes rather than font glyphs: Vazirmatn has
 * no icon range, and a missing glyph turns the whole navigation into boxes.
 * Each icon fits a 20x20 box, centred on (cx, cy). */
static void draw_nav_icon(ui_context *u, shell_screen sc, float cx, float cy, osa_color col) {
    float s = u->L.scale;
    switch (sc) {
        case SCREEN_TRANSCRIBE:                     /* document with lines */
            c_stroke_round(u->c, rectf(cx - 6 * s, cy - 7 * s, 12 * s, 15 * s),
                           2.5f * s, 1.4f * s, col);
            for (int i = 0; i < 3; i++)
                c_fill(u->c, rectf(cx - 3 * s, cy - 3.5f * s + i * 3.2f * s, 6.5f * s,
                                   1.2f * s), col);
            break;
        case SCREEN_LIVE:                           /* microphone */
            c_fill_round(u->c, rectf(cx - 3 * s, cy - 8 * s, 6 * s, 10 * s), 3 * s, col);
            c_arc(u->c, cx, cy - 1 * s, 6 * s, 1.4f * s, 0.35f, 2.79f, col);
            c_fill(u->c, rectf(cx - 0.7f * s, cy + 5 * s, 1.4f * s, 3.5f * s), col);
            c_fill(u->c, rectf(cx - 3.5f * s, cy + 8 * s, 7 * s, 1.4f * s), col);
            break;
        case SCREEN_TRANSLATE:                      /* two arrows */
            c_fill(u->c, rectf(cx - 8 * s, cy - 4.5f * s, 12 * s, 1.6f * s), col);
            c_line(u->c, cx + 3 * s, cy - 6.5f * s, cx + 7 * s, cy - 3.7f * s, 1.8f * s, col);
            c_line(u->c, cx + 7 * s, cy - 3.7f * s, cx + 3 * s, cy - 1 * s, 1.8f * s, col);
            c_fill(u->c, rectf(cx - 4 * s, cy + 3 * s, 12 * s, 1.6f * s), col);
            c_line(u->c, cx - 3 * s, cy + 6.5f * s, cx - 7 * s, cy + 3.8f * s, 1.8f * s, col);
            c_line(u->c, cx - 7 * s, cy + 3.8f * s, cx - 3 * s, cy + 1.2f * s, 1.8f * s, col);
            break;
        case SCREEN_HISTORY:                        /* clock */
            c_circle_stroke(u->c, cx, cy, 7.5f * s, 1.5f * s, col);
            c_fill(u->c, rectf(cx - 0.8f * s, cy - 4.5f * s, 1.6f * s, 5 * s), col);
            c_fill(u->c, rectf(cx - 0.8f * s, cy - 0.8f * s, 4.5f * s, 1.6f * s), col);
            break;
        case SCREEN_MODELS:                         /* stacked layers */
            for (int i = 0; i < 3; i++) {
                float yy = cy - 5 * s + i * 4 * s;
                c_line(u->c, cx - 7 * s, yy, cx, yy - 3 * s, 1.5f * s, col);
                c_line(u->c, cx, yy - 3 * s, cx + 7 * s, yy, 1.5f * s, col);
                c_line(u->c, cx - 7 * s, yy, cx, yy + 2.6f * s, 1.5f * s, col);
                c_line(u->c, cx + 7 * s, yy, cx, yy + 2.6f * s, 1.5f * s, col);
            }
            break;
        case SCREEN_TRAINING:                       /* rising bars */
            c_fill_round(u->c, rectf(cx - 7 * s, cy + 1 * s, 3.6f * s, 6 * s), 1.2f * s, col);
            c_fill_round(u->c, rectf(cx - 1.8f * s, cy - 3 * s, 3.6f * s, 10 * s), 1.2f * s, col);
            c_fill_round(u->c, rectf(cx + 3.4f * s, cy - 7 * s, 3.6f * s, 14 * s), 1.2f * s, col);
            break;
        case SCREEN_EVALUATION:                     /* gauge */
            c_arc(u->c, cx, cy + 2 * s, 7.5f * s, 1.5f * s, 3.35f, 6.07f, col);
            c_line(u->c, cx, cy + 2 * s, cx + 4 * s, cy - 3.5f * s, 1.8f * s, col);
            c_circle(u->c, cx, cy + 2 * s, 1.6f * s, col);
            break;
        case SCREEN_SETTINGS:                       /* gear */
            c_circle_stroke(u->c, cx, cy, 4.6f * s, 1.5f * s, col);
            for (int i = 0; i < 8; i++) {
                float a = (float)i * 0.7853982f;
                c_line(u->c, cx + cosf(a) * 6 * s, cy + sinf(a) * 6 * s,
                       cx + cosf(a) * 8.2f * s, cy + sinf(a) * 8.2f * s, 1.6f * s, col);
            }
            break;
        case SCREEN_ABOUT:                          /* info */
            c_circle_stroke(u->c, cx, cy, 7.5f * s, 1.5f * s, col);
            c_fill(u->c, rectf(cx - 0.8f * s, cy - 1 * s, 1.6f * s, 5.5f * s), col);
            c_circle(u->c, cx, cy - 3.6f * s, 1.2f * s, col);
            break;
        default:
            c_circle(u->c, cx, cy, 3 * s, col);
            break;
    }
}

static void draw_sidebar(shell_state *s, ui_context *u, osa_rect r) {
    c_fill(u->c, r, u->t.sidebar);
    /* 1px rule on the content edge */
    c_fill(u->c, rectf(u->L.rtl ? r.x : r.x + r.w - 1, r.y, 1, r.h), u->t.border_soft);

    float pad = 16 * u->L.scale;
    float y = r.y + 20 * u->L.scale;

    /* brand: accent tile + name + tagline */
    float box = 38 * u->L.scale;
    osa_rect logo_r = rectf(u->L.rtl ? r.x + r.w - pad - box : r.x + pad, y, box, box);
    c_fill_round(u->c, logo_r, box * 0.3f, u->t.accent);
    /* a stylised sound wave inside the mark */
    for (int i = 0; i < 3; i++) {
        float h = (i == 1 ? 16.0f : (i == 0 ? 9.0f : 11.0f)) * u->L.scale;
        c_fill_round(u->c, rectf(logo_r.x + box * (0.28f + 0.2f * i),
                                 logo_r.y + (box - h) * 0.5f,
                                 2.6f * u->L.scale, h),
                     1.3f * u->L.scale, rgba(255, 255, 255, 230));
    }
    float name_x = u->L.rtl ? r.x + pad : logo_r.x + box + 12 * u->L.scale;
    float name_w = r.w - box - pad * 3;
    ui_label(u, rectf(name_x, y + 1 * u->L.scale, name_w, 20 * u->L.scale),
             "OpenSpeech AI", UI_FS_H2 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    ui_label(u, rectf(name_x, y + 21 * u->L.scale, name_w, 16 * u->L.scale),
             tt(s->lang, "app.tagline"), UI_FS_TINY * u->L.scale, u->t.text_faint,
             u->L.rtl ? 2 : 0);
    y += box + 22 * u->L.scale;

    /* navigation */
    float item_h = 40 * u->L.scale;
    float gap = 3 * u->L.scale;
    for (int i = 0; i < SCREEN_COUNT; i++) {
        osa_rect ir = rectf(r.x + 10 * u->L.scale, y, r.w - 20 * u->L.scale, item_h);
        if (y + item_h > r.y + r.h - 120 * u->L.scale) break;
        bool selected = (s->screen == (shell_screen)i);
        bool allowed=screen_allowed(s,(shell_screen)i);
        if (allowed && ui_row(u, 1000 + i, ir, selected)) {
            s->screen = (shell_screen)i;
            s->message[0] = 0;
            u->popup_id = 0;
            if (s->screen == SCREEN_HISTORY) {
                free(s->history_json);
                s->history_json = osa_app_history_json(s->app, 100, NULL);
            } else if (s->screen == SCREEN_MODELS) {
                free(s->models_json);
                s->models_json = osa_app_models_json(s->app, NULL);
            }
        }
        bool hov = rect_has(ir, u->in.mx, u->in.my);
        osa_color fg = !allowed ? u->t.text_faint : selected ? u->t.accent : (hov ? u->t.text : u->t.text_dim);
        float ix = u->L.rtl ? ir.x + ir.w - 22 * u->L.scale : ir.x + 22 * u->L.scale;
        draw_nav_icon(u, (shell_screen)i, ix, ir.y + ir.h * 0.5f, fg);
        osa_rect lr = u->L.rtl ? rectf(ir.x + 10 * u->L.scale, ir.y,
                                       ir.w - 46 * u->L.scale, ir.h)
                               : rectf(ir.x + 44 * u->L.scale, ir.y,
                                       ir.w - 54 * u->L.scale, ir.h);
        ui_label(u, lr, tt(s->lang, nav_label_key((shell_screen)i)),
                 UI_FS_BODY * u->L.scale, !allowed ? u->t.text_faint : selected ? u->t.text : u->t.text_dim,
                 u->L.rtl ? 2 : 0);
        /* keyboard-free nav: the row handles the click, not the label */
        y += item_h + gap;
    }

    /* footer: privacy badge + version. This is the promise of the product, so it
     * gets a permanent, quiet slot rather than a toast. */
    float bottom = r.y + r.h - 18 * u->L.scale;
    float bh = 56 * u->L.scale;
    osa_rect badge = rectf(r.x + 12 * u->L.scale, bottom - bh, r.w - 24 * u->L.scale, bh);
    c_fill_round(u->c, badge, u->L.radius_sm, u->t.panel_alt);
    c_stroke_round(u->c, badge, u->L.radius_sm, 1.0f, u->t.border_soft);
    float dx = u->L.rtl ? badge.x + badge.w - 18 * u->L.scale : badge.x + 18 * u->L.scale;
    c_circle(u->c, dx, badge.y + 20 * u->L.scale, 7 * u->L.scale,
             col_alpha(u->t.success, 0.18f));
    c_circle(u->c, dx, badge.y + 20 * u->L.scale, 3.6f * u->L.scale, u->t.success);
    float tx = u->L.rtl ? badge.x + 10 * u->L.scale : badge.x + 32 * u->L.scale;
    float tw = badge.w - 44 * u->L.scale;
    ui_label(u, rectf(tx, badge.y + 10 * u->L.scale, tw, 18 * u->L.scale),
             tt(s->lang, "status.offline"), UI_FS_SMALL * u->L.scale, u->t.success,
             u->L.rtl ? 2 : 0);
    char up[96];
    snprintf(up, sizeof up, "%s  ·  %s %s", tt(s->lang, "status.noupload"),
             tt(s->lang, "about.version"), OSAI_APP_VERSION);
    ui_label(u, rectf(tx, badge.y + 28 * u->L.scale, tw, 16 * u->L.scale),
             up, UI_FS_TINY * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
}

/* ---- top bar ------------------------------------------------------ */

osa_rect shell_draw_chrome(shell_state *s, ui_context *u, osa_rect r, const char *title_key,
                           const char *subtitle_key) {
    c_fill(u->c, r, u->t.bg);

    float pad = 24 * u->L.scale;
    /* Right-hand cluster: status chip, language switch, theme button. The title
     * stops short of it, otherwise the subtitle runs underneath the pills as
     * soon as the window narrows. */
    float chip_w = 118 * u->L.scale;
    float lang_w = 78 * u->L.scale;
    float theme_w = 34 * u->L.scale;
    float cluster_w = chip_w + lang_w + theme_w + 20 * u->L.scale;
    float title_w = r.w - pad * 2 - cluster_w - 16 * u->L.scale;
    if (title_w < 140 * u->L.scale) title_w = 140 * u->L.scale;
    float title_x = u->L.rtl ? r.x + pad + cluster_w + 16 * u->L.scale : r.x + pad;
    ui_label(u, rectf(title_x, r.y + 9 * u->L.scale, title_w, 24 * u->L.scale),
             tt(s->lang, title_key), UI_FS_H1 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    /* The subtitle line reports the active model in every category, so the top
     * of the window always answers "which model is running?" — including right
     * after an activation, without opening the Models screen. */
    {
        osa_app_status a = s->status_cache;
        char head[600];
        if (a.asr_model[0] || a.mt_model[0] || a.lid_model[0]) {
            const char *none = tt(s->lang, "models.notactive");
            snprintf(head, sizeof head, tt(s->lang, "models.activehead"),
                     a.asr_ready ? a.asr_model : none,
                     a.mt_ready ? a.mt_model : none,
                     a.lid_ready ? a.lid_model : none);
            ui_label(u, rectf(title_x, r.y + 33 * u->L.scale, title_w, 18 * u->L.scale),
                     head, UI_FS_SMALL * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
        } else {
            ui_label(u, rectf(title_x, r.y + 33 * u->L.scale, title_w, 18 * u->L.scale),
                     tt(s->lang, subtitle_key), UI_FS_SMALL * u->L.scale, u->t.text_dim,
                     u->L.rtl ? 2 : 0);
        }
    }

    float right = u->L.rtl ? r.x + pad : r.x + r.w - pad;
    float chip_h = 28 * u->L.scale;
    float cy = r.y + (r.h - chip_h) * 0.5f;

    /* model readiness */
    osa_app_status st = s->status_cache;
    bool all_ready=st.asr_ready&&st.mt_ready&&st.lid_ready;
    osa_rect chip = rectf(u->L.rtl ? right : right - chip_w, cy, chip_w, chip_h);
    if (s->downloading) {
        char pct[32], loc[64], label[128];
        snprintf(pct, sizeof pct, "%d%%", (int)(s->dl_progress * 100.0 + 0.5));
        text_localize_digits(pct, s->lang == UI_FA ? "fa" : "en", loc, sizeof loc);
        snprintf(label, sizeof label, tt(s->lang, "status.downloading"), loc);
        ui_status_chip(u, chip, label, u->t.accent);
    } else if (s->activating) {
        ui_status_chip(u, chip, tt(s->lang, "status.activating"), u->t.accent);
    } else {
        ui_status_chip(u, chip, tt(s->lang, s->restoring?"status.checking":all_ready ? "status.modeready" : "status.nomodel"),
                       all_ready ? u->t.success : u->t.warning);
    }
    right += u->L.rtl ? chip_w + 8 * u->L.scale : -(chip_w + 8 * u->L.scale);

    /* FA / EN switch */
    osa_rect lr = rectf(u->L.rtl ? right : right - lang_w, cy, lang_w, chip_h);
    c_fill_round(u->c, lr, chip_h * 0.5f, u->t.panel_alt);
    c_stroke_round(u->c, lr, chip_h * 0.5f, 1.0f, u->t.border_soft);
    float half = (lang_w - 6) * 0.5f;
    osa_rect fa_r = rectf(lr.x + 3, lr.y + 3, half, chip_h - 6);
    osa_rect en_r = rectf(lr.x + 3 + half, lr.y + 3, half, chip_h - 6);
    if (s->lang == UI_FA) c_fill_round(u->c, fa_r, (chip_h - 6) * 0.5f, u->t.accent);
    else c_fill_round(u->c, en_r, (chip_h - 6) * 0.5f, u->t.accent);
    ui_label(u, fa_r, "FA", UI_FS_SMALL * u->L.scale,
             s->lang == UI_FA ? u->t.on_accent : u->t.text_dim, 1);
    ui_label(u, en_r, "EN", UI_FS_SMALL * u->L.scale,
             s->lang == UI_EN ? u->t.on_accent : u->t.text_dim, 1);
    if (rect_has(fa_r, u->in.mx, u->in.my) && u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        s->lang = UI_FA;
        shell_save_prefs(s);
    }
    if (rect_has(en_r, u->in.mx, u->in.my) && u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        s->lang = UI_EN;
        shell_save_prefs(s);
    }
    right += u->L.rtl ? lang_w + 8 * u->L.scale : -(lang_w + 8 * u->L.scale);

    /* theme button */
    osa_rect th = rectf(u->L.rtl ? right : right - theme_w, cy, theme_w, chip_h);
    bool hov = rect_has(th, u->in.mx, u->in.my);
    c_fill_round(u->c, th, chip_h * 0.5f,
                 hov ? u->t.panel_hover : u->t.panel_alt);
    c_stroke_round(u->c, th, chip_h * 0.5f, 1.0f, u->t.border_soft);
    {
        float ccx = th.x + th.w * 0.5f, ccy = th.y + th.h * 0.5f;
        if (s->dark) {
            /* moon: a disc with a bite taken out */
            c_circle(u->c, ccx, ccy, 6.4f * u->L.scale, u->t.text_dim);
            c_circle(u->c, ccx + 2.8f * u->L.scale, ccy - 2.6f * u->L.scale,
                     5.4f * u->L.scale, hov ? u->t.panel_hover : u->t.panel_alt);
        } else {
            c_circle(u->c, ccx, ccy, 5.0f * u->L.scale, u->t.warning);
            for (int k = 0; k < 8; k++) {
                float a = (float)k * 0.7853982f;
                c_line(u->c, ccx + cosf(a) * 7.0f * u->L.scale,
                       ccy + sinf(a) * 7.0f * u->L.scale,
                       ccx + cosf(a) * 9.0f * u->L.scale,
                       ccy + sinf(a) * 9.0f * u->L.scale,
                       1.6f * u->L.scale, u->t.warning);
            }
        }
    }
    if (hov && u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        s->dark = !s->dark;
        ui_set_theme(u, s->dark);
        shell_save_prefs(s);
    }

    c_fill(u->c, rectf(r.x, r.y + r.h - 1, r.w, 1), u->t.border_soft);
    return rectf(r.x, r.y + r.h, r.w, u->L.win_h - (r.y + r.h));
}

void shell_draw_toast(shell_state *s, ui_context *u, osa_rect area) {
    if (!s->message[0]) return;
    float pad = 20 * u->L.scale;
    float h = 42 * u->L.scale;
    float w = MINF(area.w - pad * 2, 560 * u->L.scale);
    osa_rect r = rectf(area.x + (area.w - w) * 0.5f, area.y + area.h - h - pad * 0.6f, w, h);
    osa_color base = s->message_is_error ? u->t.danger : u->t.success;
    c_soft_shadow(u->c, r, h * 0.5f, 12 * u->L.scale, 4 * u->L.scale, u->t.shadow);
    c_fill_round(u->c, r, h * 0.5f, col_mix(u->t.panel, base, 0.14f));
    c_stroke_round(u->c, r, h * 0.5f, 1, col_alpha(base, 0.5f));
    float dx = u->L.rtl ? r.x + r.w - 20 * u->L.scale : r.x + 20 * u->L.scale;
    c_circle(u->c, dx, r.y + h * 0.5f, 4 * u->L.scale, base);
    osa_rect tr = u->L.rtl ? rectf(r.x + 14 * u->L.scale, r.y, r.w - 42 * u->L.scale, h)
                           : rectf(r.x + 36 * u->L.scale, r.y, r.w - 44 * u->L.scale, h);
    ui_label(u, tr, s->message, UI_FS_BODY * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
}

/* =====================================================================
 *  Transcribe screen
 * ===================================================================== */

/* A metric tile inside the source pane footer: caption above, value below.
 * Compact, three per row, so the pane is never a box of empty space. */
static void metric_tile(ui_context *u, osa_rect r, const char *label, const char *value,
                        osa_color tint) {
    ui_label(u, rectf(r.x, r.y, r.w, 15 * u->L.scale), label, UI_FS_TINY * u->L.scale,
             u->t.text_faint, u->L.rtl ? 2 : 0);
    ui_label(u, rectf(r.x, r.y + 14 * u->L.scale, r.w, 20 * u->L.scale), value,
             UI_FS_BODY * u->L.scale, tint, u->L.rtl ? 2 : 0);
}

/* The file card: name, meta, the two actions and the waveform. When nothing is
 * loaded it becomes the drop zone, with a real instruction where the filename
 * would be — an empty card with two buttons and no sentence is the kind of
 * screen that makes an application feel unfinished. */
static void draw_file_card(shell_state *s, ui_context *u, osa_rect card) {
    ui_card(u, card);
    float pad = 18 * u->L.scale;
    osa_rect in = rect_inset(card, pad);
    bool have = s->audio_path[0] != 0;

    float tile = 48 * u->L.scale;
    osa_rect ir = rectf(u->L.rtl ? in.x + in.w - tile : in.x, in.y + 2 * u->L.scale,
                        tile, tile);
    c_fill_round(u->c, ir, tile * 0.3f,
                 have ? col_alpha(u->t.accent, 0.16f) : u->t.panel_alt);
    if (have) {
        for (int i = 0; i < 5; i++) {
            float hh = (6.0f + (i == 2 ? 17.0f : (i % 2 ? 10.0f : 14.0f))) * u->L.scale;
            c_fill_round(u->c, rectf(ir.x + tile * (0.2f + 0.13f * i),
                                     ir.y + (tile - hh) * 0.5f, 3.0f * u->L.scale, hh),
                         1.5f * u->L.scale, u->t.accent);
        }
    } else {
        /* a bold arrow pointing into the card */
        c_fill(u->c, rectf(ir.x + tile * 0.3f, ir.y + tile * 0.44f, tile * 0.32f,
                           2.4f * u->L.scale), u->t.text_faint);
        c_line(u->c, ir.x + tile * 0.5f, ir.y + tile * 0.3f, ir.x + tile * 0.66f,
               ir.y + tile * 0.46f, 2.2f * u->L.scale, u->t.text_faint);
        c_line(u->c, ir.x + tile * 0.5f, ir.y + tile * 0.62f, ir.x + tile * 0.66f,
               ir.y + tile * 0.46f, 2.2f * u->L.scale, u->t.text_faint);
        c_stroke_round(u->c, rectf(ir.x + tile * 0.28f, ir.y + tile * 0.56f,
                                   tile * 0.44f, tile * 0.2f),
                       2 * u->L.scale, 1.6f * u->L.scale, u->t.text_faint);
    }

    /* actions on the trailing side, side by side so they fit the card's height */
    float bw = 132 * u->L.scale, bh = 34 * u->L.scale;
    float bx = u->L.rtl ? in.x : in.x + in.w - bw;
    float bx2 = u->L.rtl ? in.x + bw + 8 * u->L.scale : in.x + in.w - bw * 2 - 8 * u->L.scale;
    /* primary action occupies the trailing slot, secondary sits next to it */
    if (ui_button_busy(u, 2002, rectf(bx, in.y + 2 * u->L.scale, bw, bh),
                       tt(s->lang, "btn.transcribe"), s->models_ready, s->working))
        shell_run_transcribe(s);
    if (ui_button(u, 2001, rectf(bx2, in.y + 2 * u->L.scale, bw, bh),
                  tt(s->lang, "btn.choose"), s->models_ready))
        s->request = SHELL_REQ_PICK_AUDIO;

    /* middle: name + meta line */
    float mx = u->L.rtl ? in.x + bw * 2 + 24 * u->L.scale : in.x + tile + 16 * u->L.scale;
    float mw = in.w - tile - bw * 2 - 56 * u->L.scale;
    if (have) {
        ui_label(u, rectf(mx, in.y + 1 * u->L.scale, mw, 22 * u->L.scale),
                 osa_path_basename(s->audio_path), UI_FS_H2 * u->L.scale, u->t.text,
                 u->L.rtl ? 2 : 0);
        char dur[32], dur_loc[64], raw[192], meta[256];
        ui_fmt_duration(s->transcript_dur, dur, sizeof dur);
        text_localize_digits(dur, s->lang == UI_FA ? "fa" : "en", dur_loc, sizeof dur_loc);
        const char *lname = s->detected_lang[0] ? shell_lang_name(s, s->detected_lang)
                                                : tt(s->lang, "transcribe.unknown");
        snprintf(raw, sizeof raw, "%s  ·  %s  ·  %s", dur_loc, lname,
                 tt(s->lang, s->transcript_id[0] ? "transcribe.done" : "transcribe.ready"));
        text_localize_digits(raw, s->lang == UI_FA ? "fa" : "en", meta, sizeof meta);
        ui_label(u, rectf(mx, in.y + 24 * u->L.scale, mw, 18 * u->L.scale), meta,
                 UI_FS_SMALL * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
    } else {
        ui_label(u, rectf(mx, in.y + 1 * u->L.scale, mw, 22 * u->L.scale),
                 tt(s->lang, "transcribe.none"), UI_FS_H2 * u->L.scale, u->t.text,
                 u->L.rtl ? 2 : 0);
        text_opts o = text_default_opts(UI_FS_SMALL * u->L.scale);
        o.max_width = mw;
        o.align = u->L.rtl ? 2 : 0;
        text_draw_box(u->c, u->reg, UI_FS_SMALL * u->L.scale, u->t.text_faint,
                      rectf(mx, in.y + 24 * u->L.scale, mw, 34 * u->L.scale),
                      tt(s->lang, "transcribe.sub"), o);
    }

    /* waveform strip along the bottom of the card */
    if (s->n_peaks > 0) {
        osa_rect wr = rectf(in.x, card.y + card.h - 48 * u->L.scale, in.w,
                            32 * u->L.scale);
        ui_waveform(u, wr, s->peaks, s->n_peaks, s->playhead, s->playing);
    }
}

/* A result pane: header, copy button, the text, optional آوانویسی strip and a
 * row of metrics. */
static void draw_result_pane(shell_state *s, ui_context *u, osa_rect r,
                             const char *title_key, int id, const char *text,
                             const char *phonetic, bool rtl_text, osa_color tint,
                             const char *foot_label, const char *foot_value) {
    ui_card(u, r);
    float pad = 16 * u->L.scale;
    c_fill_round(u->c, rectf(r.x + pad, r.y + 15 * u->L.scale, 3 * u->L.scale,
                             15 * u->L.scale), 1.5f * u->L.scale, tint);
    ui_label(u, rectf(r.x + pad + 10 * u->L.scale, r.y + 13 * u->L.scale,
                      r.w - pad * 2 - 70 * u->L.scale, 20 * u->L.scale),
             tt(s->lang, title_key), UI_FS_SMALL * u->L.scale, u->t.text_dim,
             u->L.rtl ? 2 : 0);
    if (text && *text) {
        osa_rect copy_r = rectf(u->L.rtl ? r.x + pad : r.x + r.w - pad - 54 * u->L.scale,
                                r.y + 9 * u->L.scale, 54 * u->L.scale, 26 * u->L.scale);
        if (ui_button_ghost(u, id, copy_r, tt(s->lang, "btn.copy"))) {
            osa_clipboard_set(text);
            shell_set_message(s, false, "%s", tt(s->lang, "msg.copied"));
        }
    }
    c_hline(u->c, r.x + pad, r.x + r.w - pad, r.y + 44 * u->L.scale, 1, u->t.border_soft);

    float foot_h = (foot_label && *foot_label) ? 56 * u->L.scale : 0;
    /* A transliteration of a short sentence wraps to two lines on a narrow
     * pane; charging one line made it collide with the caption above it. */
    float ph_h = (phonetic && *phonetic) ? 72 * u->L.scale : 0;
    osa_rect tr = rectf(r.x + pad, r.y + 56 * u->L.scale, r.w - pad * 2,
                        r.h - 68 * u->L.scale - foot_h - ph_h);
    if (tr.h < 24) tr.h = 24;
    if (text && *text) {
        text_opts o = text_default_opts(UI_FS_H2 * u->L.scale);
        o.max_width = tr.w;
        o.line_height = UI_FS_H2 * u->L.scale * 1.9f;
        o.rtl = rtl_text;
        o.align = rtl_text ? 2 : 0;
        text_draw_box(u->c, u->reg, UI_FS_H2 * u->L.scale, u->t.text, tr, text, o);
    } else {
        ui_label(u, rectf(tr.x, tr.y + tr.h * 0.42f, tr.w, 22 * u->L.scale),
                 tt(s->lang, "transcribe.empty"), UI_FS_BODY * u->L.scale,
                 u->t.text_faint, rtl_text ? 2 : 0);
    }

    if (ph_h > 0) {
        osa_rect pr = rectf(r.x + pad, r.y + r.h - foot_h - ph_h - 4 * u->L.scale,
                            r.w - pad * 2, ph_h - 8 * u->L.scale);
        c_fill_round(u->c, pr, u->L.radius_sm, u->t.accent2_soft);
        ui_label(u, rectf(pr.x + 12 * u->L.scale, pr.y + 2 * u->L.scale,
                          pr.w - 24 * u->L.scale, 15 * u->L.scale),
                 tt(s->lang, "transcribe.phonetic"), UI_FS_TINY * u->L.scale,
                 col_alpha(u->t.accent2, 0.95f), u->L.rtl ? 2 : 0);
        text_opts po = text_default_opts(UI_FS_BODY * u->L.scale);
        po.max_width = pr.w - 24 * u->L.scale;
        po.rtl = true;
        po.align = 2;
        po.max_lines = 2;
        po.line_height = UI_FS_BODY * u->L.scale * 1.45f;
        text_draw_box(u->c, u->reg, UI_FS_BODY * u->L.scale, u->t.text,
                      rectf(pr.x + 12 * u->L.scale, pr.y + 18 * u->L.scale,
                            pr.w - 24 * u->L.scale, pr.h - 20 * u->L.scale),
                      phonetic, po);
    }
    if (foot_h > 0) {
        float fy = r.y + r.h - foot_h + 4 * u->L.scale;
        c_hline(u->c, r.x + pad, r.x + r.w - pad, fy - 4 * u->L.scale, 1,
                u->t.border_soft);
        metric_tile(u, rectf(r.x + pad, fy, (r.w - pad * 2) * 0.5f, 34 * u->L.scale),
                    foot_label, foot_value, tint);
    }
}

/* A compact strip of the last few transcripts. It gives the screen a sense of
 * history and lets the user jump back without a detour through History, and it
 * fills the band above the docks that would otherwise be an empty rule. */
static void draw_recent_strip(shell_state *s, ui_context *u, osa_rect r) {
    /* One row: the caption sits at the leading edge, the chips flow away from
     * it. With no history the caption's row carries the hint instead, so the
     * band never looks like a rule with nothing on it. */
    float chip_h = 30 * u->L.scale;
    float y = r.y + (r.h - chip_h) * 0.5f;
    const char *cap = tt(s->lang, "transcribe.recent");
    float cap_w = text_measure(u->bold, UI_FS_SMALL * u->L.scale, cap) + 8 * u->L.scale;
    float cap_x = u->L.rtl ? r.x + r.w - cap_w : r.x;
    ui_label(u, rectf(cap_x, y, cap_w, chip_h), cap, UI_FS_SMALL * u->L.scale,
             u->t.text_faint, u->L.rtl ? 2 : 0);

    if (!s->history_json || !s->history_json[0]) {
        const char *hint = tt(s->lang, "transcribe.recenthint");
        ui_label(u, rectf(u->L.rtl ? r.x : r.x + cap_w + 16 * u->L.scale, y,
                          r.w - cap_w - 16 * u->L.scale, chip_h),
                 hint, UI_FS_TINY * u->L.scale, col_alpha(u->t.text_faint, 0.8f),
                 u->L.rtl ? 0 : 0);
        return;
    }

    shell_parse_history(s->history_json);
    float gap = 8 * u->L.scale;
    float x = u->L.rtl ? r.x + r.w - cap_w - 14 * u->L.scale : r.x + cap_w + 14 * u->L.scale;
    float limit = u->L.rtl ? r.x : r.x + r.w;
    int shown = 0;
    for (int i = 0; i < g_hist_n && shown < 5; i++) {
        hist_row *row = &g_hist_rows[i];
        char label[80];
        char cut[56];
        osa_strlcpy(cut, row->text, sizeof cut);
        snprintf(label, sizeof label, "%s%s", cut,
                 strlen(row->text) > sizeof cut - 1 ? "…" : "");
        float w = text_measure(u->bold, UI_FS_SMALL * u->L.scale, label) + 36 * u->L.scale;
        if (w > r.w * 0.28f) w = r.w * 0.28f;
        if (u->L.rtl) {
            if (x - w < limit) break;
            x -= w;
        } else {
            if (x + w > limit) break;
        }
        osa_rect cr = rectf(x, y, w, chip_h);
        bool hov = ui_hit(u, cr);
        c_fill_round(u->c, cr, chip_h * 0.5f, hov ? u->t.panel_hover : u->t.panel_alt);
        c_stroke_round(u->c, cr, chip_h * 0.5f, 1.0f,
                       hov ? u->t.border_strong : u->t.border_soft);
        ui_label(u, rectf(cr.x + 12 * u->L.scale, cr.y, cr.w - 24 * u->L.scale, cr.h),
                 label, UI_FS_SMALL * u->L.scale, hov ? u->t.text : u->t.text_dim,
                 u->L.rtl ? 2 : 0);
        if (hov && u->in.clicked && !u->consumed_click) {
            u->consumed_click = true;
            shell_load_transcript(s, row->id, row->text, row->lang, (float)row->conf,
                                  row->dur);
            shell_set_message(s, false, "%s", tt(s->lang, "transcribe.loaded"));
        }
        if (!u->L.rtl) x += w + gap;
        else x -= gap;
        shown++;
    }
}

/* The correction box. Corrections are deliberately not fed back into the model:
 * they are stored for the next training cycle (see osa_app_add_correction), and
 * the card says so, because a user who edits a transcript expects the model to
 * change and has to be told it will not — yet. */
static void draw_correction_card(shell_state *s, ui_context *u, osa_rect card) {
    ui_card(u, card);
    float p = 16 * u->L.scale;
    c_fill_round(u->c, rectf(card.x + p, card.y + 15 * u->L.scale, 3 * u->L.scale,
                             15 * u->L.scale), 1.5f * u->L.scale, u->t.warning);
    ui_label(u, rectf(card.x + p + 10 * u->L.scale, card.y + 13 * u->L.scale,
                      card.w - p * 2 - 240 * u->L.scale, 20 * u->L.scale),
             tt(s->lang, "fix.title"), UI_FS_SMALL * u->L.scale, u->t.text_dim,
             u->L.rtl ? 2 : 0);

    float bw = 168 * u->L.scale, bh = 32 * u->L.scale;
    float by = card.y + card.h - bh - 12 * u->L.scale;
    bool has = s->transcript_id[0] != 0;
    if (has) {
        osa_rect save = rectf(u->L.rtl ? card.x + p : card.x + card.w - p - bw, by, bw, bh);
        if (ui_button(u, 2200, save, tt(s->lang, "fix.save"), true)) {
            osa_error err;
            if (osa_app_add_correction(s->app, s->transcript_id, s->correction, &err))
                shell_set_message(s, false, "%s", tt(s->lang, "fix.saved"));
            else
                shell_set_message(s, true, "%s: %s", tt(s->lang, "msg.failed"), err.msg);
        }
        osa_rect reset = rectf(u->L.rtl ? card.x + p + bw + 8 * u->L.scale
                                        : card.x + card.w - p - bw * 2 - 8 * u->L.scale,
                               by, bw, bh);
        if (ui_button_ghost(u, 2201, reset, tt(s->lang, "fix.reset")))
            osa_strlcpy(s->correction, s->transcript, sizeof s->correction);
    }
    ui_label(u, rectf(u->L.rtl ? card.x + p + (has ? bw + 16 * u->L.scale : 0)
                               : card.x + p + (has ? bw * 0 + 0 : 0),
                      by, card.w - p * 2 - (has ? bw * 2 + 16 * u->L.scale : 0), bh),
             has ? tt(s->lang, "fix.note") : tt(s->lang, "fix.noresult"),
             UI_FS_TINY * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);

    osa_rect area = rectf(card.x + p, card.y + 40 * u->L.scale, card.w - p * 2,
                          card.h - 40 * u->L.scale - bh - 22 * u->L.scale);
    if (area.h < 24) area.h = 24;
    ui_field_hint(tt(s->lang, "fix.placeholder"));
    if (ui_text_area(u, 2202, area, s->correction, sizeof s->correction, 4000)) {
        /* nothing to recompute: the box is only a draft until it is saved */
    }
}

static void draw_transcribe(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 22 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);
    float gap = 14 * u->L.scale;

    float card_h = 132 * u->L.scale;
    draw_file_card(s, u, rectf(inner.x, inner.y, inner.w, card_h));

    float strip_y = inner.y + card_h + 8 * u->L.scale;
    draw_recent_strip(s, u, rectf(inner.x, strip_y, inner.w, 40 * u->L.scale));

    float top = strip_y + 40 * u->L.scale + gap;
    float avail_h = inner.y + inner.h - top - 8 * u->L.scale;
    if (avail_h < 120 * u->L.scale) avail_h = 120 * u->L.scale;
    /* A two-line transcript in a 630-pixel pane is a wall of空 space. The docks
     * keep a working height and the rest of the column goes to the correction
     * box, which is the one thing a user actually does after reading a result. */
    float fix_h = 0;
    if (avail_h > 380 * u->L.scale) {
        fix_h = avail_h * 0.30f;
        avail_h -= fix_h + gap;
    }
    float col_w = (inner.w - gap) * 0.5f;
    osa_rect col_a = rectf(u->L.rtl ? inner.x + col_w + gap : inner.x, top, col_w,
                           avail_h);
    osa_rect col_b = rectf(u->L.rtl ? inner.x : inner.x + col_w + gap, top, col_w,
                           avail_h);

    bool src_rtl = lang_rtl(s->detected_lang);
    char c_val[48] = "—", m_val[48] = "—", lab_c[64], lab_m[64];
    osa_strlcpy(lab_c, tt(s->lang, "transcribe.words"), sizeof lab_c);
    osa_strlcpy(lab_m, tt(s->lang, "transcribe.confidence"), sizeof lab_m);
    if (s->transcript[0]) {
        char raw[64];
        snprintf(raw, sizeof raw, "%d", s->transcript_words);
        text_localize_digits(raw, lt_fa(s) ? "fa" : "en", c_val, sizeof c_val);
        snprintf(raw, sizeof raw, "%.0f%%", s->transcript_conf * 100.0f);
        text_localize_digits(raw, lt_fa(s) ? "fa" : "en", m_val, sizeof m_val);
    }
    draw_result_pane(s, u, col_a, "transcribe.source", 2100, s->transcript, NULL,
                     src_rtl, u->t.accent, lab_c, c_val);
    bool tr_rtl = lang_rtl(s->lang_dst);
    char tgt_label[96], tgt_val[64];
    osa_strlcpy(tgt_label, tt(s->lang, "transcribe.target"), sizeof tgt_label);
    if (s->translation[0])
        snprintf(tgt_val, sizeof tgt_val, "%s",
                 shell_lang_name(s, s->lang_dst));
    else
        osa_strlcpy(tgt_val, "—", sizeof tgt_val);
    draw_result_pane(s, u, col_b, "transcribe.result", 2101, s->translation,
                     tr_rtl ? NULL : s->phonetic, tr_rtl, u->t.accent2, tgt_label,
                     tgt_val);

    if (fix_h > 0)
        draw_correction_card(s, u, rectf(inner.x, top + avail_h + gap, inner.w, fix_h));

    /* The آوانویسی switch belongs with the strip it controls: it sits on the
     * strip's caption row, so the label and the toggle are one control. */
    if (s->phonetic[0]) {
        float sw = 34 * u->L.scale, sh = 19 * u->L.scale;
        osa_rect knob = rectf(u->L.rtl ? col_b.x + 18 * u->L.scale
                                       : col_b.x + col_b.w - 18 * u->L.scale - sw,
                              col_b.y + col_b.h - 46 * u->L.scale, sw, sh);
        if (ui_toggle_switch(u, 2102, knob, &s->show_phonetic)) {
            shell_save_prefs(s);
            shell_refresh_phonetic(s);
        }
    }
}

/* ---- public draw entry point -------------------------------------- */

typedef struct shell_job {
    shell_job_kind kind; osa_app *app; char arg[OSAI_MAX_PATH]; char *input;
    osa_app_transcript tr; osa_app_translation mt; char *eval_json;
    float *peaks; int n_peaks; volatile int permille,cancel,done; bool ok; int rc;
    osa_error err; osa_thread th; volatile int64_t got,total;
    osa_sync_result sync;
} shell_job;
static void job_progress(int64_t got,int64_t total,void *ud){shell_job*j=ud;j->got=got;j->total=total;if(total>0)j->permille=(int)(got*1000/total);}
static void job_worker(void *ud){
    shell_job*j=ud;osa_err_ok(&j->err);osa_app_reset_cancel(j->app);
    switch(j->kind){
      case SHELL_JOB_RESTORE:j->ok=osa_app_restore_saved(j->app,&j->err);break;
      case SHELL_JOB_LOAD_AUDIO:{osa_audio au;if(osa_audio_load(j->arg,&au,&j->err)){enum{NBINS=260};j->peaks=calloc(NBINS,sizeof(float));if(j->peaks){int64_t per=au.n/NBINS;if(per<1)per=1;float mx=0;for(int i=0;i<NBINS;i++){int64_t a=(int64_t)i*per,b=a+per;if(b>au.n)b=au.n;float p=0;for(int64_t k=a;k<b;k++){float v=fabsf(au.samples[k]);if(v>p)p=v;}j->peaks[i]=p;if(p>mx)mx=p;}if(mx>1e-5f)for(int i=0;i<NBINS;i++)j->peaks[i]/=mx;j->n_peaks=NBINS;j->ok=true;}osa_audio_free(&au);}break;}
      case SHELL_JOB_TRANSCRIBE:j->ok=osa_app_transcribe_and_translate(j->app,j->arg,&j->tr,&j->mt,&j->err);break;
      case SHELL_JOB_TRANSLATE:j->ok=osa_app_translate(j->app,j->input,j->arg,&j->mt,&j->err);break;
      case SHELL_JOB_DOWNLOAD:j->rc=osa_app_download_pack(j->app,j->arg,job_progress,j,&j->cancel,&j->err);j->ok=j->rc==0;break;
      case SHELL_JOB_ACTIVATE:j->ok=osa_app_activate_model(j->app,j->arg,&j->err);break;
      case SHELL_JOB_EVALUATE:j->eval_json=osa_app_evaluate(j->app,&j->err);j->ok=j->eval_json!=NULL;break;
      case SHELL_JOB_DELETE:j->ok=osa_app_remove_model(j->app,j->arg,&j->err);break;
      case SHELL_JOB_SYNC:j->ok=osa_app_sync_models(j->app,&j->cancel,job_progress,j,&j->sync,&j->err);break;
    }
    if(osa_atomic_load(&j->cancel))j->ok=false;
    __sync_synchronize(); j->done=1;
}
bool shell_start_job(shell_state*s,shell_job_kind kind,const char*arg){
    if(!s||!s->app||s->job)return false;
    if(s->live&&kind!=SHELL_JOB_RESTORE){shell_set_message(s,true,"%s",tt(s->lang,"live.stopfirst"));return false;}
    if((kind==SHELL_JOB_TRANSCRIBE||kind==SHELL_JOB_TRANSLATE)&&!s->models_ready){shell_set_message(s,true,"%s",tt(s->lang,"models.required"));return false;}
    shell_job*j=calloc(1,sizeof*j);if(!j)return false;j->kind=kind;j->app=s->app;if(arg)osa_strlcpy(j->arg,arg,sizeof j->arg);
    if(kind==SHELL_JOB_TRANSLATE){j->input=osa_strdup(s->tr_in);if(!j->input){free(j);return false;}}
    if(!osa_thread_start(&j->th,job_worker,j)){free(j->input);free(j);return false;}
    s->job=j;s->working=kind!=SHELL_JOB_DOWNLOAD;s->downloading=kind==SHELL_JOB_DOWNLOAD;
    s->activating=kind==SHELL_JOB_ACTIVATE;s->syncing=kind==SHELL_JOB_SYNC;s->message[0]=0;
    if(s->activating)shell_set_message(s,false,"%s",tt(s->lang,"models.activating"));
    if(kind==SHELL_JOB_DELETE)shell_set_message(s,false,"%s",tt(s->lang,"models.deleting"));
    if(kind==SHELL_JOB_SYNC){s->sync_done=s->sync_total=0;shell_set_message(s,false,"%s",tt(s->lang,"settings.synchint"));}
    if(kind==SHELL_JOB_DOWNLOAD){s->dl_progress=0;s->dl_got=s->dl_total=0;osa_strlcpy(s->dl_pack,arg,sizeof s->dl_pack);}return true;
}
void shell_job_tick(shell_state*s){
    if(!s||!s->job)return;shell_job*j=s->job;
    if(j->kind==SHELL_JOB_DOWNLOAD){s->dl_progress=j->permille/1000.0;s->dl_got=j->got;s->dl_total=j->total;}
    if(j->kind==SHELL_JOB_SYNC){s->sync_done=(int)j->got;s->sync_total=(int)j->total;}if(!osa_atomic_load(&j->done))return;
    osa_thread_join(&j->th);s->job=NULL;s->working=false;s->downloading=false;s->activating=false;s->syncing=false;s->restoring=false;s->models_ready=osa_app_ready(s->app);s->status_valid=false;
    if(j->ok){
      switch(j->kind){
        case SHELL_JOB_RESTORE:osa_app_get_languages(s->app,s->lang_src,sizeof s->lang_src,s->lang_dst,sizeof s->lang_dst);s->style=osa_app_style(s->app);if(!s->models_ready)s->screen=SCREEN_MODELS;break;
        case SHELL_JOB_LOAD_AUDIO:free(s->peaks);s->peaks=j->peaks;j->peaks=NULL;s->n_peaks=j->n_peaks;shell_set_message(s,false,"%s",tt(s->lang,"msg.fileselected"));break;
        case SHELL_JOB_TRANSCRIBE:osa_strlcpy(s->transcript,j->tr.text,sizeof s->transcript);osa_strlcpy(s->correction,j->tr.text,sizeof s->correction);osa_strlcpy(s->translation,j->mt.text,sizeof s->translation);osa_strlcpy(s->detected_lang,j->tr.language,sizeof s->detected_lang);osa_strlcpy(s->transcript_id,j->tr.id,sizeof s->transcript_id);s->transcript_conf=(float)j->tr.confidence;s->transcript_rtf=j->tr.rtf;s->transcript_dur=j->tr.duration_s;s->transcript_ms=j->tr.proc_ms;s->transcript_words=j->tr.n_words;shell_refresh_phonetic(s);free(s->history_json);s->history_json=osa_app_history_json(s->app,100,NULL);shell_set_message(s,false,"%s",tt(s->lang,"msg.ok"));break;
        case SHELL_JOB_TRANSLATE:osa_strlcpy(s->tr_out,j->mt.text,sizeof s->tr_out);osa_strlcpy(s->tr_out_lang,j->mt.target_lang,sizeof s->tr_out_lang);s->tr_score=j->mt.confidence;s->tr_phonetic[0]=0;if(s->show_phonetic)osa_app_pronounce(j->mt.text,j->mt.target_lang,s->tr_phonetic,sizeof s->tr_phonetic);shell_set_message(s,false,"%s (%s → %s)",tt(s->lang,"translate.done"),shell_lang_name(s,j->mt.source_lang),shell_lang_name(s,j->mt.target_lang));break;
        case SHELL_JOB_DOWNLOAD:free(s->models_json);s->models_json=NULL;s->models_tab=0;shell_set_message(s,false,"%s",tt(s->lang,"models.installed_inactive"));break;
        case SHELL_JOB_ACTIVATE:{
            free(s->models_json);s->models_json=NULL;
            osa_app_get_languages(s->app,s->lang_src,sizeof s->lang_src,s->lang_dst,sizeof s->lang_dst);
            char name[128];osa_strlcpy(name,j->arg,sizeof name);
            osa_pack_info pk[32];int npk=osa_app_packs(s->app,pk,32);
            for(int i=0;i<npk;i++)if(osa_streq(pk[i].id,j->arg)){osa_strlcpy(name,pk[i].name,sizeof name);break;}
            osa_app_get_status(s->app,&s->status_cache);s->status_ms=osa_now_ms();s->status_valid=true;
            shell_set_message(s,false,tt(s->lang,"models.activated"),name);
            break;}
        case SHELL_JOB_EVALUATE:free(s->eval_json);s->eval_json=j->eval_json;j->eval_json=NULL;break;
        case SHELL_JOB_DELETE:{
            char name[128];osa_strlcpy(name,j->arg,sizeof name);
            osa_pack_info pk[32];int npk=osa_app_packs(s->app,pk,32);
            for(int i=0;i<npk;i++)if(osa_streq(pk[i].id,j->arg)){osa_strlcpy(name,pk[i].name,sizeof name);break;}
            free(s->models_json);s->models_json=NULL;s->delete_id[0]=0;
            osa_app_get_status(s->app,&s->status_cache);s->status_ms=osa_now_ms();s->status_valid=true;
            osa_app_get_languages(s->app,s->lang_src,sizeof s->lang_src,s->lang_dst,sizeof s->lang_dst);
            shell_set_message(s,false,tt(s->lang,"models.deleted"),name);
            break;}
        case SHELL_JOB_SYNC:{
            s->sync=j->sync;s->sync_valid=true;
            free(s->models_json);s->models_json=NULL;
            if(j->sync.added)s->models_tab=0;
            char a[16],b[16],c[16],na[48],nb[48],nc[48];
            snprintf(a,sizeof a,"%d",j->sync.added);snprintf(b,sizeof b,"%d",j->sync.existing);snprintf(c,sizeof c,"%d",j->sync.unknown);
            const char*dig=s->lang==UI_FA?"fa":"en";
            text_localize_digits(a,dig,na,sizeof na);text_localize_digits(b,dig,nb,sizeof nb);text_localize_digits(c,dig,nc,sizeof nc);
            if(!j->sync.added&&!j->sync.existing&&!j->sync.unknown)shell_set_message(s,false,"%s",tt(s->lang,"settings.syncnone"));
            else shell_set_message(s,false,tt(s->lang,"settings.syncdone"),na,nb,nc);
            break;}
      }
    }else{
        /* Keep the engine's own sentence (it is the only thing that explains a
         failed load) but lead with a Persian/English explanation for the three
         causes a user can actually act on. */
        const char*raw=j->err.msg[0]?j->err.msg:tt(s->lang,"msg.cancelled");
        const char*hint=NULL;
        if(strstr(raw,"memory")||strstr(raw,"bad_alloc")||strstr(raw,"free RAM"))hint=tt(s->lang,"err.memory");
        else if(strstr(raw,"cannot be opened")||strstr(raw,"missing"))hint=tt(s->lang,"err.modelfile");
        else if(strstr(raw,"integrity")||strstr(raw,"wrong file format"))hint=tt(s->lang,"err.integrity");
        const char *where=j->kind==SHELL_JOB_ACTIVATE?tt(s->lang,"models.loghint"):"";
        if(hint)shell_set_message(s,true,"%s: %s (%s)%s%s",tt(s->lang,"msg.failed"),hint,raw,where?" — ":"",where);
        else shell_set_message(s,true,"%s: %s%s%s",tt(s->lang,"msg.failed"),raw,where?" — ":"",where);
    }
    s->dl_pack[0]=0;s->dl_progress=0;s->dl_got=s->dl_total=0;free(j->input);free(j->peaks);free(j->eval_json);free(j);
}
void shell_cancel_jobs(shell_state*s){if(!s)return;osa_app_cancel(s->app);if(s->job){shell_job*j=s->job;__sync_lock_test_and_set(&j->cancel,1);osa_thread_join(&j->th);free(j->input);free(j->peaks);free(j->eval_json);free(j);s->job=NULL;}s->working=s->downloading=s->activating=s->syncing=s->restoring=false;}
/* Legacy entry points now share the same job queue; installation never happens on the UI thread. */
bool shell_download_start(shell_state*s,const char*id){return shell_start_job(s,SHELL_JOB_DOWNLOAD,id);}
void shell_download_tick(shell_state*s){shell_job_tick(s);}

void shell_draw(shell_state *s, osa_canvas *c, ui_context *u, const ui_input *in) {
    if (!s || !c || !u) return;
    s->frame++;
    shell_message_tick(s);
    shell_job_tick(s);
    s->models_ready = osa_app_ready(s->app);
    if(!s->status_valid || osa_now_ms()-s->status_ms>1000){osa_app_get_status(s->app,&s->status_cache);s->status_ms=(double)osa_now_ms();s->status_valid=true;}
    ui_frame_begin(u, c, in);

    ui_set_theme(u, s->dark);
    ui_set_rtl(u, s->lang == UI_FA);
    ui_set_scale(u, s->scale);

    canvas_clear(c, u->t.bg);

    /* sidebar + content split, mirrored for RTL */
    float sb_w = 232 * u->L.scale;
    osa_rect full = rectf(0, 0, (float)c->w, (float)c->h);
    osa_rect sidebar = ui_split_leading(&full, sb_w, u->L.rtl);
    draw_sidebar(s, u, sidebar);

    osa_rect body = shell_screen_chrome(s, u, full, s->screen);
    if (s->screen == SCREEN_TRANSCRIBE) draw_transcribe(s, u, body);
    else shell_draw_screen(s, u, body, in);

    shell_draw_toast(s, u, body);
    /* The drop-down list is drawn last so it is never covered by a card. The id
     * has to be captured first: the widget clears it when it reports a choice. */
    if (u->popup_id) {
        int pid = u->popup_id;
        int sel = ui_popup_draw(u);
        if (sel >= 0) shell_popup_commit(s, pid, sel);
    }
}
