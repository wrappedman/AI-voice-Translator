/* =====================================================================
 *  OpenSpeech AI — the remaining screens
 *
 *  Live, Translate, History, Models, Training, Evaluation, Settings, About.
 *  Each screen is a plain function that owns its own layout; the shell picks
 *  one per frame. Keeping them in a separate translation unit makes it easy to
 *  see which screen owns which state.
 *
 *  All application state lives in shell_state; the only module-local state is
 *  the live microphone handle, which cannot outlive the process anyway.
 * ===================================================================== */
#include "shell_internal.h"
#include "../core/osai_platform.h"
#include "../core/tts.h"
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

/* ---- helpers ------------------------------------------------------ */

static const char *kl(shell_state *s, const char *key) { return tt(s->lang, key); }

/* mt_style_key() returns "formal"/"casual"/"neutral"; the interface strings are
 * keyed style.<that>, so the lookup needs the prefix. */
static const char *style_label(shell_state *s, mt_style st) {
    char key[32];
    snprintf(key, sizeof key, "style.%s", mt_style_key(st));
    return kl(s, key);
}

static const char *lt(shell_state *s) { return s->lang == UI_FA ? "fa" : "en"; }

/* Localises a number string for display: Persian digits in the Persian UI. */
static void num_fa(shell_state *s, const char *in, char *out, size_t cap) {
    text_localize_digits(in, lt(s), out, cap);
}

/* A stat tile: label above, big value below. Used by the evaluation and
 * history summaries where a row of bare numbers otherwise reads as noise. */
