#include "asr.h"
#include "nn.h"
#include "whisper_bridge.h"
#include "../core/tts.h"
#include "../core/osai_platform.h"
#include <math.h>

/* =====================================================================
 *  Model container ".osam"
 * ===================================================================== */
#define OSAM_MAGIC 0x4D41534Fu   /* "OSAM" */
#define OSAM_VERSION 1

typedef struct osam_header {
    uint32_t magic, version;
    uint32_t n_tokens, n_confusions, n_bigrams;
    uint32_t layer_specs[4], n_specs;
    char     name[128], version_s[32], language[8], base_model[128];
    char     license[128], source_url[512], note[256];
    char     backend[16];
    uint32_t feat_dim;
    uint32_t n_exemplars;
    uint32_t tmpl_floats;
} osam_header;

void asr_init(void) {}
void asr_shutdown(void) {}

static asr_model *model_alloc(void) {
    asr_model *m = (asr_model *)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->tokens = (asr_token *)calloc(ASR_MAX_TOKENS, sizeof(asr_token));
    m->confusions = calloc(64, sizeof *m->confusions);
    m->bigrams = calloc(ASR_MAX_LM, sizeof *m->bigrams);
    if (!m->tokens || !m->confusions || !m->bigrams) { asr_model_free(m); return NULL; }
    osa_strlcpy(m->name, "OpenSpeech AI fixture recognizer", sizeof m->name);
    osa_strlcpy(m->version, OSAI_VERSION, sizeof m->version);
    osa_strlcpy(m->language, "fa", sizeof m->language);
    osa_strlcpy(m->base_model, "builtin-fixture-v1", sizeof m->base_model);
    osa_strlcpy(m->license, "GPL-3.0 (this project)", sizeof m->license);
    return m;
}

void asr_model_free(asr_model *m) {
    if (!m) return;
    if (m->wsp) wsp_free(m->wsp);
    if (m->net) nn_free((nn_model *)m->net);
    free(m->tokens);
    free(m->tmpl_feat);
    free(m->exemplars);
    free(m->confusions);
    free(m->bigrams);
    free(m);
}

/* --------------------- builtin fixture language packs --------------------- */
/* A compact lexicon with romanized pronunciation "codes" used to synthesise
 * frame targets deterministically, so the app is fully functional (and
 * testable) before any downloaded model is installed. Real models trained by
 * tools/train.py or the in-app trainer replace it without touching this code. */
/* Per-frame feature: log-mel + delta, mean-removed per frame. Dimension is
 * 2*OSAI_MEL_BANDS = 80. Shared by the trainer, recognizer and TTS bootstrap so
 * every consumer sees identical features. */
int asr_frame_features(const float *samples, int64_t n, float **feat, int *dim) {
    float *mel = NULL;
    int bands = 0;
    int64_t nf = osa_mel_spectrogram(samples, n, &mel, &bands);
    if (dim) *dim = 2 * OSAI_MEL_BANDS;
    if (feat) *feat = NULL;
    if (nf <= 0) { free(mel); return 0; }
    int D = 2 * OSAI_MEL_BANDS;
    float *f = (float *)malloc((size_t)nf * (size_t)D * sizeof(float));
    if (!f) { free(mel); return 0; }
    for (int64_t t = 0; t < nf; t++) {
        const float *cur = mel + (size_t)t * OSAI_MEL_BANDS;
        const float *prv = mel + (size_t)(t > 0 ? t - 1 : 0) * OSAI_MEL_BANDS;
        const float *nxt = mel + (size_t)(t + 1 < nf ? t + 1 : nf - 1) * OSAI_MEL_BANDS;
        float *out = f + (size_t)t * D;
        float mu = 0;
        for (int b = 0; b < OSAI_MEL_BANDS; b++) { out[b] = cur[b]; mu += cur[b]; }
        mu /= (float)OSAI_MEL_BANDS;
        for (int b = 0; b < OSAI_MEL_BANDS; b++) out[b] -= mu;
        for (int b = 0; b < OSAI_MEL_BANDS; b++)
            out[OSAI_MEL_BANDS + b] = (nxt[b] - prv[b]) * 0.5f;
    }
    free(mel);
    *feat = f;
    return (int)nf;
}


static bool fixture_build(asr_model *m, const char *lang, osa_error *err) {
    int nw = 0;
    const tts_word_pair *W = osai_lexicon(lang, &nw);
    const char *L = (lang && *lang) ? lang : "fa";
    osa_strlcpy(m->language, L, sizeof m->language);
    osa_strlcpy(m->backend, "exemplar", sizeof m->backend);
    m->n_tokens = nw;
    m->feat_dim = 2 * OSAI_MEL_BANDS;
    for (int i = 0; i < nw; i++) {
        osa_strlcpy(m->tokens[i].text, W[i].w, sizeof m->tokens[i].text);
        m->tokens[i].frames = 4;
    }
    /* synthesize one template per word with the bundled formant synth */
    size_t capacity = 0;
    for (int i = 0; i < nw; i++) {
        int64_t n = 0;
        float *pcm = tts_speak_code(W[i].code, &n);
        if (!pcm) continue;
        float *f = NULL; int D = 0;
        int nf = asr_frame_features(pcm, n, &f, &D);
        free(pcm);
        free(f);
        if (nf > 0) capacity += (size_t)nf;
    }
    m->n_exemplars = 0;
    m->n_exemplar_cap = nw * 2 + 8;
    m->exemplars = (asr_exemplar *)calloc((size_t)m->n_exemplar_cap, sizeof(asr_exemplar));
    m->tmpl_feat = (float *)malloc((capacity + 256) * (size_t)m->feat_dim * sizeof(float));
    if (!m->exemplars || !m->tmpl_feat) {
        osa_err_set(err, "out of memory");
        return false;
    }
    size_t write_at = 0;
    for (int i = 0; i < nw; i++) {
        int64_t n = 0;
        float *pcm = tts_speak_code(W[i].code, &n);
        if (!pcm) continue;
        float *f = NULL; int D = 0;
        int nf = asr_frame_features(pcm, n, &f, &D);
        free(pcm);
        if (nf <= 1) { free(f); continue; }
        m->exemplars[m->n_exemplars].token = i;
        m->exemplars[m->n_exemplars].start = (int)write_at;
        m->exemplars[m->n_exemplars].nframes = nf;
        m->n_exemplars++;
        memcpy(m->tmpl_feat + write_at * (size_t)D, f, (size_t)nf * (size_t)D * sizeof(float));
        write_at += (size_t)nf;
        free(f);
    }
    osa_strlcpy(m->name, osa_starts(L, "en") ? "OpenSpeech AI builtin (English)"
                                             : "OpenSpeech AI builtin (Persian)",
                sizeof m->name);

    /* deterministic bigram LM from the demo sentences */
    const char *sent_fa[] = {
        "سلام حال شما خوب است", "این یک آزمایش نرم افزار هوش مصنوعی است",
        "لطفا فایل صوتی را انتخاب کنید", "ترجمه متن به زبان انگلیسی",
        "مدل آموزش داده شده را ذخیره کنید", "تشخیص گفتار فارسی دقیق است",
        "امروز هوا خیلی قشنگ است", "سیستم به صورت آفلاین کار می کند",
        "میکروفون را روشن کنید", "نتیجه ترجمه را ذخیره کنید",
    };
    const char *sent_en[] = {
        "hello world", "this is a speech recognition test",
        "the quick brown fox jumps over the lazy dog", "select an audio file",
        "start recording the microphone", "artificial intelligence system",
        "the translation is very nice", "today the weather is very nice",
        "save the text file", "offline speech recognition works",
    };
    const char **S = osa_starts(L, "en") ? sent_en : sent_fa;
    int nb = 0;
    for (int si = 0; si < 10 && nb < ASR_MAX_LM - 2; si++) {
        char buf[512];
        osa_strlcpy(buf, S[si], sizeof buf);
        char *save = NULL;
        char *tok = strtok_r(buf, " ", &save);
        int prev = -1;
        while (tok) {
            int id = -1;
            for (int w = 0; w < nw; w++) if (osa_streq(W[w].w, tok)) { id = w; break; }
            if (id >= 0) {
                m->unigram[id] += 1.0f;
                if (prev >= 0 && nb < ASR_MAX_LM) {
                    m->bigrams[nb].a = prev;
                    m->bigrams[nb].b = id;
                    m->bigrams[nb].logp = -0.35f;
                    nb++;
                }
                prev = id;
            }
            tok = strtok_r(NULL, " ", &save);
        }
    }
    m->n_bigrams = nb;
    float umax = 1.0f;
    for (int i = 0; i < nw; i++) if (m->unigram[i] > umax) umax = m->unigram[i];
    for (int i = 0; i < nw; i++) m->unigram[i] = logf((m->unigram[i] + 0.25f) / (umax + 0.25f));
    osa_strlcpy(m->base_model, "builtin-exemplar-v1", sizeof m->base_model);
    osa_strlcpy(m->license, "GPL-3.0 (this project)", sizeof m->license);
    osa_strlcpy(m->training_note, "builtin exemplars generated by the bundled formant synth",
                sizeof m->training_note);
    osa_err_ok(err);
    return true;
}

