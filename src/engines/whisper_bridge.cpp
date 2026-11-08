/* whisper_bridge.cpp — the only C++ in the application's own sources. It wraps
 * whisper.cpp behind the plain C interface in whisper_bridge.h. Built without
 * exceptions or RTTI (see the Makefile); every failure is an error code. */
#include "whisper_bridge.h"
#include "whisper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct wsp_ctx {
    struct whisper_context *ctx;
    char name[64];
    int  n_threads;
    volatile int *cancel;
};

/* whisper.cpp logs model loading to stderr; a GUI build has no stderr, and the
 * application reports its own errors, so the library's log is dropped. */
static void silent_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)level; (void)text; (void)ud;
}

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap) { strncpy(err, msg, cap - 1); err[cap - 1] = 0; }
}

static bool g_log_set = false;

bool wsp_is_model_file(const char *path) {
    if (!path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char m[4] = {0, 0, 0, 0};
    size_t got = fread(m, 1, 4, f);
    fclose(f);
    /* "ggml" as a little-endian uint32 — the magic whisper.cpp checks */
    return got == 4 && ((m[0] == 0x6c && m[1] == 0x6d && m[2] == 0x67 && m[3] == 0x67) || (m[0]=='G'&&m[1]=='G'&&m[2]=='U'&&m[3]=='F'));
}

static wsp_ctx *load_impl(const char *path, char *err, size_t cap) {
    if (!path || !*path) { set_err(err, cap, "no model path"); return NULL; }
    if (!g_log_set) { whisper_log_set(silent_log, NULL); g_log_set = true; }
    if (!wsp_is_model_file(path)) { set_err(err, cap, "not a whisper model file"); return NULL; }

    struct whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;                       /* CPU only: the same on every PC */
    struct whisper_context *ctx = whisper_init_from_file_with_params(path, cp);
    if (!ctx) { set_err(err, cap, "cannot load the whisper model"); return NULL; }

    wsp_ctx *c = (wsp_ctx *)calloc(1, sizeof *c);
    if (!c) { whisper_free(ctx); set_err(err, cap, "out of memory"); return NULL; }
    c->ctx = ctx;
    c->n_threads = 4;

    /* name = file name without directory or extension */
    const char *base = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!base || bslash > base)) base = bslash;
    base = base ? base + 1 : path;
    snprintf(c->name, sizeof c->name, "%s", base);
    char *dot = strrchr(c->name, '.');
    if (dot) *dot = 0;
    return c;
}

void wsp_free(wsp_ctx *c) {
    if (!c) return;
    if (c->ctx) whisper_free(c->ctx);
    free(c);
}

const char *wsp_model_name(const wsp_ctx *c) { return c ? c->name : ""; }
void wsp_set_cancel(wsp_ctx *c, volatile int *cancel) { if(c) c->cancel=cancel; }
int wsp_languages(wsp_ctx *c,const char **codes,int max) { if(!c||!codes||max<=0)return 0; int n=0; for(int i=0;i<=whisper_lang_max_id()&&n<max;i++){const char*s=whisper_lang_str(i);if(s&&*s&&whisper_lang_id(s)>=0)codes[n++]=s;} return n; }
static bool abort_cb(void *ud){volatile int *p=(volatile int*)ud;return p&&*p;}

static bool transcribe_impl(wsp_ctx *c, const float *pcm, int64_t n, const char *lang,
                            wsp_result *out, char *err, size_t cap) {
    if (!c || !c->ctx || !pcm || n <= 0 || !out) { set_err(err, cap, "no audio"); return false; }
    memset(out, 0, sizeof *out);

    struct whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.n_threads = c->n_threads;
    wp.print_progress = false;
    wp.print_realtime = false;
    wp.print_timestamps = false;
    wp.print_special = false;
    wp.no_context = true;                     /* each pass stands on its own */
    wp.translate = false;
    wp.abort_callback=abort_cb; wp.abort_callback_user_data=(void*)c->cancel;
    wp.suppress_blank=true; wp.suppress_nst=true; wp.temperature_inc=0.0f;
    wp.no_speech_thold=0.6f; wp.logprob_thold=-1.0f; wp.entropy_thold=2.4f;
    if (lang && *lang) {
        if (whisper_lang_id(lang)<0) { set_err(err,cap,"unsupported speech language"); return false; }
        if (!whisper_is_multilingual(c->ctx) && strcmp(lang, "en") != 0) {
            set_err(err, cap, "this model only understands English");
            return false;
        }
        wp.language = lang;
        wp.detect_language = false;
    } else {
        wp.language = "auto";
        wp.detect_language = true;
    }

    if (n > INT_MAX) { set_err(err,cap,"audio is too long for one pass"); return false; }
    if (whisper_full(c->ctx, wp, pcm, (int)n) != 0) {
        set_err(err, cap, c->cancel&&*c->cancel?"recognition cancelled":"recognition failed");
        return false;
    }

    const char *lid = whisper_lang_str(whisper_full_lang_id(c->ctx));
    snprintf(out->language, sizeof out->language, "%s", lid ? lid : (lang ? lang : ""));

    /* Text, and the mean probability of the text tokens (ids below EOT) as the
     * confidence. Special tokens — timestamps, EOT — are not words. */
    const whisper_token eot = whisper_token_eot(c->ctx);
    double psum = 0.0;
    int    pn = 0;
    size_t used = 0;
    int nseg = whisper_full_n_segments(c->ctx);
    for (int i = 0; i < nseg; i++) {
        const char *t = whisper_full_get_segment_text(c->ctx, i);
        if (t) {
            size_t tl = strlen(t);
            size_t extra=(used && t[0]!=' ' && t[0]!='\n')?1:0;
            if (used + tl + extra + 1 >= sizeof out->text) { set_err(err,cap,"transcript exceeds 64 KiB; split the audio"); return false; }
            if(extra)out->text[used++]=' ';
            memcpy(out->text+used,t,tl);used+=tl;out->text[used]=0;
        }
        int nt = whisper_full_n_tokens(c->ctx, i);
        for (int j = 0; j < nt; j++) {
            if (whisper_full_get_token_id(c->ctx, i, j) >= eot) continue;
            psum += whisper_full_get_token_p(c->ctx, i, j);
            pn++;
        }
    }
    out->confidence = pn ? (float)(psum / pn) : 0.0f;
    return true;
}

/* whisper.cpp reports a corrupt model by throwing (std::bad_alloc on a garbage
 * header), so every entry point catches: a bad file must never end the program. */
wsp_ctx *wsp_load(const char *path, char *err, size_t cap) {
    try {
        return load_impl(path, err, cap);
    } catch (...) {
        set_err(err, cap, "the whisper model file is damaged");
        return NULL;
    }
}

bool wsp_transcribe(wsp_ctx *c, const float *pcm, int64_t n, const char *lang,
                    wsp_result *out, char *err, size_t cap) {
    try {
        return transcribe_impl(c, pcm, n, lang, out, err, cap);
    } catch (...) {
        set_err(err, cap, "recognition failed inside the whisper engine");
        return false;
    }
}