static void stat_tile(ui_context *u, osa_rect r, const char *label, const char *value,
                      osa_color tint) {
    c_fill_round(u->c, r, u->L.radius_sm, u->t.panel_alt);
    c_stroke_round(u->c, r, u->L.radius_sm, 1.0f, u->t.border_soft);
    ui_label(u, rectf(r.x + 12 * u->L.scale, r.y + 8 * u->L.scale,
                      r.w - 24 * u->L.scale, 16 * u->L.scale),
             label, UI_FS_TINY * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
    /* Numbers get the display size; a word ("exemplar") would overflow the tile,
     * so it steps down a size and ellipsizes instead of bleeding out. */
    /* A multi-byte literal is not allowed in a char comparison, so the
     * multiplication sign is matched as a sequence. */
    bool numeric = true;
    for (const char *q = value; *q; q++) {
        if ((*q >= '0' && *q <= '9') || *q == '.' || *q == '%' || *q == '-' || *q == ' ')
            continue;
        if ((unsigned char)q[0] == 0xC3 && (unsigned char)q[1] == 0x97) { q++; continue; }
        numeric = false;
        break;
    }
    ui_label(u, rectf(r.x + 12 * u->L.scale, r.y + (numeric ? 24 : 26) * u->L.scale,
                      r.w - 24 * u->L.scale, 26 * u->L.scale),
             value, (numeric ? UI_FS_H1 : UI_FS_H2) * u->L.scale, tint,
             u->L.rtl ? 2 : 0);
}

/* ---- Live --------------------------------------------------------- */

/* Live capture state. One growing segment of audio is recognised again every
 * time it gets longer, and the text of the whole segment replaces the pane.
 *
 * Whisper is an offline segment recogniser, not a streaming decoder. A pass
 * needs enough context: short sliding windows produce stray words. Recognise
 * the whole growing segment, at most one pass at a time, and translate that
 * result on the same worker. The UI thread only applies finished results. */
#define LIVE_MIN_S      1.2        /* no pass before this much audio       */
#define LIVE_SEG_MAX_S  25.0       /* commit and start a new segment here  */

static struct {
    osa_mic *mic;
    struct live_job *job;            /* the pass in flight, or NULL */
    float    ring[OSAI_SR * 30];
    int64_t  n;
    double   since_emit;
    /* text already committed from earlier segments, and the tail currently
     * being recognised; the pane shows committed + partial */
    char     committed[16384];
    char     partial[8192];
    char     final_text[32768];
    char     translation[65536];
    bool     stopping;
} g_live;

static void live_reset(void) {
    g_live.n = 0;
    g_live.since_emit = 0;
    g_live.committed[0] = 0;
    g_live.partial[0] = 0;
    g_live.final_text[0] = 0;
    g_live.translation[0] = 0;
}

static void live_join(char *dst, size_t cap, const char *a, const char *b) {
    if (a && *a && b && *b) snprintf(dst, cap, "%s %s", a, b);
    else osa_strlcpy(dst, (a && *a) ? a : (b ? b : ""), cap);
}

/* ---- recognition worker -------------------------------------------
 * A pass recognises the whole segment, which takes seconds with the whisper
 * engine. Passes run on a worker thread so the window keeps painting: the pass
 * sees a private copy of the audio, one pass is in flight at a time, and its
 * result is applied on the UI thread by live_reap(). */
typedef struct live_job {
    float    *pcm;
    int64_t   n;
    osa_app  *app;
    char      text[32768];
    char      lang[8];
    char      translation[65536];
    float     conf;
    bool      ok, mt_ok;
    volatile int done;
    osa_thread th;
} live_job;

static void live_worker(void *ud) {
    live_job *j = (live_job *)ud;
    osa_error e;
    osa_err_ok(&e);
    j->ok = osa_app_recognize(j->app, j->pcm, j->n, j->text, sizeof j->text,
                              j->lang, sizeof j->lang, &j->conf, &e);
    if(j->ok&&j->text[0]){osa_app_translation mt;osa_error me;osa_err_ok(&me);j->mt_ok=osa_app_translate_live(j->app,j->text,j->lang,&mt,&me);if(j->mt_ok)osa_strlcpy(j->translation,mt.text,sizeof j->translation);}
    __sync_synchronize(); j->done = 1;
}

static bool live_start_pass(shell_state *s) {
    if (g_live.job) return false;
    if (g_live.n < (int64_t)(OSAI_SR * LIVE_MIN_S)) return false;
    live_job *j = (live_job *)calloc(1, sizeof *j);
    if (!j) return false;
    j->pcm = (float *)malloc((size_t)g_live.n * sizeof(float));
    if (!j->pcm) { free(j); return false; }
    memcpy(j->pcm, g_live.ring, (size_t)g_live.n * sizeof(float));
    j->n = g_live.n;
    j->app = s->app;
    if (!osa_thread_start(&j->th, live_worker, j)) { free(j->pcm); free(j); return false; }
    g_live.job = j;
    return true;
}

/* Shows the result of a finished pass. */
static void live_apply(shell_state *s, const live_job *j) {
    if (!j->ok || !j->text[0]) return;
    osa_strlcpy(g_live.partial, j->text, sizeof g_live.partial);
    char joined[32768]; live_join(joined,sizeof joined,g_live.committed,g_live.partial);
    if (osa_streq(joined, g_live.final_text)) return;
    osa_strlcpy(g_live.final_text,joined,sizeof g_live.final_text);
    if(j->mt_ok)osa_strlcpy(g_live.translation,j->translation,sizeof g_live.translation);
    s->live_words=0;for(const char*c=g_live.final_text;*c;c++)if(*c==' ')s->live_words++;if(g_live.final_text[0])s->live_words++;
}

/* Collects a finished pass. Joined here, on the UI thread, so the worker's
 * handle is never used after it has been released. */
static void live_finish_job(shell_state *s) {
    live_job *j = g_live.job;
    osa_thread_join(&j->th);
    live_apply(s, j);
    free(j->pcm);
    free(j);
    g_live.job = NULL;
}

static void live_reap(shell_state *s) {
    if (!g_live.job || !osa_atomic_load(&g_live.job->done)) return;
    live_finish_job(s);
    if(g_live.stopping){g_live.stopping=false;s->live=false;s->live_level=0;osa_strlcpy(s->translation,g_live.translation,sizeof s->translation);shell_set_message(s,!g_live.final_text[0],"%s",kl(s,g_live.final_text[0]?"live.done":"live.nothing"));}
}

/* A segment that has grown past the window is committed and the buffer starts
 * again from empty. Only called with no pass in flight, so the commit and the
 * pass cannot disagree about which text is partial. */
static void live_commit_if_long(shell_state *s) {
    if (g_live.n < (int64_t)(OSAI_SR * LIVE_SEG_MAX_S)) return;
    if (g_live.partial[0]) {
        char joined[32768];
        live_join(joined, sizeof joined, g_live.committed, g_live.partial);
        osa_strlcpy(g_live.committed, joined, sizeof g_live.committed);
        g_live.partial[0] = 0;
    }
    /* keep the last 2 s as the context lead-in for the next segment */
    int64_t keep = OSAI_SR * 2;
    if (keep > g_live.n) keep = g_live.n;
    memmove(g_live.ring, g_live.ring + (g_live.n - keep), (size_t)keep * sizeof(float));
    g_live.n = keep;
    (void)s;
}

static void live_stop(shell_state *s) {
    if(g_live.mic){osa_mic_close(g_live.mic);g_live.mic=NULL;}
    s->live_level=0;
    if(g_live.job){g_live.stopping=true;return;} /* reap on a later timer tick, never join on UI thread */
    if(live_start_pass(s)){g_live.stopping=true;return;}
    s->live=false;osa_strlcpy(s->translation,g_live.translation,sizeof s->translation);
    shell_set_message(s,!g_live.final_text[0],"%s",kl(s,g_live.final_text[0]?"live.done":"live.silent"));
}
void shell_live_shutdown(shell_state*s){
    if(g_live.mic){osa_mic_close(g_live.mic);g_live.mic=NULL;}
    if(g_live.job){live_job*j=g_live.job;osa_thread_join(&j->th);free(j->pcm);free(j);g_live.job=NULL;}
    g_live.stopping=false;s->live=false;s->live_level=0;
}

void shell_live_tick(shell_state *s, double dt) {
    if (!s->live) return;
    if (!g_live.mic) { live_reap(s); return; }
    s->live_time += dt;
    float buf[1600];
    int n;
    /* drain the device every tick: the reader is what re-queues finished
     * buffers, so not reading would stop capture after one buffer */
    while ((n = osa_mic_read(g_live.mic, buf, 1600)) > 0) {
        for (int i = 0; i < n && g_live.n < (int64_t)(sizeof g_live.ring / sizeof g_live.ring[0]); i++)
            g_live.ring[g_live.n++] = buf[i];
        if (g_live.n >= (int64_t)(sizeof g_live.ring / sizeof g_live.ring[0])) break;
    }
    s->live_level = osa_mic_level(g_live.mic);
    live_reap(s);
    if (!g_live.job) live_commit_if_long(s);
    g_live.since_emit += dt;
    if (g_live.since_emit >= 0.7 && !g_live.job) {
        g_live.since_emit = 0;
        live_start_pass(s);
    }
}

static void draw_live(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 22 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);

    /* hero: record button + level ring + caption */
    float hero_h = 226 * u->L.scale;
    osa_rect hero = rectf(inner.x, inner.y, inner.w, hero_h);
    ui_card(u, hero);
    float cx = hero.x + hero.w * 0.5f;
    float cy = hero.y + hero_h * 0.5f - 14 * u->L.scale;
    float R = 46 * u->L.scale;
    bool avail = osa_mic_available();

    if (s->live) {
        for (int i = 0; i < 3; i++) {
            float t = fmodf((float)(s->frame) * 0.012f + i / 3.0f, 1.0f);
            float rr = R * (1.0f + t * 0.6f);
            c_circle_stroke(u->c, cx, cy, rr, 2.0f * (1.0f - t) + 0.5f,
                            col_alpha(u->t.danger, 0.40f * (1.0f - t)));
        }
    }
    c_circle(u->c, cx, cy, R, s->live ? col_alpha(u->t.danger, 0.16f) : u->t.panel_alt);

    /* level meter: 32 radial segments, computed from the live_level field */
    int nseg = 32;
    for (int i = 0; i < nseg; i++) {
        float a = (float)i / nseg * 6.2831853f;
        bool on = s->live && s->live_level > (float)i / nseg;
        float rr = R * 1.22f;
        c_circle(u->c, cx + cosf(a) * rr, cy + sinf(a) * rr,
                 on ? 3.6f * u->L.scale : 2.4f * u->L.scale,
                 on ? u->t.success : u->t.border);
    }
    /* the button itself: a mic glyph drawn with shapes */
    c_circle(u->c, cx, cy, R * 0.62f, s->live ? u->t.danger : u->t.accent);
    {
        float sc = u->L.scale;
        osa_color w = u->t.on_accent;
        c_fill_round(u->c, rectf(cx - 4 * sc, cy - 13 * sc, 8 * sc, 15 * sc), 4 * sc, w);
        c_arc(u->c, cx, cy - 2 * sc, 9 * sc, 2.2f * sc, 0.3f, 2.84f, w);
        c_fill(u->c, rectf(cx - 1 * sc, cy + 8 * sc, 2 * sc, 5 * sc), w);
        c_fill(u->c, rectf(cx - 6 * sc, cy + 12 * sc, 12 * sc, 2.2f * sc), w);
    }
    osa_rect btn = rectf(cx - R * 0.62f, cy - R * 0.62f, R * 1.24f, R * 1.24f);
    if (ui_hit(u, btn) && u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        if (!s->live) {
            if (!s->models_ready) { shell_set_message(s,true,"%s",kl(s,"models.required")); }
            else if (!avail) {
                shell_set_message(s, true, "%s", kl(s, "msg.nomic"));
            } else {
                osa_error err;
                g_live.mic = osa_mic_open(OSAI_SR, &err);
                if (!g_live.mic) {
                    shell_set_message(s, true, "%s", err.msg);
                } else {
                    live_reset();
                    s->live = true;
                    s->live_time = 0;
                    s->live_words = 0;
                    shell_set_message(s, false, "%s", kl(s, "live.listening"));
                }
            }
        } else {
            live_stop(s);
        }
    }

    /* Session panel (leading corner) and target-language panel (trailing):
     * a wide card with a button in the middle and nothing else reads as a
     * placeholder, so the two corners carry the state the user cares about. */
    {
        float ip = 18 * u->L.scale;
        float panel_w = 148 * u->L.scale;
        osa_rect sess = rectf(u->L.rtl ? hero.x + hero.w - ip - panel_w : hero.x + ip,
                              hero.y + ip, panel_w, 62 * u->L.scale);
        c_fill_round(u->c, sess, u->L.radius_sm, u->t.panel_alt);
        char raw[32], loc[48];
        int secs = (int)s->live_time;
        snprintf(raw, sizeof raw, "%02d:%02d", secs / 60, secs % 60);
        num_fa(s, raw, loc, sizeof loc);
        ui_label(u, rectf(sess.x + 12 * u->L.scale, sess.y + 8 * u->L.scale,
                          sess.w - 24 * u->L.scale, 15 * u->L.scale),
                 kl(s, "live.session"), UI_FS_TINY * u->L.scale, u->t.text_faint,
                 u->L.rtl ? 2 : 0);
        ui_label(u, rectf(sess.x + 12 * u->L.scale, sess.y + 24 * u->L.scale,
                          sess.w - 24 * u->L.scale, 26 * u->L.scale),
                 loc, UI_FS_H1 * u->L.scale, s->live ? u->t.danger : u->t.text_dim,
                 u->L.rtl ? 2 : 0);

        osa_rect lng = rectf(u->L.rtl ? hero.x + ip : hero.x + hero.w - ip - panel_w,
                             hero.y + ip, panel_w, 62 * u->L.scale);
        c_fill_round(u->c, lng, u->L.radius_sm, u->t.panel_alt);
        ui_label(u, rectf(lng.x + 12 * u->L.scale, lng.y + 8 * u->L.scale,
                          lng.w - 24 * u->L.scale, 15 * u->L.scale),
                 kl(s, "live.targetlang"), UI_FS_TINY * u->L.scale, u->t.text_faint,
                 u->L.rtl ? 2 : 0);
        ui_label(u, rectf(lng.x + 12 * u->L.scale, lng.y + 24 * u->L.scale,
                          lng.w - 24 * u->L.scale, 26 * u->L.scale),
                 s->lang_dst[0] ? shell_lang_name(s, s->lang_dst) : kl(s, "lang.auto"),
                 UI_FS_H2 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    }

    ui_label(u, rectf(hero.x + 20 * u->L.scale, hero.y + hero_h - 52 * u->L.scale,
                      hero.w - 40 * u->L.scale, 22 * u->L.scale),
             s->live ? kl(s, "live.stop") : kl(s, "live.start"),
             UI_FS_H2 * u->L.scale, s->live ? u->t.danger : u->t.text, 1);
    char raw[64], loc[96];
    if (s->live) {
        int secs = (int)s->live_time;
        snprintf(raw, sizeof raw, "%02d:%02d", secs / 60, secs % 60);
    } else if (!avail) {
        snprintf(raw, sizeof raw, "%s", kl(s, "msg.nomic"));
    } else {
        snprintf(raw, sizeof raw, "%s", kl(s, "live.hint"));
    }
    num_fa(s, raw, loc, sizeof loc);
    ui_label(u, rectf(hero.x + 20 * u->L.scale, hero.y + hero_h - 28 * u->L.scale,
                      hero.w - 40 * u->L.scale, 18 * u->L.scale),
             loc, UI_FS_SMALL * u->L.scale, u->t.text_faint, 1);

    /* text panes: recognised speech on the leading side, translation trailing */
    float top = hero.y + hero_h + 14 * u->L.scale;
    float h = inner.y + inner.h - top - 4 * u->L.scale;
    if (h < 90 * u->L.scale) h = 90 * u->L.scale;
    float col_w = (inner.w - 14 * u->L.scale) * 0.5f;
    osa_rect a = rectf(u->L.rtl ? inner.x + col_w + 14 * u->L.scale : inner.x, top,
                       col_w, h);
    osa_rect b = rectf(u->L.rtl ? inner.x : inner.x + col_w + 14 * u->L.scale, top,
                       col_w, h);
    const char *txt[2] = { g_live.final_text, g_live.translation };
    const char *lbl[2] = { "live.recognized", "live.translated" };
    osa_rect *pane[2] = { &a, &b };
    for (int i = 0; i < 2; i++) {
        ui_card(u, *pane[i]);
        float p = 16 * u->L.scale;
        c_fill_round(u->c, rectf(pane[i]->x + p, pane[i]->y + 14 * u->L.scale,
                                 3 * u->L.scale, 16 * u->L.scale),
                     1.5f * u->L.scale, i == 0 ? u->t.accent : u->t.accent2);
        ui_label(u, rectf(pane[i]->x + p + 10 * u->L.scale, pane[i]->y + 12 * u->L.scale,
                          pane[i]->w - p * 2 - 10 * u->L.scale, 20 * u->L.scale),
                 kl(s, lbl[i]), UI_FS_SMALL * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
        c_hline(u->c, pane[i]->x + p, pane[i]->x + pane[i]->w - p,
                pane[i]->y + 40 * u->L.scale, 1, u->t.border_soft);
        osa_rect tr = rectf(pane[i]->x + p, pane[i]->y + 52 * u->L.scale,
                            pane[i]->w - p * 2, pane[i]->h - 68 * u->L.scale);
        if (txt[i][0]) {
            text_opts o = text_default_opts(UI_FS_H2 * u->L.scale);
            o.max_width = tr.w;
            o.line_height = UI_FS_H2 * u->L.scale * 1.85f;
            o.align = u->L.rtl ? 2 : 0;
            text_draw_box(u->c, u->reg, UI_FS_H2 * u->L.scale, u->t.text, tr, txt[i], o);
        } else {
            /* The two panes must not say the same thing twice: the source pane
             * explains how to start, the translation pane waits for it. */
            const char *head = s->live ? kl(s, "live.listening")
                             : i == 0 ? kl(s, "live.empty")
                                      : kl(s, "live.emptyout");
            const char *hint = s->live ? (i == 0 ? kl(s, "live.speaking")
                                                : kl(s, "live.waiting"))
                             : i == 0 ? kl(s, "live.emptyhint") : NULL;
            ui_label(u, rectf(tr.x, tr.y + tr.h * 0.34f, tr.w, 22 * u->L.scale), head,
                     UI_FS_BODY * u->L.scale, u->t.text_faint, 1);
            if (hint) {
                text_opts ho = text_default_opts(UI_FS_SMALL * u->L.scale);
                ho.max_width = tr.w * 0.8f;
                ho.align = 1;
                ho.max_lines = 2;
                text_draw_box(u->c, u->reg, UI_FS_SMALL * u->L.scale, u->t.text_faint,
                              rectf(tr.x + tr.w * 0.1f, tr.y + tr.h * 0.34f + 26 * u->L.scale,
                                    tr.w * 0.8f, 40 * u->L.scale),
                              hint, ho);
            }
        }
    }
}

/* ---- Translate ---------------------------------------------------- */

static void lang_picker(shell_state *s, ui_context *u, osa_rect r, int id,
                        const char *caption, const char *value, bool open);
static osa_lang_info g_mt_langs[128];
static int g_mt_lang_n;
static void fill_lang_items(shell_state *s, ui_context *u, bool allow_auto) {
    g_mt_lang_n=osa_app_languages(s->app,g_mt_langs,128);
    int total=g_mt_lang_n+(allow_auto?1:0), first=u->popup_scroll;
    if(first<0)first=0;
    if(first>total-12&&total>12)first=total-12;
    u->popup_scroll=first;u->popup_n=total;
    for(int i=0;i<12&&first+i<total;i++){
        int g=first+i;const char*label=(allow_auto&&g==0)?kl(s,"lang.auto"):shell_lang_name(s,g_mt_langs[g-(allow_auto?1:0)].code);
        osa_strlcpy(u->popup_items[i],label,sizeof u->popup_items[0]);
    }
}
static int lang_index(const char*code,bool allow_auto){if(allow_auto&&(!code||!*code||osa_streq(code,"auto")))return 0;for(int i=0;i<g_mt_lang_n;i++)if(osa_streq(code,g_mt_langs[i].code))return i+(allow_auto?1:0);return allow_auto?0:-1;}

/* One language selector. Rendered like the header of the web translators the
 * screen is modelled on: a small caption inside the control, the language in
 * bold under it, a chevron on the trailing edge. */
static void lang_picker(shell_state *s, ui_context *u, osa_rect r, int id,
                        const char *caption, const char *value, bool open) {
    bool hov = ui_hit(u, r);
    c_fill_round(u->c, r, u->L.radius_sm, hov || open ? u->t.panel_hover : u->t.panel_alt);
    c_stroke_round(u->c, r, u->L.radius_sm, open ? 1.6f : 1.0f,
                   open ? u->t.accent : (hov ? u->t.border_strong : u->t.border));
    float p = 14 * u->L.scale;
    ui_label(u, rectf(r.x + p, r.y + 8 * u->L.scale, r.w - p * 2 - 20 * u->L.scale,
                      14 * u->L.scale),
             caption, UI_FS_TINY * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
    ui_label(u, rectf(r.x + p, r.y + 23 * u->L.scale, r.w - p * 2 - 20 * u->L.scale,
                      22 * u->L.scale),
             value, UI_FS_H2 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    osa_rect ar = rectf(u->L.rtl ? r.x + 4 * u->L.scale : r.x + r.w - 22 * u->L.scale,
                        r.y, 18 * u->L.scale, r.h);
    ui_dropdown_arrow(u, ar);
    if (hov && u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        if(!s->models_ready){shell_set_message(s,true,"%s",kl(s,"models.required"));return;}
        u->popup_id = open ? 0 : id; u->popup_rect = r; u->popup_scroll=0; u->popup_n=0;
    }
    if(u->popup_id==id){bool src=(id==3100||id==8100);fill_lang_items(s,u,src);u->popup_sel=lang_index(src?s->lang_src:s->lang_dst,src);}
}


static void draw_lang_bar(shell_state *s, ui_context *u, osa_rect r) {
    float h = 54 * u->L.scale;
    float arrow_w = 36 * u->L.scale;
    float w = (r.w - arrow_w - 20 * u->L.scale) * 0.5f;
    osa_rect src_r = rectf(u->L.rtl ? r.x + w + arrow_w + 20 * u->L.scale : r.x, r.y, w, h);
    osa_rect dst_r = rectf(u->L.rtl ? r.x : r.x + w + arrow_w + 20 * u->L.scale, r.y, w, h);
    osa_rect arrow = rectf(r.x + w + 10 * u->L.scale, r.y + (h - arrow_w) * 0.5f,
                           arrow_w, arrow_w);

    lang_picker(s, u, src_r, 3100, kl(s, "lang.from"),
                s->lang_src[0] ? shell_lang_name(s, s->lang_src) : kl(s, "lang.auto"),
                u->popup_id == 3100);
    lang_picker(s, u, dst_r, 3101, kl(s, "lang.to"),
                s->lang_dst[0] ? shell_lang_name(s, s->lang_dst) : kl(s, "lang.auto"),
                u->popup_id == 3101);

    /* swap button */
    bool hov = ui_hit(u, arrow);
    c_fill_round(u->c, arrow, arrow_w * 0.5f, u->t.accent_soft);
    c_stroke_round(u->c, arrow, arrow_w * 0.5f, 1.0f,
                   col_alpha(u->t.accent, hov ? 0.8f : 0.4f));
    {
        float ax = arrow.x + arrow.w * 0.5f, ay = arrow.y + arrow.h * 0.5f;
        osa_color c1 = u->t.accent;
        c_fill(u->c, rectf(ax - 8 * u->L.scale, ay - 4.0f * u->L.scale,
                           14 * u->L.scale, 1.8f * u->L.scale), c1);
        c_line(u->c, ax + 2 * u->L.scale, ay - 6.5f * u->L.scale,
               ax + 7 * u->L.scale, ay - 3.1f * u->L.scale, 1.8f * u->L.scale, c1);
        c_line(u->c, ax + 7 * u->L.scale, ay - 3.1f * u->L.scale,
               ax + 2 * u->L.scale, ay + 0.3f * u->L.scale, 1.8f * u->L.scale, c1);
        c_fill(u->c, rectf(ax - 6 * u->L.scale, ay + 3.0f * u->L.scale,
                           14 * u->L.scale, 1.8f * u->L.scale), c1);
        c_line(u->c, ax - 2 * u->L.scale, ay + 6.5f * u->L.scale,
               ax - 7 * u->L.scale, ay + 3.1f * u->L.scale, 1.8f * u->L.scale, c1);
        c_line(u->c, ax - 7 * u->L.scale, ay + 3.1f * u->L.scale,
               ax - 2 * u->L.scale, ay - 0.3f * u->L.scale, 1.8f * u->L.scale, c1);
    }
    if (s->models_ready && !osa_streq(s->lang_src,"auto") && hov && u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        if(s->models_ready&&!osa_streq(s->lang_src,"auto")&&s->lang_src[0]){char tmp[8];osa_strlcpy(tmp,s->lang_src,8);osa_strlcpy(s->lang_src,s->lang_dst,8);osa_strlcpy(s->lang_dst,tmp,8);osa_error e;if(!osa_app_set_languages(s->app,s->lang_src,s->lang_dst,&e)){osa_strlcpy(s->lang_dst,s->lang_src,8);osa_strlcpy(s->lang_src,tmp,8);shell_set_message(s,true,"%s",e.msg);}else shell_save_prefs(s);}
    }

    /* second row: register on the leading side, آوانویسی switch on the trailing.
     * The two share the row, so the picker is sized against the switch group
     * rather than as a fixed share of the window — at 42% of an 1160-pixel row
     * it reached the switch and the label landed on top of it. */
    float ry = r.y + h + 10 * u->L.scale;
    const char *plabel0 = kl(s, "lang.phonetic");
    float lw0 = text_measure(u->reg, UI_FS_SMALL * u->L.scale, plabel0);
    float tg_gap = 10 * u->L.scale;
    float tg_w = lw0 + 40 * u->L.scale + tg_gap;
    float st_w = r.w * 0.42f;
    if (st_w > r.w - tg_w - 32 * u->L.scale) st_w = r.w - tg_w - 32 * u->L.scale;
    if (st_w < 180 * u->L.scale) st_w = 180 * u->L.scale;
    /* In RTL the picker also reads from the trailing edge, so it has to stop
     * short of the switch group instead of starting at the window edge — the
     * two occupied the same pixels. */
    osa_rect st_r = rectf(u->L.rtl ? r.x + r.w - tg_w - 12 * u->L.scale - st_w
                                   : r.x,
                          ry, st_w, 34 * u->L.scale);
    char reg[128];
    snprintf(reg, sizeof reg, "%s: %s", kl(s, "lang.register"), style_label(s, s->style));
    bool st_open = u->popup_id == 3102;
    if (ui_dropdown(u, 3102, st_r, reg, st_open)) {
        u->popup_id = st_open ? 0 : 3102;
        u->popup_rect = st_r;
        u->popup_n = MT_STYLE_COUNT;
        for (int i = 0; i < MT_STYLE_COUNT; i++)
            osa_strlcpy(u->popup_items[i], style_label(s, (mt_style)i),
                        sizeof u->popup_items[0]);
        u->popup_sel = (int)s->style;
    }

    /* آوانویسی toggle: label immediately left of its switch, on the trailing
     * edge. It used to be placed from the *leading* edge in RTL, which put the
     * label at the far side of the window from the control it describes. */
    float sw = 40 * u->L.scale, sh = 22 * u->L.scale;
    const char *plabel = plabel0;
    float lw = lw0;
    /* RTL: label on the right of the group, switch to its left (reading order
     * right→left). LTR: the mirror image. */
    float gx = r.x + r.w - tg_w;
    float tx = u->L.rtl ? gx + sw + tg_gap : gx;
    float knob_x = u->L.rtl ? gx : gx + lw + tg_gap;
    ui_label(u, rectf(tx, ry, lw, 34 * u->L.scale), plabel,
             UI_FS_SMALL * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
    osa_rect knob = rectf(knob_x, ry + (34 * u->L.scale - sh) * 0.5f, sw, sh);
    if (ui_toggle_switch(u, 3103, knob, &s->show_phonetic)) {
        shell_save_prefs(s); shell_refresh_phonetic(s);
    }
}

static void draw_translate(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 22 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);
    float toolbar_h = 98 * u->L.scale;

    draw_lang_bar(s, u, rectf(inner.x, inner.y, inner.w, toolbar_h));

    float top = inner.y + toolbar_h + 6 * u->L.scale;
    float total = inner.y + inner.h - top;
    float gap = 12 * u->L.scale;
    /* The input gets the larger share: it is the part with a cursor in it and
     * the part that grows when someone pastes a paragraph. On a tall window,
     * though, a text box four hundred pixels tall is a blank wall — both panes
     * stop growing at a readable size and the pair sits centred instead of
     * stretched to the floor. */
    float pair_max = 560 * u->L.scale;
    if (total > pair_max) {
        top += (total - pair_max) * 0.35f;
        total = pair_max;
    }
    float in_h = total * 0.56f;
    osa_rect in_r = rectf(inner.x, top, inner.w, in_h);
    osa_rect out_r = rectf(inner.x, top + in_h + gap, inner.w, total - in_h - gap);

    /* ---------- input ---------- */
    ui_card(u, in_r);
    float p = 16 * u->L.scale;
    c_fill_round(u->c, rectf(in_r.x + p, in_r.y + 15 * u->L.scale, 3 * u->L.scale,
                             15 * u->L.scale), 1.5f * u->L.scale, u->t.accent);
    ui_label(u, rectf(in_r.x + p + 10 * u->L.scale, in_r.y + 13 * u->L.scale,
                      in_r.w - p * 2 - 160 * u->L.scale, 20 * u->L.scale),
             kl(s, "translate.input"), UI_FS_SMALL * u->L.scale, u->t.text_dim,
             u->L.rtl ? 2 : 0);
    {
        char raw[64], loc[80];
        snprintf(raw, sizeof raw, "%d %s", (int)strlen(s->tr_in), kl(s, "translate.chars"));
        num_fa(s, raw, loc, sizeof loc);
        ui_label(u, rectf(in_r.x + p, in_r.y + 13 * u->L.scale,
                          in_r.w - p * 2 - 10 * u->L.scale, 20 * u->L.scale),
                 loc, UI_FS_TINY * u->L.scale, u->t.text_faint, u->L.rtl ? 0 : 2);
    }
    c_hline(u->c, in_r.x + p, in_r.x + in_r.w - p, in_r.y + 42 * u->L.scale, 1,
            u->t.border_soft);

    float btn_h = 36 * u->L.scale;
    osa_rect area = rectf(in_r.x + p, in_r.y + 50 * u->L.scale, in_r.w - p * 2,
                          in_r.h - 50 * u->L.scale - btn_h - 24 * u->L.scale);
    ui_field_hint(kl(s, "translate.placeholder"));
    bool changed = ui_text_area(u, 3200, area, s->tr_in, sizeof s->tr_in, 65000);
    if (changed) { s->tr_out[0]=0; s->tr_phonetic[0]=0; }

    float bw = 138 * u->L.scale;
    float by = in_r.y + in_r.h - btn_h - 14 * u->L.scale;
    osa_rect run_r = rectf(u->L.rtl ? in_r.x + p : in_r.x + in_r.w - p - bw, by, bw, btn_h);
    if (ui_button(u, 3201, run_r, kl(s, "btn.translate"), s->models_ready&&!s->working)) shell_run_translate(s);
    osa_rect clr_r = rectf(u->L.rtl ? in_r.x + p + bw + 8 * u->L.scale
                                    : in_r.x + in_r.w - p - bw * 2 - 8 * u->L.scale,
                           by, bw, btn_h);
    if (ui_button_ghost(u, 3202, clr_r, kl(s, "btn.clear"))) {
        s->tr_in[0] = 0;
        s->tr_out[0] = 0;
        s->tr_phonetic[0] = 0;
        u->focus_id = 3200;
    }

    /* ---------- output ---------- */
    ui_card(u, out_r);
    c_fill_round(u->c, rectf(out_r.x + p, out_r.y + 15 * u->L.scale, 3 * u->L.scale,
                             15 * u->L.scale), 1.5f * u->L.scale, u->t.accent2);
    ui_label(u, rectf(out_r.x + p + 10 * u->L.scale, out_r.y + 13 * u->L.scale,
                      out_r.w - p * 2 - 190 * u->L.scale, 20 * u->L.scale),
             kl(s, "translate.output"), UI_FS_SMALL * u->L.scale, u->t.text_dim,
             u->L.rtl ? 2 : 0);
    if (s->tr_out[0]) {
        float ow = 60 * u->L.scale;
        osa_rect copy_r = rectf(u->L.rtl ? out_r.x + p : out_r.x + out_r.w - p - ow,
                                out_r.y + 9 * u->L.scale, ow, 28 * u->L.scale);
        if (ui_button_ghost(u, 3203, copy_r, kl(s, "btn.copy"))) {
            osa_clipboard_set(s->tr_out);
            shell_set_message(s, false, "%s", kl(s, "msg.copied"));
        }
        osa_rect say_r = rectf(u->L.rtl ? out_r.x + p + ow + 6 * u->L.scale
                                        : out_r.x + out_r.w - p - ow * 2 - 6 * u->L.scale,
                               out_r.y + 9 * u->L.scale, ow, 28 * u->L.scale);
        if (ui_button_ghost(u, 3204, say_r, kl(s, "btn.listen"))) {
            const char *txt = s->tr_out;
            const char *lng = s->tr_out_lang[0] ? s->tr_out_lang : "en";
            shell_speak(s, txt, lng);
        }
    }
    c_hline(u->c, out_r.x + p, out_r.x + out_r.w - p, out_r.y + 42 * u->L.scale, 1,
            u->t.border_soft);

    bool ph = s->show_phonetic && s->tr_phonetic[0];
    float ph_h = ph ? 52 * u->L.scale : 0;
    osa_rect tr = rectf(out_r.x + p, out_r.y + 52 * u->L.scale, out_r.w - p * 2,
                        out_r.h - 64 * u->L.scale - ph_h);
    if (tr.h < 24) tr.h = 24;
    if (s->tr_out[0]) {
        text_opts o = text_default_opts(UI_FS_H2 * u->L.scale);
        o.max_width = tr.w;
        o.line_height = UI_FS_H2 * u->L.scale * 1.9f;
        /* The gutter follows the interface, not the text: a Latin translation in
         * the Persian UI still starts at the right edge, the way every other
         * block on the screen does. */
        o.rtl = text_is_rtl(s->tr_out);
        o.align = u->L.rtl ? 2 : 0;
        text_draw_box(u->c, u->reg, UI_FS_H2 * u->L.scale, u->t.text, tr, s->tr_out, o);
    } else {
        ui_label(u, rectf(tr.x, tr.y + 4 * u->L.scale, tr.w, 22 * u->L.scale),
                 kl(s, "translate.empty"), UI_FS_BODY * u->L.scale, u->t.text_faint,
                 u->L.rtl ? 2 : 0);
    }
    if (ph) {
        osa_rect pr = rectf(out_r.x + p, out_r.y + out_r.h - ph_h - 4 * u->L.scale,
                            out_r.w - p * 2, ph_h - 8 * u->L.scale);
        c_fill_round(u->c, pr, u->L.radius_sm, u->t.accent2_soft);
        ui_label(u, rectf(pr.x + 12 * u->L.scale, pr.y + 3 * u->L.scale,
                          pr.w - 24 * u->L.scale, 15 * u->L.scale),
                 kl(s, "transcribe.phonetic"), UI_FS_TINY * u->L.scale,
                 col_alpha(u->t.accent2, 0.95f), u->L.rtl ? 2 : 0);
        text_opts po = text_default_opts(UI_FS_BODY * u->L.scale);
        po.max_width = pr.w - 24 * u->L.scale;
        po.rtl = true;
        po.align = 2;
        text_draw_box(u->c, u->reg, UI_FS_BODY * u->L.scale, u->t.text,
                      rectf(pr.x + 12 * u->L.scale, pr.y + 18 * u->L.scale,
                            pr.w - 24 * u->L.scale, 24 * u->L.scale),
                      s->tr_phonetic, po);
    }

}

/* ---- History ------------------------------------------------------ */

static void draw_history(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 22 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);
    if (!s->history_json) s->history_json = osa_app_history_json(s->app, 100, NULL);

    /* summary strip */
    osa_app_status st;
    osa_app_get_status(s->app, &st);
    float tile_h = 62 * u->L.scale;
    float gap = 10 * u->L.scale;
    float tw = (inner.w - gap * 3) / 4;
    /* Four tiles, laid out from the leading edge so RTL mirrors the row. */
    char vloc[96];
    const char *labels[4] = { "history.total", "history.translated", "history.models",
                              "history.engine" };
    char values[4][64];
    osa_color tints[4] = { u->t.accent, u->t.accent2, u->t.success, u->t.warning };
    snprintf(values[0], sizeof values[0], "%d", st.n_transcripts);
    snprintf(values[1], sizeof values[1], "%d", st.n_translations);
    snprintf(values[2], sizeof values[2], "%d", st.n_models);
    snprintf(values[3], sizeof values[3], "%s", st.asr_backend[0] ? st.asr_backend : "—");
    for (int i = 0; i < 4; i++) {
        int slot = u->L.rtl ? (3 - i) : i;
        osa_rect tr = rectf(inner.x + (tw + gap) * i, inner.y, tw, tile_h);
        /* Persian digits for the counts; the engine name is prose and passes
         * through untouched. */
        num_fa(s, values[slot], vloc, sizeof vloc);
        stat_tile(u, tr, kl(s, labels[slot]), vloc, tints[slot]);
    }

    float top = inner.y + tile_h + 14 * u->L.scale;
    osa_rect list = rectf(inner.x, top, inner.w, inner.y + inner.h - top);
    ui_card(u, list);

    /* One pass over the array, then draw from the parsed rows. Parsing and
     * drawing are separate so the scroll offset and the visible window are
     * decided once, not per row. */
    hist_row *H = g_hist_rows;
    shell_parse_history(s->history_json);
    if (g_hist_n <= 0) {
        ui_label(u, list, kl(s, "history.empty"), UI_FS_BODY * u->L.scale,
                 u->t.text_faint, 1);
        return;
    }
    /* Header row: a table, because a history is a table. */
    float p = 14 * u->L.scale;
    float head_h = 30 * u->L.scale;
    float row_h = 50 * u->L.scale;
    float col_w = list.w - p * 2;
    float c_time = 104 * u->L.scale, c_lang = 84 * u->L.scale, c_conf = 74 * u->L.scale;
    float c_text = col_w - c_time - c_lang - c_conf - 40 * u->L.scale;
    if (c_text < 100 * u->L.scale) c_text = 100 * u->L.scale;

    /* Columns are laid out from the leading edge, so the same table reads
     * correctly in both directions. `colx(i)` returns the x of column i. */
    float widths[4] = { c_time, c_text, c_lang, c_conf };
    float colx[4];
    {
        float used = 10 * u->L.scale;
        for (int i = 0; i < 4; i++) {
            if (u->L.rtl) {
                colx[i] = list.x + list.w - p - used - widths[i];
            } else {
                colx[i] = list.x + p + used;
            }
            used += widths[i];
        }
    }
    osa_rect head = rectf(list.x + p, list.y + 12 * u->L.scale, col_w, head_h);
    static const char *head_keys[4] = { "history.time", "history.text", "history.language",
                                        "transcribe.confidence" };
    for (int i = 0; i < 4; i++)
        ui_label(u, rectf(colx[i], head.y, widths[i], head_h), kl(s, head_keys[i]),
                 UI_FS_TINY * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
    c_hline(u->c, list.x + p, list.x + list.w - p, head.y + head_h, 1, u->t.border_soft);

    osa_rect rows = rectf(list.x + p, head.y + head_h + 2, col_w,
                          list.h - (head.y - list.y) - head_h - 16 * u->L.scale);
    int visible = (int)(rows.h / row_h);
    if (visible < 1) visible = 1;
    int max_scroll = g_hist_n - visible;
    if (max_scroll < 0) max_scroll = 0;
    if (ui_hit(u, rows) && u->in.wheel != 0)
        s->history_scroll -= (int)(u->in.wheel * 2);
    if (s->history_scroll < 0) s->history_scroll = 0;
    if (s->history_scroll > max_scroll) s->history_scroll = max_scroll;

    canvas_clip_push(u->c, rows);
    for (int i = s->history_scroll; i < g_hist_n && i < s->history_scroll + visible; i++) {
        int slot = i - s->history_scroll;
        osa_rect rr = rectf(rows.x, rows.y + slot * row_h, rows.w, row_h - 2);
        hist_row *row = &H[i];
        if (ui_row(u, 4100 + i, rr, osa_streq(row->id, s->transcript_id))) {
            osa_strlcpy(s->transcript_id, row->id, sizeof s->transcript_id);
            osa_strlcpy(s->transcript, row->text, sizeof s->transcript);
            osa_strlcpy(s->detected_lang, row->lang, sizeof s->detected_lang);
            s->transcript_conf = (float)row->conf;
            s->transcript_dur = row->dur;
            s->translation[0] = 0;
            const char *pv = osa_app_translate_preview(s->app, row->text);
            if (pv) osa_strlcpy(s->translation, pv, sizeof s->translation);
            shell_refresh_phonetic(s);
            s->screen = SCREEN_TRANSCRIBE;
            break;
        }
        ui_label(u, rectf(colx[0], rr.y, c_time, row_h - 2), row->when,
                 UI_FS_SMALL * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
        ui_label(u, rectf(colx[1], rr.y, c_text, row_h - 2), row->text,
                 UI_FS_BODY * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
        ui_label(u, rectf(colx[2], rr.y, c_lang, row_h - 2),
                 shell_lang_name(s, row->lang), UI_FS_SMALL * u->L.scale, u->t.text_dim,
                 u->L.rtl ? 2 : 0);
        char pc[32], pcl[40];
        snprintf(pc, sizeof pc, "%.0f%%", row->conf * 100.0);
        num_fa(s, pc, pcl, sizeof pcl);
        ui_label(u, rectf(colx[3], rr.y, c_conf, row_h - 2), pcl,
                 UI_FS_SMALL * u->L.scale,
                 row->conf > 0.7 ? u->t.success : u->t.warning, u->L.rtl ? 2 : 0);
    }
    canvas_clip_pop(u->c);
    if (max_scroll > 0)
        ui_scrollbar(u, rectf(list.x + list.w - 10 * u->L.scale, rows.y, 5 * u->L.scale, rows.h),
                     s->history_scroll, g_hist_n, visible);
}

/* ---- Models ------------------------------------------------------- */

#define MAX_MODELS 64

typedef struct {
    char     id[64], name[128], kind[16], engine[32], license[72];
    bool     active;
    long long bytes;
} model_row;

typedef struct { model_row *r; int n; } model_ud;

static void model_visit(void *ud, const char *o, const char *e) {
    model_ud *mu = (model_ud *)ud;
    if (mu->n >= MAX_MODELS) return;
    model_row *m = &mu->r[mu->n++];
    memset(m, 0, sizeof *m);
    shell_json_str(o, e, "\"id\"", m->id, sizeof m->id);
    shell_json_str(o, e, "\"name\"", m->name, sizeof m->name);
    shell_json_str(o, e, "\"kind\"", m->kind, sizeof m->kind);
    shell_json_str(o, e, "\"engine\"", m->engine, sizeof m->engine);
    shell_json_str(o, e, "\"license\"", m->license, sizeof m->license);
    char b[16];
    memset(b, 0, sizeof b);
    if (shell_json_str(o, e, "\"bytes\"", b, sizeof b)) m->bytes = atoll(b);
    m->active = shell_json_str(o, e, "\"active\"", b, sizeof b) && osa_starts(b, "true");
}

static model_row g_models[MAX_MODELS];

static const char *kind_label(shell_state *s, const char *kind) {
    if (osa_starts(kind, "asr")) return kl(s, "models.kindasr");
    if (osa_starts(kind, "lang")) return kl(s, "models.kindlang");
    return kl(s, "models.kindmt");
}

/* One row of the download catalogue.
 *
 * The row has three bands, laid out per reading direction: a kind badge on the
 * leading edge, the text column, and the action on the trailing edge. The whole
 * row is a hit target: a 30-pixel button inside a 68-pixel row is a miss waiting
 * to happen, so the button is the affordance, not the only clickable part.
 *
 * The action depends on what the pack actually is. A bundle that ships with the
 * application says so and is a pill. A pack with an empty URL is a manual
 * install, and the row says which button installs it rather than showing a dead
 * "download" label. Only a real URL gets a download button. */
static void draw_pack_row(shell_state *s, ui_context *u, osa_rect r, const osa_pack_info *p,
                          int idx, int *clicked_idx) {
    bool hov = ui_hit(u, r);
    if (hov) c_fill_round(u->c, r, u->L.radius_sm, u->t.panel_alt);
    float pad = 14 * u->L.scale;

    osa_color tint = osa_starts(p->kind, "asr") ? u->t.accent
                   : osa_starts(p->kind, "lang") ? u->t.warning : u->t.accent2;
    float badge_w = 58 * u->L.scale;
    osa_rect badge = rectf(u->L.rtl ? r.x + r.w - pad - badge_w : r.x + pad,
                           r.y + 9 * u->L.scale, badge_w, 22 * u->L.scale);
    c_fill_round(u->c, badge, 11 * u->L.scale, col_alpha(tint, 0.16f));
    ui_label(u, badge, kind_label(s, p->kind), UI_FS_TINY * u->L.scale, tint, 1);

    float side = badge_w + 12 * u->L.scale;
    float action_w = 104 * u->L.scale;
    float tx = u->L.rtl ? r.x + pad : r.x + pad + side;
    float tw = r.w - pad * 2 - side - action_w - 12 * u->L.scale;
    ui_label(u, rectf(tx, r.y + 6 * u->L.scale, tw, 20 * u->L.scale), p->name,
             UI_FS_BODY * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);

    char sz[32], meta[256];
    ui_fmt_bytes(p->size_mb * 1024.0 * 1024.0, sz, sizeof sz);
    snprintf(meta, sizeof meta, "%s  ·  %s  ·  %s", p->language[0] ? p->language : "—",
             sz, p->license[0] ? p->license : "—");
    ui_label(u, rectf(tx, r.y + 25 * u->L.scale, tw, 17 * u->L.scale), meta,
             UI_FS_TINY * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);

    /* The third line is what the row is for, so it explains the action instead
     * of repeating the pack's marketing copy. */
    const char *line3 = p->installed ? kl(s, "models.installed_inactive")
                       : kl(s, "models.hintdownload");
    ui_label(u, rectf(tx, r.y + 42 * u->L.scale, tw, 16 * u->L.scale), line3,
             UI_FS_TINY * u->L.scale, col_alpha(u->t.text_faint, 0.85f),
             u->L.rtl ? 2 : 0);

    osa_rect ar = rectf(u->L.rtl ? r.x + pad : r.x + r.w - pad - action_w,
                        r.y + (r.h - 30 * u->L.scale) * 0.5f, action_w, 30 * u->L.scale);
    if (p->installed) {
        ui_pill(u, ar, kl(s, "models.installed"), u->t.success, u->t.success_soft);
    } else if (p->installable) {
        /* All catalog entries are real downloads; none are generated fixtures. */
        if (s->downloading && osa_streq(s->dl_pack, p->id)) {
            c_fill_round(u->c, ar, u->L.radius_sm, u->t.panel_alt);
            ui_progress(u, rect_inset(ar, 9 * u->L.scale), (float)s->dl_progress, u->t.accent);
        } else if (ui_button(u, 5200 + idx, ar, kl(s, "models.get"), !s->job)) {
            *clicked_idx = idx;
        } else if (hov && u->in.clicked && !u->consumed_click) {
            u->consumed_click = true;
            *clicked_idx = idx;
        }
    } else if (!p->url[0]) {
        /* Honest dead end: the file is not in the binary, so the only real
         * action is the picker. */
        if (ui_button_ghost(u, 5200 + idx, ar, kl(s, "models.locallic")))
            *clicked_idx = idx;
    } else if (s->downloading && osa_streq(s->dl_pack, p->id)) {
        c_fill_round(u->c, ar, u->L.radius_sm, u->t.panel_alt);
        ui_progress(u, rect_inset(ar, 9 * u->L.scale), (float)s->dl_progress, u->t.accent);
    } else {
        bool b = ui_button(u, 5200 + idx, ar, kl(s, "models.get"), !s->job);
        if (b || (hov && u->in.clicked && !u->consumed_click)) *clicked_idx = idx;
    }
}

static void draw_models(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 22 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);

    static osa_pack_info packs[24];
    int np = osa_app_packs(s->app, packs, 24);
    int n_inst = 0;
    for (int i = 0; i < np; i++) if (packs[i].installed) n_inst++;

    osa_app_status st=s->status_cache;

    /* One explicit active model per required category. */
    float sum_h=92*u->L.scale;osa_rect sum=rectf(inner.x,inner.y,inner.w,sum_h);ui_card(u,sum);
    {
        float p=16*u->L.scale,col=(sum.w-p*2)/3.0f;
        const char*labels[]={"models.asrlabel","models.mtlabel","models.langlabel"};
        const char*names[]={st.asr_model,st.mt_model,st.lid_model};
        const char*engines[]={st.asr_backend,st.mt_backend,st.lid_backend};
        bool ready[]={st.asr_ready,st.mt_ready,st.lid_ready};
        for(int i=0;i<3;i++){float x=u->L.rtl?sum.x+sum.w-p-col*(i+1):sum.x+p+col*i;
            ui_label(u,rectf(x,sum.y+10*u->L.scale,col-12*u->L.scale,16*u->L.scale),kl(s,labels[i]),UI_FS_TINY*u->L.scale,u->t.text_faint,u->L.rtl?2:0);
            ui_label(u,rectf(x,sum.y+30*u->L.scale,col-12*u->L.scale,24*u->L.scale),names[i][0]?names[i]:kl(s,"models.none"),UI_FS_BODY*u->L.scale,ready[i]?u->t.text:u->t.warning,u->L.rtl?2:0);
            ui_label(u,rectf(x,sum.y+58*u->L.scale,col-12*u->L.scale,18*u->L.scale),ready[i]?engines[i]:kl(s,"models.required"),UI_FS_TINY*u->L.scale,u->t.text_dim,u->L.rtl?2:0);
        }
    }

    /* ---- toolbar: tabs on the leading edge, import on the trailing one.
     * Tabs used to start at the trailing edge and so collided with the import
     * button on any window narrower than the design width. ---- */
    float tab_h = 34 * u->L.scale;
    float tab_w = 146 * u->L.scale;
    float tabs_y = sum.y + sum_h + 14 * u->L.scale;
    float bx0 = u->L.rtl ? inner.x + inner.w - tab_w : inner.x;
    float bx1 = u->L.rtl ? inner.x + inner.w - tab_w * 2 - 6 * u->L.scale
                         : inner.x + tab_w + 6 * u->L.scale;
    osa_rect t0 = rectf(bx0, tabs_y, tab_w, tab_h);
    osa_rect t1 = rectf(bx1, tabs_y, tab_w, tab_h);
    if (ui_tab(u, 5001, t0, kl(s, "models.tabinstalled"), s->models_tab == 0)) {
        s->models_tab = 0;
        free(s->models_json);
        s->models_json = osa_app_models_json(s->app, NULL);
    }
    if (ui_tab(u, 5002, t1, kl(s, "models.tabdownload"), s->models_tab == 1))
        s->models_tab = 1;
    c_hline(u->c, inner.x, inner.x + inner.w, tabs_y + tab_h, 1, u->t.border_soft);

    float iw = 176 * u->L.scale;
    osa_rect imp = rectf(u->L.rtl ? inner.x : inner.x + inner.w - iw, tabs_y, iw, tab_h);
    if (ui_button(u, 5003, imp, kl(s, "models.import"), false))
        s->request = SHELL_REQ_PICK_PACK;

    float top = tabs_y + tab_h + 12 * u->L.scale;
    osa_rect list = rectf(inner.x, top, inner.w, inner.y + inner.h - top);
    ui_card(u, list);

    /* ---- installed tab ---------------------------------------------- */
    if (s->models_tab == 0) {
        if (!s->models_json) s->models_json = osa_app_models_json(s->app, NULL);
        model_ud mu = { g_models, 0 };
        shell_json_walk(s->models_json, model_visit, &mu);
        float p = 16 * u->L.scale;
        float row_h = 62 * u->L.scale;
        float act_w = 104 * u->L.scale;
        float foot = 42 * u->L.scale;
        osa_rect rows = rectf(list.x + p, list.y + p, list.w - p * 2,
                              list.h - p * 2 - foot);
        canvas_clip_push(u->c, rows);
        for (int i = 0; i < mu.n; i++) {
            osa_rect rr = rectf(rows.x, rows.y + i * row_h, rows.w, row_h - 4 * u->L.scale);
            if (rr.y + rr.h > rows.y + rows.h) break;
            bool hov = ui_hit(u, rr);
            if (hov) c_fill_round(u->c, rr, u->L.radius_sm, u->t.panel_alt);

            osa_color tint = osa_starts(g_models[i].kind, "asr") ? u->t.accent : u->t.accent2;
            osa_rect badge = rectf(u->L.rtl ? rr.x + rr.w - 58 * u->L.scale : rr.x,
                                   rr.y + 9 * u->L.scale, 58 * u->L.scale, 22 * u->L.scale);
            c_fill_round(u->c, badge, 11 * u->L.scale, col_alpha(tint, 0.16f));
            ui_label(u, badge, kind_label(s, g_models[i].kind), UI_FS_TINY * u->L.scale,
                     tint, 1);

            float cb = 58 * u->L.scale + 12 * u->L.scale;
            float cx = u->L.rtl ? rr.x + act_w + 12 * u->L.scale : rr.x + cb;
            float cw = rr.w - cb - act_w - 24 * u->L.scale;
            ui_label(u, rectf(cx, rr.y + 6 * u->L.scale, cw, 22 * u->L.scale),
                     g_models[i].name, UI_FS_BODY * u->L.scale, u->t.text,
                     u->L.rtl ? 2 : 0);
            /* id · engine · size: the three things that tell two builds of the
             * same model apart, which is what "is this the file I installed?" is */
            char meta[256], sz[32] = "";
            ui_fmt_bytes((double)g_models[i].bytes, sz, sizeof sz);
            snprintf(meta, sizeof meta, "%s  ·  %s%s%s", g_models[i].id,
                     g_models[i].engine[0] ? g_models[i].engine : "—",
                     sz[0] ? "  ·  " : "", sz);
            ui_label(u, rectf(cx, rr.y + 26 * u->L.scale, cw, 17 * u->L.scale), meta,
                     UI_FS_TINY * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);

            osa_rect ar = rectf(u->L.rtl ? rr.x : rr.x + rr.w - act_w,
                                rr.y + (rr.h - 30 * u->L.scale) * 0.5f, act_w,
                                30 * u->L.scale);
            if (g_models[i].active) {
                ui_pill(u, ar, kl(s, "models.active"), u->t.accent, u->t.accent_soft);
            } else if (ui_button(u, 5100 + i, ar, kl(s, "btn.activate"), !s->job)) {
                shell_activate_model(s,g_models[i].id);
            }
            c_hline(u->c, rr.x, rr.x + rr.w, rr.y + rr.h, 1, u->t.border_soft);
        }
        if (mu.n == 0) {
            ui_label(u, rectf(rows.x, rows.y + rows.h * 0.32f, rows.w, 24 * u->L.scale),
                     kl(s, "models.none"), UI_FS_BODY * u->L.scale, u->t.text_faint, 1);
        }
        canvas_clip_pop(u->c);

        /* footer: where the files live and a shortcut to the folder */
        float fy = list.y + list.h - foot + 4 * u->L.scale;
        c_hline(u->c, list.x + p, list.x + list.w - p, fy - 8 * u->L.scale, 1,
                u->t.border_soft);
        float ob = 132 * u->L.scale;
        osa_rect ofr = rectf(u->L.rtl ? list.x + list.w - p - ob : list.x + p,
                             fy + 2 * u->L.scale, ob, 28 * u->L.scale);
        float px = u->L.rtl ? list.x + p : ofr.x + ob + 12 * u->L.scale;
        ui_label(u, rectf(px, fy + 6 * u->L.scale,
                          list.w - p * 2 - ob - 16 * u->L.scale, 24 * u->L.scale),
                 osa_path(OSAI_DIR_MODELS), UI_FS_MONO * u->L.scale, u->t.text_faint,
                 u->L.rtl ? 2 : 0);
        if (ui_button_ghost(u, 5004, ofr, kl(s, "btn.openfolder")))
            s->request = SHELL_REQ_OPEN_FOLDER;
        return;
    }

    /* ---- downloadable tab ------------------------------------------- */
    float p = 8 * u->L.scale;
    float row_h = 68 * u->L.scale;
    osa_rect rows = rectf(list.x + p, list.y + p, list.w - p * 2,
                          list.h - p * 2 - 28 * u->L.scale);
    int visible = (int)(rows.h / row_h);
    if (visible < 1) visible = 1;
    int maxs = np - visible;
    if (maxs < 0) maxs = 0;
    if (ui_hit(u, rows) && u->in.wheel != 0) s->packs_scroll -= (int)(u->in.wheel * 2);
    if (s->packs_scroll < 0) s->packs_scroll = 0;
    if (s->packs_scroll > maxs) s->packs_scroll = maxs;

    int clicked = -1;
    canvas_clip_push(u->c, rows);
    for (int i = s->packs_scroll; i < np && i < s->packs_scroll + visible + 1; i++) {
        osa_rect rr = rectf(rows.x, rows.y + (i - s->packs_scroll) * row_h, rows.w,
                            row_h - 4 * u->L.scale);
        if (rr.y + rr.h > rows.y + rows.h) break;
        draw_pack_row(s, u, rr, &packs[i], i, &clicked);
        c_hline(u->c, rr.x + 8 * u->L.scale, rr.x + rr.w - 8 * u->L.scale, rr.y + rr.h, 1,
                u->t.border_soft);
    }
    canvas_clip_pop(u->c);
    if (maxs > 0)
        ui_scrollbar(u, rectf(list.x + list.w - 10 * u->L.scale, rows.y, 5 * u->L.scale,
                              rows.h), s->packs_scroll, np, visible);

    if (clicked >= 0 && clicked < np && !packs[clicked].installed && !s->job) {
        if (!osa_http_has_tls()) shell_set_message(s,true,"%s",kl(s,"models.notls"));
        else if (!shell_download_start(s,packs[clicked].id)) shell_set_message(s,true,"%s",kl(s,"msg.failed"));
    }

    char head[192], loc2[224];
    snprintf(head, sizeof head, kl(s, "models.dlhead"), np, n_inst);
    num_fa(s, head, loc2, sizeof loc2);
    float half = list.w * 0.5f;
    ui_label(u, rectf(u->L.rtl ? list.x + list.w - 14 * u->L.scale - half
                               : list.x + 14 * u->L.scale,
                      list.y + list.h - 22 * u->L.scale, half, 18 * u->L.scale), loc2,
             UI_FS_TINY * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
    ui_label(u, rectf(u->L.rtl ? list.x + 14 * u->L.scale
                               : list.x + half, list.y + list.h - 22 * u->L.scale,
                      half - 14 * u->L.scale, 18 * u->L.scale),
             kl(s, "models.dlnote"), UI_FS_TINY * u->L.scale, u->t.text_faint,
             u->L.rtl ? 0 : 2);
}

/* ---- Training ----------------------------------------------------- */

/* A labelled control row: caption on the leading side, control on the trailing
 * side. Used by Training and Settings so both screens share one rhythm. */
static void setting_row(ui_context *u, osa_rect r, const char *label, const char *hint) {
    float lw = r.w * 0.52f;
    ui_label(u, rectf(u->L.rtl ? r.x + r.w - lw : r.x, r.y, lw, 20 * u->L.scale), label,
             UI_FS_BODY * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    if (hint && *hint)
        ui_label(u, rectf(u->L.rtl ? r.x + r.w - lw : r.x, r.y + 18 * u->L.scale, lw,
                          16 * u->L.scale), hint, UI_FS_TINY * u->L.scale,
                 u->t.text_faint, u->L.rtl ? 2 : 0);
}


static void draw_training(shell_state *s, ui_context *u, osa_rect body) {
    float pad=28*u->L.scale;osa_rect r=rect_inset(body,pad);ui_card(u,r);
    float p=24*u->L.scale;
    ui_label(u,rectf(r.x+p,r.y+p,r.w-p*2,28*u->L.scale),kl(s,"training.title"),UI_FS_H1*u->L.scale,u->t.text,u->L.rtl?2:0);
    text_opts o=text_default_opts(UI_FS_H2*u->L.scale);o.max_width=r.w-p*2;o.rtl=u->L.rtl;o.align=u->L.rtl?2:0;o.line_height=UI_FS_H2*u->L.scale*1.9f;
    text_draw_box(u->c,u->reg,UI_FS_H2*u->L.scale,u->t.text_dim,rectf(r.x+p,r.y+64*u->L.scale,r.w-p*2,r.h-120*u->L.scale),kl(s,"training.unsupported"),o);
    ui_label(u,rectf(r.x+p,r.y+r.h-44*u->L.scale,r.w-p*2,24*u->L.scale),kl(s,"training.note"),UI_FS_SMALL*u->L.scale,u->t.text_faint,u->L.rtl?2:0);
}

static void draw_evaluation(shell_state *s, ui_context *u, osa_rect body) {
    float pad=28*u->L.scale;osa_rect r=rect_inset(body,pad);ui_card(u,r);float p=24*u->L.scale;
    ui_label(u,rectf(r.x+p,r.y+p,r.w-p*2,28*u->L.scale),kl(s,"eval.title"),UI_FS_H1*u->L.scale,u->t.text,u->L.rtl?2:0);
    text_opts o=text_default_opts(UI_FS_H2*u->L.scale);o.max_width=r.w-p*2;o.rtl=u->L.rtl;o.align=u->L.rtl?2:0;o.line_height=UI_FS_H2*u->L.scale*1.9f;
    text_draw_box(u->c,u->reg,UI_FS_H2*u->L.scale,u->t.text_dim,rectf(r.x+p,r.y+64*u->L.scale,r.w-p*2,100*u->L.scale),kl(s,"eval.nodata"),o);
    osa_rect br=rectf(u->L.rtl?r.x+p:r.x+r.w-p-160*u->L.scale,r.y+r.h-60*u->L.scale,160*u->L.scale,36*u->L.scale);
    if(ui_button(u,7000,br,kl(s,"btn.run"),s->models_ready&&!s->working))shell_start_job(s,SHELL_JOB_EVALUATE,NULL);
    if(s->eval_json)ui_label(u,rectf(r.x+p,r.y+180*u->L.scale,r.w-p*2,40*u->L.scale),kl(s,"eval.nodata"),UI_FS_BODY*u->L.scale,u->t.warning,u->L.rtl?2:0);
}

static void settings_group(ui_context *u, osa_rect r, const char *title) {
    ui_card(u, r);
    float p = 16 * u->L.scale;
    ui_label(u, rectf(r.x + p, r.y + 12 * u->L.scale, r.w - p * 2, 20 * u->L.scale), title,
             UI_FS_SMALL * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
    c_hline(u->c, r.x + p, r.x + r.w - p, r.y + 36 * u->L.scale, 1, u->t.border_soft);
}

static void draw_settings(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 22 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);
    float gap = 14 * u->L.scale;
    float col_w = (inner.w - gap) * 0.5f;
    float row_h = 50 * u->L.scale;
    float ctrl_w = 140 * u->L.scale;
    float sw = 42 * u->L.scale, sh = 23 * u->L.scale;
    /* Cards are sized to their content, the way a settings page is: stretching
     * a three-row card to the bottom of a 900-pixel window only creates empty
     * space that reads as a missing control. */
    float bottom_h = 178 * u->L.scale;
    float half_h = 352 * u->L.scale;
    float total_h = half_h + gap + bottom_h;
    if (total_h > inner.h) {
        float f = inner.h / total_h;
        half_h *= f;
        bottom_h *= f;
    }

    /* Leading column: appearance. Trailing: translation. Mirrored in RTL. */
    osa_rect a = rectf(u->L.rtl ? inner.x + col_w + gap : inner.x, inner.y, col_w, half_h);
    osa_rect b = rectf(u->L.rtl ? inner.x : inner.x + col_w + gap, inner.y, col_w, half_h);
    osa_rect c = rectf(inner.x, inner.y + half_h + gap, inner.w, bottom_h);

    /* ================= appearance ================= */
    settings_group(u, a, kl(s, "settings.appearance"));
    float p = 18 * u->L.scale;
    float y = a.y + 50 * u->L.scale;

    setting_row(u, rectf(a.x + p, y, a.w - p * 2, row_h), kl(s, "settings.dark"),
                kl(s, "settings.darkhint"));
    {
        osa_rect t = rectf(u->L.rtl ? a.x + p : a.x + a.w - p - sw, y + 6 * u->L.scale, sw, sh);
        if (ui_toggle_switch(u, 8000, t, &s->dark)) {
            ui_set_theme(u, s->dark);
            shell_save_prefs(s);
        }
    }
    y += row_h;

    setting_row(u, rectf(a.x + p, y, a.w - p * 2, row_h), kl(s, "settings.rtl"),
                kl(s, "settings.rtlhint"));
    {
        int v = s->lang == UI_FA ? 0 : 1;
        osa_rect seg = rectf(u->L.rtl ? a.x + p : a.x + a.w - p - ctrl_w,
                             y + 8 * u->L.scale, ctrl_w, 30 * u->L.scale);
        if (ui_segmented(u, 8001, seg, "فارسی|English", &v, 2)) {
            s->lang = v == 0 ? UI_FA : UI_EN;
            shell_save_prefs(s);
        }
    }
    y += row_h;

    setting_row(u, rectf(a.x + p, y, a.w - p * 2, row_h), kl(s, "settings.scale"),
                kl(s, "settings.scalehint"));
    {
        float frac = (s->scale - 0.85f) / 0.65f;
        char pct[32];
        snprintf(pct, sizeof pct, "%d%%", (int)(s->scale * 100.0f + 0.5f));
        osa_rect sr = rectf(u->L.rtl ? a.x + p : a.x + a.w - p - ctrl_w - 44 * u->L.scale,
                            y + 8 * u->L.scale, ctrl_w - 40 * u->L.scale, 28 * u->L.scale);
        (void)frac;
        if (ui_slider(u, 8002, sr, &s->scale, 0.85f, 1.5f)) shell_save_prefs(s);
        ui_label(u, rectf(u->L.rtl ? sr.x + sr.w + 4 * u->L.scale
                                   : a.x + a.w - p - 40 * u->L.scale,
                          y + 8 * u->L.scale, 40 * u->L.scale, 28 * u->L.scale),
                 pct, UI_FS_SMALL * u->L.scale, u->t.text_dim, u->L.rtl ? 0 : 2);
    }
    y += row_h;

    setting_row(u, rectf(a.x + p, y, a.w - p * 2, row_h), kl(s, "settings.threads"),
                kl(s, "settings.threadshint"));
    {
        osa_rect sr = rectf(u->L.rtl ? a.x + p : a.x + a.w - p - ctrl_w,
                            y + 6 * u->L.scale, ctrl_w, 32 * u->L.scale);
        if (ui_stepper(u, 8003, sr, &s->asr_threads, 1, 16)) shell_save_prefs(s);
    }

    /* ================= translation ================= */
    settings_group(u, b, kl(s, "settings.translation"));
    y = b.y + 50 * u->L.scale;
    float half = (b.w - p * 2 - 10 * u->L.scale) * 0.5f;

    char src_lbl[64], dst_lbl[64];
    snprintf(src_lbl, sizeof src_lbl, "%s",
             s->lang_src[0] ? shell_lang_name(s, s->lang_src) : kl(s, "lang.auto"));
    snprintf(dst_lbl, sizeof dst_lbl, "%s",
             s->lang_dst[0] ? shell_lang_name(s, s->lang_dst) : kl(s, "lang.auto"));
    osa_rect src_r = rectf(u->L.rtl ? b.x + p + half + 10 * u->L.scale : b.x + p, y,
                           half, 48 * u->L.scale);
    osa_rect dst_r = rectf(u->L.rtl ? b.x + p : b.x + p + half + 10 * u->L.scale, y,
                           half, 48 * u->L.scale);
    lang_picker(s, u, src_r, 8100, kl(s, "lang.from"), src_lbl, u->popup_id == 8100);
    lang_picker(s, u, dst_r, 8101, kl(s, "lang.to"), dst_lbl, u->popup_id == 8101);
    y += 58 * u->L.scale;

    ui_label(u, rectf(b.x + p, y, b.w - p * 2, 16 * u->L.scale), kl(s, "lang.register"),
             UI_FS_TINY * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
    y += 18 * u->L.scale;
    {
        osa_rect seg = rectf(b.x + p, y, b.w - p * 2, 34 * u->L.scale);
        int v = (int)s->style;
        if (ui_segmented(u, 8102, seg, kl(s, "lang.styles"), &v, MT_STYLE_COUNT)) {
            s->style = (mt_style)v;
            osa_app_set_style(s->app, s->style);
            shell_save_prefs(s);
        }
    }
    y += 38 * u->L.scale;
    ui_label(u, rectf(b.x + p, y, b.w - p * 2, 16 * u->L.scale), kl(s, "lang.registerhint"),
             UI_FS_TINY * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
    y += 22 * u->L.scale;

    /* RTL: the switch leads the cluster on the right, its label follows on the
     * left. LTR is the mirror image. */
    setting_row(u, rectf(u->L.rtl ? b.x + p : b.x + p + sw + 12 * u->L.scale, y,
                         b.w - p * 2 - sw - 12 * u->L.scale, 42 * u->L.scale),
                kl(s, "settings.autodetect"), kl(s, "settings.autodetecthint"));
    {
        osa_rect t = rectf(u->L.rtl ? b.x + b.w - p - sw : b.x + p,
                           y + 8 * u->L.scale, sw, sh);
        if (ui_toggle_switch(u, 8103, t, &s->auto_detect)) {
            osa_strlcpy(s->lang_src,s->auto_detect?"auto":"fa",sizeof s->lang_src);
            if(s->models_ready)osa_app_set_languages(s->app,s->lang_src,s->lang_dst,NULL);
            shell_save_prefs(s);
        }
    }
    y += 42 * u->L.scale;
    setting_row(u, rectf(u->L.rtl ? b.x + p : b.x + p + sw + 12 * u->L.scale, y,
                         b.w - p * 2 - sw - 12 * u->L.scale, 42 * u->L.scale),
                kl(s, "lang.phonetic"), kl(s, "settings.phonetichint"));
    {
        osa_rect t = rectf(u->L.rtl ? b.x + b.w - p - sw : b.x + p,
                           y + 8 * u->L.scale, sw, sh);
        if (ui_toggle_switch(u, 8104, t, &s->show_phonetic)) {
            shell_save_prefs(s); shell_refresh_phonetic(s);
        }
    }

    /* ================= privacy + data ================= */
    settings_group(u, c, kl(s, "settings.privacy"));
    y = c.y + 50 * u->L.scale;
    struct { const char *label, *hint; int id; bool *v; } sws[2] = {
        { "settings.saveaudio", "settings.saveaudiohint", 8200, &s->save_audio },
        { "settings.privacy", "settings.privacyhint", 8201, &s->privacy_logs },
    };
    /* Two settings side by side. Each half is a cluster read in the direction of
     * the paragraph: the switch comes first, its label and hint right after it.
     * Anchoring the switch to the outer edge and the text to the opposite side
     * left half a window's width between a control and the words describing it. */
    {
        float colw = (c.w - p * 2) / 2.0f;
        float unit_gap = 12 * u->L.scale;
        for (int i = 0; i < 2; i++) {
            float sx = u->L.rtl ? c.x + c.w - p - colw * (i + 1) : c.x + p + colw * i;
            float knob_x = u->L.rtl ? sx + colw - sw : sx;
            osa_rect t = rectf(knob_x, y + 10 * u->L.scale, sw, sh);
            if (ui_toggle_switch(u, sws[i].id, t, sws[i].v)) shell_save_prefs(s);
            float tx = u->L.rtl ? sx : sx + sw + unit_gap;
            float tw = colw - sw - unit_gap;
            setting_row(u, rectf(tx, y, tw, 44 * u->L.scale),
                        kl(s, sws[i].label), kl(s, sws[i].hint));
        }
    }

    /* data folder: path + open, on the same row as an explanation */
    float fy = c.y + c.h - 62 * u->L.scale;
    c_hline(u->c, c.x + p, c.x + c.w - p, fy - 10 * u->L.scale, 1, u->t.border_soft);
    ui_label(u, rectf(c.x + p, fy, c.w - p * 2 - 150 * u->L.scale, 18 * u->L.scale),
             kl(s, "settings.datalocation"), UI_FS_TINY * u->L.scale, u->t.text_faint,
             u->L.rtl ? 2 : 0);
    osa_rect path_r = rectf(u->L.rtl ? c.x + p + 132 * u->L.scale : c.x + p,
                            fy + 20 * u->L.scale,
                            c.w - p * 2 - 132 * u->L.scale, 30 * u->L.scale);
    c_fill_round(u->c, path_r, u->L.radius_sm, u->t.panel_alt);
    c_stroke_round(u->c, path_r, u->L.radius_sm, 1.0f, u->t.border_soft);
    ui_label(u, rectf(path_r.x + 10 * u->L.scale, path_r.y,
                      path_r.w - 20 * u->L.scale, path_r.h), osa_path(OSAI_DIR_ROOT),
             UI_FS_MONO * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
    osa_rect open_r = rectf(u->L.rtl ? c.x + p : c.x + c.w - p - 124 * u->L.scale,
                            fy + 20 * u->L.scale, 124 * u->L.scale, 30 * u->L.scale);
    if (ui_button(u, 8300, open_r, kl(s, "btn.openfolder"), false))
        s->request = SHELL_REQ_OPEN_FOLDER;
}

/* ---- About -------------------------------------------------------- */

/* Machine readings, refreshed at most twice a second. The screen used to ask for
 * a fresh status on every frame: three SQLite COUNTs, /proc/meminfo, /proc/stat
 * and a statvfs, sixty times a second, to draw five lines that cannot change
 * that fast. Cheap to fix and it keeps the repaint rate honest on a laptop. */
static void about_status(shell_state *s, osa_app_status *out) {
    if (!s->status_valid || osa_now_ms() - s->status_ms > 500) {
        osa_app_get_status(s->app, &s->status_cache);
        s->status_ms = osa_now_ms();
        s->status_valid = true;
    }
    *out = s->status_cache;
}


static void about_bullet(ui_context *u, osa_rect r, const char *text) {
    float dx = u->L.rtl ? r.x + r.w - 8 * u->L.scale : r.x + 8 * u->L.scale;
    c_circle(u->c, dx, r.y + 9 * u->L.scale, 3 * u->L.scale, u->t.accent);
    float tx = u->L.rtl ? r.x : r.x + 20 * u->L.scale;
    ui_label(u, rectf(tx, r.y, r.w - 20 * u->L.scale, 20 * u->L.scale), text,
             UI_FS_BODY * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
}

static void draw_about(shell_state *s, ui_context *u, osa_rect body) {
    float pad = 22 * u->L.scale;
    osa_rect inner = rect_inset(body, pad);
    float hero_h = 132 * u->L.scale;
    osa_rect hero = rectf(inner.x, inner.y, inner.w, hero_h);
    ui_card(u, hero);

    float box = 56 * u->L.scale;
    osa_rect logo = rectf(u->L.rtl ? hero.x + hero.w - 28 * u->L.scale - box
                                   : hero.x + 28 * u->L.scale,
                          hero.y + (hero_h - box) * 0.5f, box, box);
    c_fill_round(u->c, logo, box * 0.28f, u->t.accent);
    for (int i = 0; i < 3; i++) {
        float h = (i == 1 ? 24.0f : (i == 0 ? 14.0f : 18.0f)) * u->L.scale;
        c_fill_round(u->c, rectf(logo.x + box * (0.28f + 0.2f * i),
                                 logo.y + (box - h) * 0.5f, 3.6f * u->L.scale, h),
                     1.8f * u->L.scale, rgba(255, 255, 255, 235));
    }
    float tx = u->L.rtl ? hero.x + 28 * u->L.scale : logo.x + box + 20 * u->L.scale;
    float tw = hero.w - box - 28 * u->L.scale * 2 - 20 * u->L.scale;
    ui_label(u, rectf(tx, hero.y + 30 * u->L.scale, tw, 30 * u->L.scale), "OpenSpeech AI",
             UI_FS_DISPLAY * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    char ver[96];
    snprintf(ver, sizeof ver, "%s %s  ·  %s", kl(s, "about.version"), OSAI_APP_VERSION,
             kl(s, "about.tagline"));
    ui_label(u, rectf(tx, hero.y + 64 * u->L.scale, tw, 20 * u->L.scale), ver,
             UI_FS_SMALL * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
    osa_rect pill = rectf(u->L.rtl ? tx + tw - 88 * u->L.scale : tx, hero.y + 90 * u->L.scale,
                          88 * u->L.scale, 24 * u->L.scale);
    ui_pill(u, pill, kl(s, "about.license"), u->t.success, u->t.success_soft);

    /* Two content-sized columns instead of two cards stretched to the window
     * floor: the second one also has room for the licence notes, which belong on
     * an About screen and were missing from it. */
    float top = hero.y + hero_h + 14 * u->L.scale;
    float col_w = (inner.w - 14 * u->L.scale) * 0.5f;
    float col_h = inner.y + inner.h - top;
    float want_h = 118 * u->L.scale + 5 * 34 * u->L.scale;
    if (col_h > want_h) col_h = want_h;
    osa_rect a = rectf(u->L.rtl ? inner.x + col_w + 14 * u->L.scale : inner.x, top, col_w, col_h);
    osa_rect b = rectf(u->L.rtl ? inner.x : inner.x + col_w + 14 * u->L.scale, top, col_w, col_h);
    ui_card(u, a);
    ui_card(u, b);
    float p = 18 * u->L.scale;

    ui_label(u, rectf(a.x + p, a.y + 14 * u->L.scale, a.w - p * 2, 22 * u->L.scale),
             kl(s, "about.principles"), UI_FS_H2 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    c_hline(u->c, a.x + p, a.x + a.w - p, a.y + 42 * u->L.scale, 1, u->t.border_soft);
    const char *bullets[5] = { "about.offline", "about.noupload", "about.models",
                               "about.privacylogs", "about.rollback" };
    float y = a.y + 54 * u->L.scale;
    for (int i = 0; i < 5; i++) {
        if (y + 22 * u->L.scale > a.y + a.h - 12 * u->L.scale) break;
        about_bullet(u, rectf(a.x + p, y, a.w - p * 2, 20 * u->L.scale), kl(s, bullets[i]));
        y += 30 * u->L.scale;
    }

    ui_label(u, rectf(b.x + p, b.y + 14 * u->L.scale, b.w - p * 2, 22 * u->L.scale),
             kl(s, "about.system"), UI_FS_H2 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    c_hline(u->c, b.x + p, b.x + b.w - p, b.y + 42 * u->L.scale, 1, u->t.border_soft);
    osa_app_status st;
    about_status(s, &st);
    struct { const char *k; char v[192]; } rows[5];
    osa_strlcpy(rows[0].v, st.asr_model[0] ? st.asr_model : "—", sizeof rows[0].v);
    rows[0].k = "about.asr";
    osa_strlcpy(rows[1].v, st.mt_model[0] ? st.mt_model : "—", sizeof rows[1].v);
    rows[1].k = "about.mt";
    osa_strlcpy(rows[2].v,st.lid_model[0]?st.lid_model:"—",sizeof rows[2].v);rows[2].k="models.langlabel";
    /* "Memory in use" is this application's own resident size, not the whole
     * machine's: /proc/meminfo (or GlobalMemoryStatusEx) moves with every other
     * program on the computer, so a busy desktop made an idle window look like
     * it was leaking, and nothing the app did could ever make the figure fall.
     *
     * The resident size is real, live memory, so only the two run modes that
     * need a byte-identical picture — the snapshot renderer and the visual
     * regression job — replace it with the frozen value. Everything else shows
     * the truth. */
    char ram[96], ramloc[160];
    snprintf(ram, sizeof ram, "%.0f MB",
             osa_sys_stats_frozen() ? st.app_ram_mb : (double)osa_process_ram_mb());
    num_fa(s, ram, ramloc, sizeof ramloc);
    snprintf(rows[3].v, sizeof rows[3].v, "%s", ramloc);
    rows[3].k = "about.ram";
    osa_strlcpy(rows[4].v, st.data_root, sizeof rows[4].v);
    rows[4].k = "about.dataroot";
    y = b.y + 54 * u->L.scale;
    for (int i = 0; i < 5; i++) {
        if (y + 30 * u->L.scale > b.y + b.h - 10 * u->L.scale) break;
        ui_label(u, rectf(b.x + p, y, b.w - p * 2, 16 * u->L.scale), kl(s, rows[i].k),
                 UI_FS_TINY * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
        ui_label(u, rectf(b.x + p, y + 14 * u->L.scale, b.w - p * 2, 18 * u->L.scale),
                 rows[i].v, UI_FS_SMALL * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
        y += 34 * u->L.scale;
    }

    /* ---- licence footer: shipped components, with their obligations ----
     * Each row is a Persian caption plus the licence identifier as its own
     * LTR value. Mixing Latin and Persian in one paragraph reorders the Latin
     * run on its punctuation ("GPL-3.0-or-later" came out reversed), so the two
     * languages never share a string here. */
    float fy = top + col_h + 14 * u->L.scale;
    float rows_h = 3 * 22 * u->L.scale;
    float fh = 42 * u->L.scale + rows_h + 26 * u->L.scale;
    if (inner.y + inner.h - fy < fh) fh = inner.y + inner.h - fy;
    if (fh >= 80 * u->L.scale) {
        osa_rect foot = rectf(inner.x, fy, inner.w, fh);
        ui_card(u, foot);
        ui_label(u, rectf(foot.x + p, foot.y + 11 * u->L.scale, foot.w - p * 2,
                          20 * u->L.scale), kl(s, "about.licenses"),
                 UI_FS_SMALL * u->L.scale, u->t.text_dim, u->L.rtl ? 2 : 0);
        c_hline(u->c, foot.x + p, foot.x + foot.w - p, foot.y + 36 * u->L.scale, 1,
                u->t.border_soft);

        static const char *cap_keys[3] = { "about.lic.capp", "about.lic.capf",
                                           "about.lic.capl" };
        static const char *val_keys[3] = { "about.lic.app", "about.lic.font",
                                           "about.lic.libs" };
        float ry = foot.y + 44 * u->L.scale;
        for (int i = 0; i < 3; i++) {
            ui_label(u, rectf(foot.x + p, ry, foot.w * 0.30f, 18 * u->L.scale),
                     kl(s, cap_keys[i]), UI_FS_TINY * u->L.scale, u->t.text_faint,
                     u->L.rtl ? 2 : 0);
            float cap_w = foot.w * 0.30f;
            float vgap = 14 * u->L.scale;
            float vx = foot.x + p + cap_w + vgap;
            ui_label(u, rectf(vx, ry, foot.x + foot.w - p - vx, 18 * u->L.scale),
                     kl(s, val_keys[i]), UI_FS_TINY * u->L.scale, u->t.text_dim, 0);
            ry += 22 * u->L.scale;
        }
        ui_label(u, rectf(foot.x + p, ry + 2 * u->L.scale, foot.w - p * 2,
                          18 * u->L.scale), kl(s, "about.lic.offline"),
                 UI_FS_TINY * u->L.scale, u->t.text_faint, u->L.rtl ? 2 : 0);
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

/* Draws the title bar for `sc` and returns the content rect below it, so every
 * screen gets the identical header. */
osa_rect shell_screen_chrome(shell_state *s, ui_context *u, osa_rect content,
                             shell_screen sc) {
    osa_rect rest = content;
    float top_h = 64 * u->L.scale;
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

    /* One preferences write per frame, whichever settings control changed.
     * Without this a stepper or a slider only persisted when the user happened
     * to press something else afterwards. */
    if (u->value_changed) {
        if (s->screen == SCREEN_SETTINGS) {
            ui_set_scale(u, s->scale);
            shell_save_prefs(s);
        } else if (s->screen == SCREEN_TRAINING) {
            shell_save_prefs(s);
        }
    }
}

/* Applies a drop-down selection. `popup_id` must be captured *before*
 * ui_popup_draw(), because the widget clears the id when it returns a choice. */
void shell_popup_commit(shell_state *s, int popup_id, int sel) {
    if (sel < 0) return;
    switch (popup_id) {
        case 3100: case 8100: case 3101: case 8101: {
            bool source=(popup_id==3100||popup_id==8100);
            osa_lang_info langs[128];int n=osa_app_languages(s->app,langs,128);char code[8]={0};
            if(source&&sel==0)osa_strlcpy(code,"auto",8);else{int i=sel-(source?1:0);if(i<0||i>=n)return;osa_strlcpy(code,langs[i].code,8);}
            char ns[8],nd[8];osa_strlcpy(ns,source?code:s->lang_src,8);osa_strlcpy(nd,source?s->lang_dst:code,8);
            osa_error e;if(osa_app_set_languages(s->app,ns,nd,&e)){osa_strlcpy(s->lang_src,ns,8);osa_strlcpy(s->lang_dst,nd,8);if(source)s->auto_detect=osa_streq(ns,"auto");shell_save_prefs(s);}else shell_set_message(s,true,"%s",e.msg);
            break;
        }
        case 3102:                      /* register */
            if (sel < MT_STYLE_COUNT) {
                s->style = (mt_style)sel;
                osa_app_set_style(s->app, s->style);
                shell_save_prefs(s);
            }
            break;
        case 6100:                      /* training language */
            s->train_lang = sel;
            break;
        case 6111: {                    /* training data: the in-app list */
            if (sel == 0) {
                s->ds_sel = -1;
                s->dataset_path[0] = 0;
                shell_set_message(s, false, "%s", kl(s, "training.datasetbuiltin"));
                break;
            }
            static osa_dataset_info ds[8];
            int nds = s->app ? osa_app_datasets(s->app, ds, 8) : 0;
            int i = sel - 1;
            if (i < 0 || i >= nds) break;
            if (ds[i].installed) {
                osa_strlcpy(s->dataset_path, ds[i].path, sizeof s->dataset_path);
                s->ds_sel = i;
                shell_set_message(s, false, "%s", kl(s, "training.datasetset"));
            } else {
                /* building a set writes a few hundred small WAVs; doing it when
                 * it is picked (not when the run starts) keeps the run quick and
                 * makes the choice verifiable before anything is trained */
                osa_error err;
                osa_err_ok(&err);
                char path[OSAI_MAX_PATH];
                if (osa_app_install_dataset(s->app, ds[i].id, path, sizeof path, &err)) {
                    osa_strlcpy(s->dataset_path, path, sizeof s->dataset_path);
                    s->ds_sel = i;
                    shell_set_message(s, false, "%s", kl(s, "training.datasetready"));
                } else {
                    shell_set_message(s, true, "%s: %s", kl(s, "msg.failed"), err.msg);
                }
            }
            break;
        }
        default:
            break;
    }
}