/* ------------------------- exemplar management ------------------------- */
bool asr_model_add_exemplar(asr_model *m, const char *word, const float *samples, int64_t n,
                            osa_error *err) {
    if (!m || !word || !samples || n <= 0) { osa_err_set(err, "bad exemplar input"); return false; }
    if (m->wsp) { osa_err_set(err, "a whisper model learns from its own training, not from exemplars"); return false; }
    int tok = -1;
    for (int i = 0; i < m->n_tokens; i++) if (osa_streq(m->tokens[i].text, word)) { tok = i; break; }
    if (tok < 0) {
        if (m->n_tokens >= ASR_MAX_TOKENS) { osa_err_set(err, "lexicon full"); return false; }
        tok = m->n_tokens++;
        osa_strlcpy(m->tokens[tok].text, word, sizeof m->tokens[tok].text);
        m->tokens[tok].frames = 4;
    }
    float *f = NULL; int D = 0;
    int nf = asr_frame_features(samples, n, &f, &D);
    if (nf <= 1) { free(f); osa_err_set(err, "recording too short"); return false; }
    if (m->n_exemplars >= m->n_exemplar_cap) {
        int nc = m->n_exemplar_cap ? m->n_exemplar_cap * 2 : 64;
        asr_exemplar *ne = (asr_exemplar *)realloc(m->exemplars, (size_t)nc * sizeof(asr_exemplar));
        if (!ne) { free(f); osa_err_set(err, "out of memory"); return false; }
        m->exemplars = ne;
        m->n_exemplar_cap = nc;
    }
    int64_t used = 0;
    for (int i = 0; i < m->n_exemplars; i++) used += m->exemplars[i].nframes;
    float *nt = (float *)realloc(m->tmpl_feat, (size_t)(used + nf) * (size_t)D * sizeof(float));
    if (!nt) { free(f); osa_err_set(err, "out of memory"); return false; }
    m->tmpl_feat = nt;
    memcpy(m->tmpl_feat + (size_t)used * D, f, (size_t)nf * (size_t)D * sizeof(float));
    free(f);
    m->exemplars[m->n_exemplars].token = tok;
    m->exemplars[m->n_exemplars].start = (int)used;
    m->exemplars[m->n_exemplars].nframes = nf;
    m->n_exemplars++;
    m->feat_dim = D;
    osa_err_ok(err);
    return true;
}


asr_model *asr_model_fixture(const char *lang, osa_error *err) {
    asr_model *m = model_alloc();
    if (!m) { osa_err_set(err, "out of memory"); return NULL; }
    if (!fixture_build(m, lang && *lang ? lang : "fa", err)) {
        asr_model_free(m);
        return NULL;
    }
    return m;
}

/* ---- model card accessors ----------------------------------------- */
const char *asr_model_name(const asr_model *m)     { return m ? m->name : ""; }
const char *asr_model_language(const asr_model *m) { return m ? m->language : ""; }
const char *asr_model_backend(const asr_model *m)  { return m ? m->backend : ""; }
const char *asr_model_license(const asr_model *m)  { return m ? m->license : ""; }
const char *asr_model_source(const asr_model *m)   { return m ? m->source_url : ""; }
const char *asr_model_version(const asr_model *m)  { return m ? m->version : ""; }
const char *asr_model_base(const asr_model *m)     { return m ? m->base_model : ""; }

/* A stable id derived from the model's own name/backend when the container does
 * not carry one, so imported models always get a usable identifier. */
const char *asr_model_id(const asr_model *m) {
    static __thread char buf[64];
    if (!m) return "";
    if (m->name[0]) {
        /* "<backend>-<language>" is unique per shipped pack */
        if (osa_starts(m->name, "OpenSpeech AI builtin")) {
            snprintf(buf, sizeof buf, "builtin-asr-%s", m->language[0] ? m->language : "fa");
            return buf;
        }
    }
    return m->name;
}

bool asr_model_save(asr_model *m, const char *path, osa_error *err) {
    if (m && m->wsp) { osa_err_set(err, "a whisper model is not written by this application"); return false; }
    char tmp[OSAI_MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { osa_err_set(err, "cannot write %s", path); return false; }
    osam_header h;
    memset(&h, 0, sizeof h);
    h.magic = OSAM_MAGIC;
    h.version = OSAM_VERSION;
    h.n_tokens = (uint32_t)m->n_tokens;
    h.n_confusions = (uint32_t)m->n_confusions;
    h.n_bigrams = (uint32_t)m->n_bigrams;
    h.n_specs = (uint32_t)m->n_specs;
    for (int i = 0; i < 4; i++) h.layer_specs[i] = (uint32_t)m->layer_specs[i];
    osa_strlcpy(h.name, m->name, sizeof h.name);
    osa_strlcpy(h.version_s, m->version, sizeof h.version_s);
    osa_strlcpy(h.language, m->language, sizeof h.language);
    osa_strlcpy(h.base_model, m->base_model, sizeof h.base_model);
    osa_strlcpy(h.license, m->license, sizeof h.license);
    osa_strlcpy(h.source_url, m->source_url, sizeof h.source_url);
    osa_strlcpy(h.note, m->training_note, sizeof h.note);
    osa_strlcpy(h.backend, m->backend, sizeof h.backend);
    h.feat_dim = (uint32_t)m->feat_dim;
    h.n_exemplars = (uint32_t)m->n_exemplars;
    h.tmpl_floats = (uint32_t)(m->n_exemplars > 0 ? 0 : 0);
    fwrite(&h, sizeof h, 1, f);
    /* template blob size is derived from the exemplars, written below */
    fwrite(m->tokens, sizeof(asr_token), (size_t)m->n_tokens, f);
    fwrite(m->unigram, sizeof(float), ASR_MAX_TOKENS, f);
    fwrite(m->bigrams, sizeof(*m->bigrams), (size_t)m->n_bigrams, f);
    fwrite(m->confusions, sizeof(*m->confusions), (size_t)m->n_confusions, f);
    if (m->n_exemplars > 0 && m->feat_dim > 0) {
        fwrite(m->exemplars, sizeof(asr_exemplar), (size_t)m->n_exemplars, f);
        int64_t total = 0;
        for (int i = 0; i < m->n_exemplars; i++)
            total += (int64_t)m->exemplars[i].nframes * m->feat_dim;
        fwrite(m->tmpl_feat, sizeof(float), (size_t)total, f);
    }
    if (m->net) nn_save((nn_model *)m->net, f);
    fclose(f);
    if (!osa_file_rename(tmp, path)) {
        osa_file_remove(tmp);
        osa_err_set(err, "cannot finalize %s", path);
        return false;
    }
    char sha[65];
    osa_sha256_file(path, sha, NULL);
    osa_strlcpy(m->sha256, sha, sizeof m->sha256);
    osa_strlcpy(m->path, path, sizeof m->path);
    osa_err_ok(err);
    return true;
}

asr_model *asr_model_load(const char *path, osa_error *err) {
    if (wsp_is_model_file(path)) {
        char werr[160];
        wsp_ctx *w = wsp_load(path, werr, sizeof werr);
        if (!w) { osa_err_set(err, "%s", werr); return NULL; }
        asr_model *m = (asr_model *)calloc(1, sizeof *m);
        if (!m) { wsp_free(w); osa_err_set(err, "out of memory"); return NULL; }
        m->wsp = w;
        osa_strlcpy(m->backend, "whisper", sizeof m->backend);
        osa_strlcpy(m->name, wsp_model_name(w), sizeof m->name);
        osa_strlcpy(m->language, "multi", sizeof m->language);
        osa_strlcpy(m->version, "ggml", sizeof m->version);
        osa_strlcpy(m->base_model, "openai-whisper", sizeof m->base_model);
        osa_strlcpy(m->license, "MIT (OpenAI Whisper weights)", sizeof m->license);
        osa_strlcpy(m->path, path, sizeof m->path);
        osa_err_ok(err);
        return m;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { osa_err_set(err, "cannot open model %s", path); return NULL; }
    osam_header h;
    if (fread(&h, sizeof h, 1, f) != 1 || h.magic != OSAM_MAGIC || h.version != OSAM_VERSION) {
        fclose(f);
        osa_err_set(err, "not a valid .osam model");
        return NULL;
    }
    if (h.n_tokens == 0 || h.n_tokens > ASR_MAX_TOKENS) {
        fclose(f);
        osa_err_set(err, "model token table out of range");
        return NULL;
    }
    asr_model *m = model_alloc();
    if (!m) { fclose(f); osa_err_set(err, "out of memory"); return NULL; }
    osa_strlcpy(m->name, h.name, sizeof m->name);
    osa_strlcpy(m->version, h.version_s, sizeof m->version);
    osa_strlcpy(m->language, h.language, sizeof m->language);
    osa_strlcpy(m->base_model, h.base_model, sizeof m->base_model);
    osa_strlcpy(m->license, h.license, sizeof m->license);
    osa_strlcpy(m->source_url, h.source_url, sizeof m->source_url);
    osa_strlcpy(m->training_note, h.note, sizeof m->training_note);
    osa_strlcpy(m->backend, h.backend, sizeof m->backend);
    m->n_tokens = (int)h.n_tokens;
    m->n_specs = (int)h.n_specs;
    for (int i = 0; i < 4; i++) m->layer_specs[i] = (int)h.layer_specs[i];
    bool ok = fread(m->tokens, sizeof(asr_token), (size_t)m->n_tokens, f) == (size_t)m->n_tokens;
    ok = ok && fread(m->unigram, sizeof(float), ASR_MAX_TOKENS, f) == ASR_MAX_TOKENS;
    ok = ok && h.n_bigrams <= ASR_MAX_LM;
    if (ok && h.n_bigrams)
        ok = fread(m->bigrams, sizeof(*m->bigrams), h.n_bigrams, f) == h.n_bigrams;
    m->n_bigrams = ok ? (int)h.n_bigrams : 0;
    if (ok && h.n_confusions)
        ok = fread(m->confusions, sizeof(*m->confusions), h.n_confusions, f) == h.n_confusions;
    m->n_confusions = ok ? (int)h.n_confusions : 0;
    if (ok && h.n_exemplars > 0 && h.feat_dim > 0) {
        m->feat_dim = (int)h.feat_dim;
        m->n_exemplars = (int)h.n_exemplars;
        m->n_exemplar_cap = m->n_exemplars + 32;
        m->exemplars = (asr_exemplar *)calloc((size_t)m->n_exemplar_cap, sizeof(asr_exemplar));
        ok = m->exemplars && fread(m->exemplars, sizeof(asr_exemplar), (size_t)m->n_exemplars, f)
                                  == (size_t)m->n_exemplars;
        if (ok) {
            int64_t total = 0;
            for (int i = 0; i < m->n_exemplars; i++)
                total += (int64_t)m->exemplars[i].nframes * m->feat_dim;
            m->tmpl_feat = (float *)malloc((size_t)total * sizeof(float));
            ok = m->tmpl_feat && fread(m->tmpl_feat, sizeof(float), (size_t)total, f) == (size_t)total;
        }
    }
    if (ok && h.backend[0] && osa_strieq(h.backend, "exemplar")) {
        /* exemplar packs carry no neural net */
    } else if (ok) {
        nn_model *net = nn_create(1, 1, 1, 1, 0);
        if (net && nn_load(net, f)) {
            if (net->n_out == m->n_tokens) m->net = net;
            else { nn_free(net); net = NULL; ok = false; }
        } else { if (net) nn_free(net); ok = false; }
    }
    fclose(f);
    if (!ok) {
        osa_err_set(err, "corrupt model payload");
        asr_model_free(m);
        return NULL;
    }
    osa_strlcpy(m->path, path, sizeof m->path);
    osa_sha256_file(path, m->sha256, NULL);
    osa_err_ok(err);
    return m;
}

/* =====================================================================
 *  Features
 * ===================================================================== */
/* =====================================================================
 *  CTC decoding
 * ===================================================================== */
typedef struct frame_hyp { int tok; float prob; } frame_hyp;

static float bigram_logp(const asr_model *m, int a, int b) {
    for (int i = 0; i < m->n_bigrams; i++)
        if (m->bigrams[i].a == a && m->bigrams[i].b == b) return m->bigrams[i].logp;
    return -2.2f;
}

/* CTC greedy decode: used by the trained ("mlp"/"ctc") backend, where the network
 * emits one posterior vector per frame. The exemplar backend below does not need
 * it, which is why it is `static` and only reachable through asr_decode_ctc(). */
static int ctc_decode(const float *probs, int nframes, int ntok, int blank,
                      frame_hyp *out, float *frame_conf) {
    int prev = -1, k = 0;
    double conf = 0;
    int conf_n = 0;
    for (int t = 0; t < nframes; t++) {
        const float *p = probs + (size_t)t * ntok;
        int best = nn_argmax(p, ntok);
        if (best != blank && best != prev) {
            if (k < ASR_MAX_TOKENS) {
                out[k].tok = best;
                out[k].prob = p[best];
                k++;
            }
        }
        if (best != blank) {
            /* confidence of emitted frames only */
            if (best != prev) { conf += p[best]; conf_n++; }
        }
        prev = best;
    }
    if (frame_conf) *frame_conf = conf_n ? (float)(conf / conf_n) : 0.0f;
    return k;
}

/* Rescore the hypothesis with the LM and drop tokens the LM strongly rejects. */
static int lm_rescore(const asr_model *m, frame_hyp *h, int n, float lm_weight) {
    if (n <= 1) return n;
    int kept = 0;
    int prev = -1;
    for (int i = 0; i < n; i++) {
        float lm = prev >= 0 ? bigram_logp(m, prev, h[i].tok) : m->unigram[h[i].tok];
        float score = logf(fmax(h[i].prob, 1e-6f)) + lm_weight * lm;
        (void)score;
        /* keep the token; the score feeds confidence below */
        h[kept] = h[i];
        h[kept].prob = h[i].prob * (0.6f + 0.1f * osa_clampf(lm + 2.2f, 0, 2.2f));
        prev = h[i].tok;
        kept++;
    }
    return kept;
}

/* =====================================================================
 *  Language identification
 * ===================================================================== */
const char *asr_detect_language(const float *samples, int64_t n) {
    /* Token-free heuristic on spectral shape: Persian/Arabic speech has more
     * energy in 200-1200 Hz, English in 1200-3000 Hz; we also use the zero
     * crossing rate as a voicing cue. Returns "fa"/"en" (first release set). */
    if (!samples || n < 8000) return "fa";
    double e_low = 0, e_high = 0, zcr = 0;
    int win = 320;
    int frames = (int)(n / win);
    if (frames > 400) frames = 400;
    for (int f = 0; f < frames; f++) {
        const float *p = samples + (int64_t)f * win;
        double s1 = 0, s2 = 0;
        for (int i = 1; i < win; i++) {
            s1 += (double)p[i] * p[i-1];
            s2 += (double)p[i] * p[i];
            if ((p[i] > 0) != (p[i-1] > 0)) zcr++;
        }
        /* normalized autocorrelation at 1 and 2 lags */
        if (s2 > 1e-9) { e_low += s1 / s2; }
        e_high += s2;
    }
    double ac1 = frames ? e_low / frames : 0;
    double z = frames > 0 ? zcr / (double)(frames * win) : 0;
    (void)e_high;
    if (ac1 > 0.62 && z < 0.16) return "fa";
    return "en";
}

/* =====================================================================
 *  Text normalization (Persian: ی/ک/نیم‌فاصله/ارقام)
 * ===================================================================== */
static void append_rune(char *out, size_t cap, size_t *o, osa_rune r) {
    if (*o + 5 > cap) return;
    *o += (size_t)osa_utf8_encode(r, out + *o);
}

void asr_normalize_text(const char *in, const char *lang, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = 0;
    if (!in) return;
    bool fa = !lang || osa_starts(lang, "fa") || osa_starts(lang, "ar");
    size_t o = 0;
    size_t len = strlen(in);
    size_t i = 0;
    bool prev_space = true;
    while (i < len) {
        osa_rune r = 0;
        int n = osa_utf8_decode(in + i, len - i, &r);
        if (n <= 0) n = 1;
        i += (size_t)n;
        if (fa) {
            switch (r) {
                case 0x064A: r = 0x06CC; break;             /* ي -> ی */
                case 0x0643: r = 0x06A9; break;             /* ك -> ک */
                case 0x0629: r = 0x0647; break;             /* ة -> ه */
                case 0x200C: r = 0x0020; break;             /* ZWNJ -> space for display */
                case 0x06F0: case 0x06F1: case 0x06F2: case 0x06F3: case 0x06F4:
                case 0x06F5: case 0x06F6: case 0x06F7: case 0x06F8: case 0x06F9:
                    r = (osa_rune)('0' + (r - 0x06F0));     /* keep ASCII digits */
                    break;
                case 0x0660: case 0x0661: case 0x0662: case 0x0663: case 0x0664:
                case 0x0665: case 0x0666: case 0x0667: case 0x0668: case 0x0669:
                    r = (osa_rune)('0' + (r - 0x0660));
                    break;
                default: break;
            }
        }
        if (r == ' ' || r == '\t' || r == '\n' || r == '\r') {
            if (prev_space) continue;
            append_rune(out, cap, &o, ' ');
            prev_space = true;
            continue;
        }
        append_rune(out, cap, &o, r);
        prev_space = false;
    }
    /* trim */
    while (o > 1 && out[o-1] == ' ') { out[--o] = 0; }
    out[o] = 0;
}

/* =====================================================================
 *  Metrics
 * ===================================================================== */
static int tokenize(const char *s, char toks[][128], int max) {
    int n = 0;
    size_t len = strlen(s), i = 0;
    while (i < len && n < max) {
        while (i < len && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t')) i++;
        size_t st = i;
        while (i < len && !(s[i] == ' ' || s[i] == '\n' || s[i] == '\t')) i++;
        size_t l = i - st;
        if (l > 127) l = 127;
        if (l) { memcpy(toks[n], s + st, l); toks[n][l] = 0; n++; }
    }
    return n;
}

static double edit_distance(const int *a, int na, const int *b, int nb) {
    if (!na) return (double)nb;
    if (!nb) return (double)na;
    int *prev = (int *)malloc((size_t)(nb + 1) * sizeof(int));
    int *cur = (int *)malloc((size_t)(nb + 1) * sizeof(int));
    if (!prev || !cur) { free(prev); free(cur); return (double)(na > nb ? na : nb); }
    for (int j = 0; j <= nb; j++) prev[j] = j;
    for (int i = 1; i <= na; i++) {
        cur[0] = i;
        for (int j = 1; j <= nb; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int d = prev[j-1] + cost;
            if (prev[j] + 1 < d) d = prev[j] + 1;
            if (cur[j-1] + 1 < d) d = cur[j-1] + 1;
            cur[j] = d;
        }
        int *t = prev; prev = cur; cur = t;
    }
    double r = (double)prev[nb];
    free(prev); free(cur);
    return r;
}

double asr_wer(const char *ref, const char *hyp, const char *lang) {
    (void)lang;
    static char r[8192], h[8192];
    asr_normalize_text(ref ? ref : "", lang, r, sizeof r);
    asr_normalize_text(hyp ? hyp : "", lang, h, sizeof h);
    static char ra[512][128], ha[512][128];
    int na = tokenize(r, ra, 512), nb = tokenize(h, ha, 512);
    if (na == 0) return nb == 0 ? 0.0 : 1.0;
    int *ia = (int *)malloc((size_t)na * sizeof(int));
    int *ib = (int *)malloc((size_t)nb * sizeof(int));
    /* map tokens to integer ids for O(n*m) DP */
    osa_arena pa, pb;
    osa_arena_init(&pa); osa_arena_init(&pb);
    for (int i = 0; i < na; i++) {
        int id = -1;
        for (int j = 0; j < i; j++) if (osa_streq(ra[i], ra[j])) { id = j; break; }
        if (id < 0) id = i;
        ia[i] = id;
    }
    for (int i = 0; i < nb; i++) {
        int id = -1;
        for (int j = 0; j < i; j++) if (osa_streq(ha[i], ha[j])) { id = ib[j]; break; }
        if (id < 0) {
            id = 500 + i;                       /* ids must not collide with ia[] */
            for (int k = 0; k < na; k++)
                if (osa_streq(ha[i], ra[k])) { id = ia[k]; break; }
        }
        ib[i] = id;
    }
    double d = edit_distance(ia, na, ib, nb);
    free(ia); free(ib);
    osa_arena_free(&pa); osa_arena_free(&pb);
    return d / (double)na;
}

double asr_cer(const char *ref, const char *hyp) {
    static char r[8192], h[8192];
    asr_normalize_text(ref ? ref : "", "", r, sizeof r);
    asr_normalize_text(hyp ? hyp : "", "", h, sizeof h);
    /* compare by rune */
    int nr = 0, nh = 0;
    int ir[4096], ih[4096];
    {
        size_t len = strlen(r), i = 0;
        while (i < len && nr < 4096) {
            osa_rune c = 0; int n = osa_utf8_decode(r + i, len - i, &c);
            if (n <= 0) break;
            ir[nr++] = (int)c;
            i += (size_t)n;
        }
        len = strlen(h); i = 0;
        while (i < len && nh < 4096) {
            osa_rune c = 0; int n = osa_utf8_decode(h + i, len - i, &c);
            if (n <= 0) break;
            ih[nh++] = (int)c;
            i += (size_t)n;
        }
    }
    if (nr == 0) return nh == 0 ? 0.0 : 1.0;
    return edit_distance(ir, nr, ih, nh) / (double)nr;
}

/* =====================================================================
 *  Transcription
 * ===================================================================== */
void asr_result_free(asr_result *r) {
    if (!r) return;
    free(r->segments);
    memset(r, 0, sizeof *r);
}

typedef struct ctx_frame { float *feat; } ctx_frame;

/* --------------------------- DTW decoder ---------------------------
 * Each token owns one or more exemplars (frame sequences). We score every
 * exemplar with DTW against the utterance (normalized per frame) and then pick
 * the best non-overlapping chain of detections with a Viterbi pass that also
 * applies the n-gram language model. This is classic isolated-word spotting,
 * fully deterministic and dependency-free. */

typedef struct det { int token, ex; float score; int s0, s1; } det;

/* DTW word spotting with weighted-state DP. The matrix is indexed by absolute
 * utterance frame; the first template row is anchored at candidate frames inside
 * an anchor window, and every transition pays the local distance so a path
 * cannot collapse. The END of the match may be anywhere inside the duration
 * window, which is what lets one template spot a word at any position. */
/* ---------------------------------------------------------------------------
 * Sub-sequence DTW + Viterbi decoding.
 *
 * Each lexicon word ships one or more templates (the builtin bootstrap derives
 * them from the bundled formant synthesiser). `dtw_word` returns, for a fixed
 * start frame, the cost of the best duration-constrained path, so a word cannot
 * borrow acoustic evidence from its neighbours. The lattice holds one node per
 * (word, start) and a Viterbi pass tiles the utterance with non-overlapping,
 * adjacent words, rewarded by LM support.
 * ------------------------------------------------------------------------- */


/* A decoded lattice node: one word occupying [s0, s1). */
typedef struct node_s { int token, ex, s0, s1; float score; } node;

/* Distance matrix (template frame i vs utterance frame t), computed once per
 * template and reused by every candidate (start, length). Without this the
 * segmental DP spends all its time recomputing the same local distances. */
#if defined(_MSC_VER)
#define OSAI_TLS __declspec(thread)
#else
#define OSAI_TLS __thread
#endif
enum { ASR_DTB_MAXF = 8192, ASR_DTB_MAXT = 400 };
static OSAI_TLS float s_dtb[ASR_DTB_MAXT][ASR_DTB_MAXF];
static OSAI_TLS int s_dtb_tpl = -2, s_dtb_nf = -1;

/* Banded DTW with both endpoints fixed; normalised by the path length so long
 * and short templates are directly comparable. */
static float dtw_full(const asr_model *m, const asr_exemplar *ex, const float *feat,
                      int nframes, int s, int e) {
    int D = m->feat_dim;
    const float *tpl = m->tmpl_feat + (size_t)ex->start * D;
    int Ta = ex->nframes, Tb = e - s;
    if (Ta < 2 || Tb < 2 || s < 0 || e > nframes) return 1e30f;
    if (Ta > ASR_DTB_MAXT || nframes > ASR_DTB_MAXF) return 1e30f;
    if (s_dtb_tpl != ex->start || s_dtb_nf != nframes) {
        for (int i = 0; i < Ta; i++) {
            const float *a = tpl + (size_t)i * D;
            float *row = s_dtb[i];
            for (int t = 0; t < nframes; t++) {
                const float *b = feat + (size_t)t * D;
                float acc = 0;
                for (int k = 0; k < D; k++) { float d = a[k] - b[k]; acc += d * d; }
                row[t] = sqrtf(acc / (float)D);
            }
        }
        s_dtb_tpl = ex->start; s_dtb_nf = nframes;
    }
    static OSAI_TLS float g[2][ASR_DTB_MAXF];
    int band = (int)((float)(Ta > Tb ? Ta : Tb) * 0.55f) + 4;
    g[0][0] = s_dtb[0][s];
    for (int j = 1; j < Tb; j++) g[0][j] = g[0][j-1] + s_dtb[0][s + j];
    int prev = 0, cur = 1;
    for (int i = 1; i < Ta; i++) {
        const float *Li = s_dtb[i] + s;
        for (int j = 0; j < Tb; j++) g[cur][j] = 1e30f;
        int center = (int)((float)i * (float)Tb / (float)Ta);
        int jlo = center - band, jhi = center + band;
        if (jlo < 1) jlo = 1;
        if (jhi > Tb - 1) jhi = Tb - 1;
        for (int j = jlo; j <= jhi; j++) {
            float best = g[prev][j];
            if (g[prev][j-1] < best) best = g[prev][j-1];
            if (g[cur][j-1] < best) best = g[cur][j-1];
            if (best >= 1e29f) continue;
            g[cur][j] = Li[j] + best;
        }
        int t = prev; prev = cur; cur = t;
    }
    float total = g[prev][Tb - 1];
    if (total >= 1e29f) return 1e30f;
    return total / (float)(Ta + Tb);
}

/* Mean local distance when template `ex` is warped linearly onto frames [s, s+L).
 *
 * The frame distances are cached per template (one row per template frame over
 * the whole utterance): the duration sweep calls this once per candidate length
 * for the same template and start, so caching turns the inner loop into an
 * O(T) gather instead of an O(T*D) recomputation. */
enum { ASR_TD_MAXF = 8192, ASR_TD_MAXT = 400 };
#if defined(_MSC_VER)
#define OSAI_TLS __declspec(thread)
#else
#define OSAI_TLS __thread
#endif
static OSAI_TLS float s_td[ASR_TD_MAXT][ASR_TD_MAXF];
static OSAI_TLS int s_td_tpl = -2, s_td_nf = -1;
/* Transposed copy: s_tdt[frame][template_row]. The duration sweep reads one row
 * per template row and reads it in increasing frame order, so walking the
 * transposed layout turns the gather into sequential memory access. */
static OSAI_TLS float s_tdt[ASR_TD_MAXF][64];
static OSAI_TLS int s_tdt_tpl = -2, s_tdt_nf = -1;

static float dtw_fixed_avg(const asr_model *m, const asr_exemplar *ex, const float *feat,
                           int nframes, int s, int L) {
    int D = m->feat_dim;
    const float *tpl = m->tmpl_feat + (size_t)ex->start * D;
    int T = ex->nframes;
    if (T < 2 || L < 2 || s < 0 || s + L > nframes) return 1e30f;
    if (T > ASR_TD_MAXT || nframes > ASR_TD_MAXF) return 1e30f;
    if (s_td_tpl != ex->start || s_td_nf != nframes) {
        for (int i = 0; i < T; i++) {
            const float *a = tpl + (size_t)i * D;
            float *row = s_td[i];
            for (int t = 0; t < nframes; t++) {
                const float *b = feat + (size_t)t * D;
                float acc = 0;
                for (int k = 0; k < D; k++) { float d = a[k] - b[k]; acc += d * d; }
                row[t] = sqrtf(acc / (float)D);
            }
        }
        s_td_tpl = ex->start; s_td_nf = nframes;
        s_tdt_tpl = -2;                        /* transposed copy is stale */
    }
    if (T <= 64 && (s_tdt_tpl != ex->start || s_tdt_nf != nframes)) {
        for (int i = 0; i < T; i++)
            for (int tt = 0; tt < nframes; tt++) s_tdt[tt][i] = s_td[i][tt];
        s_tdt_tpl = ex->start; s_tdt_nf = nframes;
    }
    float acc = 0;
    if (T <= 64) {
        for (int i = 0; i < T; i++) {
            int j = s + (int)((long)i * (long)L / (long)T);
            if (j >= s + L) j = s + L - 1;
            acc += s_tdt[j][i];
        }
    } else {
        for (int i = 0; i < T; i++) {
            int j = s + (int)((long)i * (long)L / (long)T);
            if (j >= s + L) j = s + L - 1;
            acc += s_td[i][j];
        }
    }
    return acc / (float)T;
}

float asr_debug_span_score(const asr_model *m, int ex_index, const float *feat,
                           int nframes, int s, int L) {
    if (ex_index < 0 || ex_index >= m->n_exemplars) return 1e30f;
    return dtw_fixed_avg(m, &m->exemplars[ex_index], feat, nframes, s, L);
}

static int decode_dtw(const asr_model *m, const float *feat, int nframes,
                      const float *samples, int64_t nsamp,
                      frame_hyp *out, float *conf_out) {
    if (nframes < 6 || m->n_exemplars <= 0 || !samples || nsamp <= 0) {
        if (conf_out) *conf_out = 0.0f;
        return 0;
    }
    enum { MAXF = 8192, MAXSEG = 512 };
    if (nframes > MAXF) nframes = MAXF;

    /* 1. sample-domain energy envelope on the mel hop grid (one value per 10 ms) */
    static float env[MAXF];
    for (int t = 0; t < nframes; t++) {
        int64_t s0 = (int64_t)t * OSAI_HOP;
        int64_t s1 = s0 + OSAI_FRAME_LEN;
        if (s1 > nsamp) s1 = nsamp;
        double acc = 0;
        for (int64_t i = s0; i < s1; i++) acc += (double)samples[i] * samples[i];
        int64_t len = s1 - s0;
        env[t] = len > 0 ? (float)sqrt(acc / (double)len) : 0.0f;
    }
    float ep = 0;
    for (int t = 0; t < nframes; t++) if (env[t] > ep) ep = env[t];
    if (ep <= 1e-6f) { if (conf_out) *conf_out = 0.0f; return 0; }
    for (int t = 0; t < nframes; t++) env[t] /= ep;

        /* 2. Trim the utterance to the region containing speech. */
    int first = -1, last = -1;
    for (int t = 0; t < nframes; t++) if (env[t] >= 0.06f) { if (first < 0) first = t; last = t; }
    if (first < 0) { if (conf_out) *conf_out = 0.0f; return 0; }
    if (first > 6) first -= 4;
    if (last < nframes - 8) last += 4;
    if (last <= first) { if (conf_out) *conf_out = 0.0f; return 0; }


    /* 3. Segmental DP with a duration prior.
     *
     * The utterance is tiled by words. Placing word x on [t, t+L) costs the DTW
     * distance of that slice to the word's template PLUS a penalty for departing
     * from the template's own duration. The duration prior is what keeps a long
     * word from swallowing several short words (and vice versa) — with templates
     * that come from the same synthesiser, durations are directly comparable. */
    enum { MAXSEGP = 256, NOFF = 12 };
    const int lo_t = first, hi_t = last + 1;
    const float SIL_COST = 1.35f;      /* per frame: worse than a matching word */
    const float DUR_W = 0.45f;

    /* Candidate table: one row per (exemplar, start frame) holding the five
     * locally-best durations and their DTW costs.
     *
     * The scoring loop is template-outer on purpose. Both scorers cache a
     * template-vs-utterance distance matrix, and an inner loop over frames would
     * invalidate that cache on every single step: with 50 templates and 300
     * starts that is 15000 full matrix rebuilds. Iterating templates outermost
     * rebuilds each matrix exactly once. */
    const int nx = m->n_exemplars;
    const int span = hi_t - lo_t;
    if (span <= 0) { if (conf_out) *conf_out = 0.0f; return 0; }
    size_t cells = (size_t)nx * (size_t)span;
    float *cbest = (float *)malloc(cells * sizeof(float) * NOFF);
    int *clen = (int *)malloc(cells * sizeof(int) * NOFF);
    if (!cbest || !clen) { free(cbest); free(clen); if (conf_out) *conf_out = 0.0f; return 0; }
    for (size_t i = 0; i < cells * NOFF; i++) { cbest[i] = 1e30f; clen[i] = 0; }

    for (int x = 0; x < nx; x++) {
        const asr_exemplar *ex = &m->exemplars[x];
        int T = ex->nframes;
        if (T < 4) continue;
        int lo = (int)(0.60f * (float)T) - 1;
        int hi = (int)(1.75f * (float)T) + 3;
        if (lo < 4) lo = 4;
        /* prime both row caches once for this template */
        (void)dtw_fixed_avg(m, ex, feat, nframes, lo_t, T);
        for (int t = lo_t; t < hi_t; t++) {
            /* stage 1: fixed-tempo sweep over candidate durations (cheap) */
            int l1[NOFF]; float c1[NOFF];
            for (int k = 0; k < NOFF; k++) { l1[k] = 0; c1[k] = 1e30f; }
            for (int L = lo; L <= hi && t + L <= hi_t; L++) {
                /* Both caches are warmed by dtw_fixed_avg() before this loop:
                 * s_td[i][j] is the template row read forward, s_tdt[j][i] the
                 * same matrix transposed for sequential access. Long templates
                 * (over 64 frames — most multi-syllable English words) have no
                 * transposed copy, so they read the forward one. Skipping them,
                 * as an earlier version did, made every such word
                 * unrecognisable: "recording" (65 frames) could never win a
                 * span, and the decoder emitted whichever short words scored
                 * best in its place. */
                float acc = 0;
                if (T <= 64) {
                    for (int i = 0; i < T; i++) {
                        int j = t + (int)((long)i * (long)L / (long)T);
                        if (j >= t + L) j = t + L - 1;
                        acc += s_tdt[j][i];
                    }
                } else {
                    for (int i = 0; i < T; i++) {
                        int j = t + (int)((long)i * (long)L / (long)T);
                        if (j >= t + L) j = t + L - 1;
                        acc += s_td[i][j];
                    }
                }
                float avg = acc / (float)T;
                float ratio = (float)L / (float)T;
                float c = avg + DUR_W * fabsf(logf(ratio));
                for (int k = 0; k < NOFF; k++) {
                    if (c < c1[k]) {
                        for (int q = NOFF - 1; q > k; q--) { c1[q] = c1[q-1]; l1[q] = l1[q-1]; }
                        c1[k] = c; l1[k] = L;
                        break;
                    }
                }
            }
            /* stage 2: exact banded DTW around the most promising durations */
            float *dst = cbest + ((size_t)x * (size_t)span + (size_t)(t - lo_t)) * NOFF;
            int *dln = clen + ((size_t)x * (size_t)span + (size_t)(t - lo_t)) * NOFF;
            for (int k = 0; k < NOFF; k++) {
                if (l1[k] == 0) continue;
                int L = l1[k];
                float sc = dtw_full(m, ex, feat, nframes, t, t + L);
                if (sc >= 1e29f) continue;
                float ratio = (float)L / (float)T;
                dst[k] = sc + DUR_W * fabsf(logf(ratio));
                dln[k] = L;
            }
        }
    }

    /* Frame-ordered segmental DP over the precomputed candidates. */
    static float dp[MAXF + 1];
    static int bword[MAXF + 1], blen[MAXF + 1];
    for (int t = lo_t; t <= hi_t; t++) { dp[t] = 1e30f; bword[t] = -1; blen[t] = 0; }
    dp[lo_t] = 0.0f;
    for (int t = lo_t; t < hi_t; t++) {
        if (dp[t] >= 1e29f) continue;
        /* Skipping frames is allowed but expensive, so edge silence and short
         * inter-word gaps remain representable. */
        for (int L = 1; L <= 12 && t + L <= hi_t; L++) {
            float c = dp[t] + SIL_COST * (float)L;
            if (c < dp[t + L]) { dp[t + L] = c; bword[t + L] = -1; blen[t + L] = L; }
        }
        for (int x = 0; x < nx; x++) {
            const float *src = cbest + ((size_t)x * (size_t)span + (size_t)(t - lo_t)) * NOFF;
            const int *sln = clen + ((size_t)x * (size_t)span + (size_t)(t - lo_t)) * NOFF;
            for (int k = 0; k < NOFF; k++) {
                int L = sln[k];
                if (L <= 0) continue;
                float c = dp[t] + src[k] * (float)L;
                if (c < dp[t + L]) { dp[t + L] = c; bword[t + L] = x; blen[t + L] = L; }
            }
        }
    }
    free(cbest);
    free(clen);
    static node path[MAXSEGP];
    int np = 0;
    for (int t = hi_t; t > lo_t && np < MAXSEGP; ) {
        int L = blen[t];
        if (L <= 0) break;
        if (bword[t] >= 0) {
            path[np].ex = bword[t];
            path[np].token = m->exemplars[bword[t]].token;
            path[np].s0 = t - L;
            path[np].s1 = t;
            np++;
        }
        t -= L;
    }
    int nout = 0;
    float sum = 0;
    for (int i = np - 1; i >= 0 && nout < ASR_MAX_TOKENS; i--) {
        out[nout].tok = path[i].token;
        float p = osa_clampf(0.80f, 0.05f, 0.99f);
        out[nout].prob = p;
        sum += p;
        nout++;
        if (getenv("OSAI_ASR_DEBUG"))
            fprintf(stderr, "    %-10s [%3d..%3d]\n", m->tokens[path[i].token].text,
                    path[i].s0, path[i].s1);
    }
    if (conf_out) *conf_out = nout ? sum / (float)nout : 0.0f;
    return nout;
}

/* Trained-model ("mlp"/"ctc") decode: the network emits one posterior vector per
 * frame; greedy CTC collapse plus an LM rescore produces the hypothesis. This is
 * the path used by models produced by the training script, while the builtin
 * exemplar decoder above serves the offline bootstrap. */
static int decode_trained(asr_model *m, const float *feat, int nframes,
                          frame_hyp *out, float *conf_out) {
    if (!m->net2 || m->n_tokens <= 0) { if (conf_out) *conf_out = 0.0f; return 0; }
    enum { MAXF = 8192 };
    if (nframes > MAXF) nframes = MAXF;
    static float probs[MAXF * 256];
    if (m->n_tokens > 256) { if (conf_out) *conf_out = 0.0f; return 0; }
    for (int t = 0; t < nframes; t++) {
        float *p = probs + (size_t)t * (size_t)m->n_tokens;
        nn_forward(m->net2, feat + (size_t)t * m->feat_dim, p);
        nn_softmax(p, m->n_tokens);
    }
    float conf = 0;
    /* the last output index is reserved as the CTC blank by the training script */
    int n = ctc_decode(probs, nframes, m->n_tokens, m->n_tokens - 1, out, &conf);
    n = lm_rescore(m, out, n, 0.6f);
    if (conf_out) *conf_out = conf;
    return n;
}

static bool run_frames(asr_model *m, const float *samples, int64_t n,
                       double offset_s, asr_segment_result *seg_out, char *text_out,
                       size_t text_cap, float *conf_out, osa_error *err) {
    if (m->n_tokens <= 0 || m->n_exemplars <= 0 || m->feat_dim <= 0) {
        if (err) osa_err_set(err, "no model loaded");
        return false;
    }
    float *feat = NULL; int D = 0;
    int nframes = asr_frame_features(samples, n, &feat, &D);
    if (nframes <= 0 || !feat) {
        free(feat);
        if (err) osa_err_set(err, "audio too short");
        return false;
    }
    frame_hyp hyps[ASR_MAX_TOKENS];
    float conf = 0;
    int nh;
    if (m->net2 && !osa_starts(m->backend, "exemplar"))
        nh = decode_trained(m, feat, nframes, hyps, &conf);
    else
        nh = decode_dtw(m, feat, nframes, samples, n, hyps, &conf);
    free(feat);

    size_t o = 0;
    text_out[0] = 0;
    if (seg_out) memset(seg_out, 0, sizeof *seg_out);
    for (int i = 0; i < nh; i++) {
        const char *w = m->tokens[hyps[i].tok].text;
        size_t wl = strlen(w);
        if (o + wl + 2 < text_cap) {
            if (o) text_out[o++] = ' ';
            memcpy(text_out + o, w, wl);
            o += wl;
            text_out[o] = 0;
        }
    }
    if (conf_out) *conf_out = conf;
    if (getenv("OSAI_ASR_DEBUG"))
        fprintf(stderr, "[asr] frames=%d tokens=%d words=%d conf=%.2f text=[%s]\n",
                nframes, m->n_tokens, nh, conf, text_out);
    if (seg_out && nh > 0) {
        seg_out->start = offset_s;
        seg_out->end = offset_s + (double)n / OSAI_SR;
        osa_strlcpy(seg_out->text, text_out, sizeof seg_out->text);
        seg_out->confidence = conf;
    }
    return true;
}

bool asr_transcribe_samples(asr_model *m, const float *samples, int64_t n,
                            const char *force_lang, asr_result *out, osa_error *err) {
    if (!m || !samples || n <= 0 || !out) {
        osa_err_set(err, "invalid input");
        return false;
    }
    memset(out, 0, sizeof *out);
    int64_t t0 = osa_now_ms();
    if (m->wsp) {
        /* whisper.cpp: the whole clip at once, language forced or detected */
        wsp_result wr;
        char werr[160];
        const char *lang = (force_lang && *force_lang && !osa_strieq(force_lang, "auto"))
                           ? force_lang : NULL;
        if (!wsp_transcribe(m->wsp, samples, n, lang, &wr, werr, sizeof werr)) {
            osa_err_set(err, "%s", werr);
            return false;
        }
        osa_strlcpy(out->text, wr.text, sizeof out->text);
        osa_strlcpy(out->language, wr.language[0] ? wr.language : (lang ? lang : "fa"),
                    sizeof out->language);
        out->confidence = wr.confidence;
        out->duration_s = (double)n / OSAI_SR;
        out->proc_ms = (double)(osa_now_ms() - t0);
        out->model_name = m->name;
        out->used_fixture = false;
        osa_err_ok(err);
        return true;
    }
    const char *lang = (force_lang && *force_lang && !osa_strieq(force_lang, "auto"))
                       ? force_lang : asr_detect_language(samples, n);
    osa_strlcpy(out->language, lang, sizeof out->language);
    /* A lexicon covers exactly one language. Asking an English clip of the
     * Persian lexicon does not fail, it returns the nearest Persian words —
     * confident nonsense. Say so instead: drop the guess and report a failed
     * attempt, so callers (evaluation, the transcribe screen) show an honest
     * outcome rather than a wrong one. A model with an explicit multi-language
     * marker ("multi", "fa;en", "*") is left alone. */
    const char *cover = m->language[0] ? m->language : "*";
    if (!osa_streq(cover, "multi") && !strstr(cover, ";") && !osa_streq(cover, "*") &&
        !osa_starts(cover, lang)) {
        out->text[0] = 0;
        osa_err_set(err, "this model's lexicon covers '%s', not '%s'", cover, lang);
        return false;
    }
    osa_audio tmp;
    osa_audio_init(&tmp);
    tmp.samples = (float *)samples;
    tmp.n = n;
    osa_segment segs[256];
    int ns = osa_audio_vad(&tmp, segs, 256);
    free(out->segments);
    out->segments = (asr_segment_result *)calloc((size_t)(ns > 0 ? ns : 1), sizeof(asr_segment_result));
    out->n_segments = 0;
    char full[8192] = {0};
    size_t o = 0;
    double conf_sum = 0;
    if (ns <= 0) {
        asr_segment_result seg;
        char text[1024];
        float c = 0;
        if (!run_frames(m, samples, n, 0.0, &seg, text, sizeof text, &c, err)) {
            free(out->segments);
            out->segments = NULL;
            return false;
        }
        osa_strlcpy(full, text, sizeof full);
        out->segments[0] = seg;
        out->n_segments = 1;
        conf_sum = c;
    } else {
        for (int i = 0; i < ns; i++) {
            int64_t s0 = (int64_t)(segs[i].start * OSAI_SR);
            int64_t s1 = (int64_t)(segs[i].end * OSAI_SR);
            if (s0 < 0) s0 = 0;
            if (s1 > n) s1 = n;
            if (s1 - s0 < OSAI_SR / 5) continue;
            asr_segment_result seg;
            char text[1024];
            float c = 0;
            if (!run_frames(m, samples + s0, s1 - s0, segs[i].start, &seg, text, sizeof text, &c, err))
                continue;
            seg.start = segs[i].start;
            seg.end = segs[i].end;
            osa_strlcpy(seg.text, text, sizeof seg.text);
            seg.confidence = c;
            out->segments[out->n_segments++] = seg;
            conf_sum += c;
            size_t tl = strlen(text);
            if (o + tl + 2 < sizeof full) {
                if (o) full[o++] = ' ';
                memcpy(full + o, text, tl);
                o += tl;
                full[o] = 0;
            }
        }
    }
    if (out->n_segments == 0) {
        /* VAD rejected everything: transcribe the whole clip anyway */
        asr_segment_result seg;
        char text[1024];
        float c = 0;
        if (!run_frames(m, samples, n, 0.0, &seg, text, sizeof text, &c, NULL)) {
            osa_err_set(err, "recognition failed");
            return false;
        }
        osa_strlcpy(full, text, sizeof full);
        out->segments[0] = seg;
        out->n_segments = 1;
        conf_sum = c;
    }
    asr_normalize_text(full, out->language, out->text, sizeof out->text);
    out->confidence = out->n_segments ? (float)(conf_sum / out->n_segments) : 0.0f;
    out->duration_s = (double)n / OSAI_SR;
    out->proc_ms = (double)(osa_now_ms() - t0);
    out->model_name = m->name;
    out->used_fixture = osa_starts(m->base_model, "builtin-fixture");
    osa_err_ok(err);
    return true;
}

bool asr_transcribe_file(asr_model *m, const char *audio_path, const char *force_lang,
                         asr_result *out, osa_error *err) {
    osa_audio a;
    if (!osa_audio_load(audio_path, &a, err)) return false;
    osa_audio_normalize(a.samples, a.n);
    bool ok = asr_transcribe_samples(m, a.samples, a.n, force_lang, out, err);
    osa_audio_free(&a);
    return ok;
}

/* =====================================================================
 *  Streaming
 * ===================================================================== */
struct asr_stream {
    asr_model *model;
    char lang[8];
    float *pending;
    int64_t n_pending, cap_pending;
    char partial[4096];
    float silence_run;
    double t;
};

asr_stream *asr_stream_start(asr_model *m, const char *lang, osa_error *err) {
    if (!m) { osa_err_set(err, "no model"); return NULL; }
    asr_stream *s = (asr_stream *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->model = m;
    osa_strlcpy(s->lang, lang && *lang ? lang : "fa", sizeof s->lang);
    s->cap_pending = OSAI_SR * 30;
    s->pending = (float *)malloc((size_t)s->cap_pending * sizeof(float));
    osa_err_ok(err);
    return s;
}

bool asr_stream_feed(asr_stream *s, const float *samples, int64_t n) {
    if (!s || !samples || n <= 0) return false;
    if (s->n_pending + n > s->cap_pending) {
        int64_t keep = s->cap_pending / 2;
        memmove(s->pending, s->pending + (s->n_pending - keep), (size_t)keep * sizeof(float));
        s->n_pending = keep;
    }
    memcpy(s->pending + s->n_pending, samples, (size_t)n * sizeof(float));
    s->n_pending += n;
    /* decode what we have (chunked, like the phase-6 design) */
    if (s->n_pending >= OSAI_SR / 2) {
        asr_result r;
        if (asr_transcribe_samples(s->model, s->pending, s->n_pending, s->lang, &r, NULL)) {
            osa_strlcpy(s->partial, r.text, sizeof s->partial);
            asr_result_free(&r);
        }
    }
    return true;
}

const char *asr_stream_partial(asr_stream *s) { return s ? s->partial : ""; }

bool asr_stream_finish(asr_stream *s, asr_result *out) {
    if (!s) return false;
    bool ok = asr_transcribe_samples(s->model, s->pending, s->n_pending, s->lang, out, NULL);
    return ok;
}

void asr_stream_free(asr_stream *s) {
    if (!s) return;
    free(s->pending);
    free(s);
}
