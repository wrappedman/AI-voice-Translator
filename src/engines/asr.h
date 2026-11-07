/* asr.h — OpenSpeech AI recognizer.
 * Pipeline: audio -> mel bands -> sliding context vector -> MLP (CTC frame
 * targets) -> CTC greedy collapse -> lexicon/LM rescoring -> text normalization.
 * Models are .osam containers (see docs/MODEL_FORMAT.md) and are replaceable at
 * runtime: the engine never assumes a specific model file. */
#ifndef OSAI_ASR_H
#define OSAI_ASR_H

#include "osai_common.h"
#include "../core/audio.h"

#define ASR_MAX_TOKENS 256
#define ASR_MAX_LEX   4000
#define ASR_MAX_LM    4000
#define ASR_CTX       9      /* mel frames of left+right context */

typedef struct asr_token {
    char text[64];       /* utf-8 token (word or subword) */
    int  frames;         /* typical duration in frames (for alignment) */
} asr_token;

typedef struct asr_exemplar { int token, start, nframes; } asr_exemplar;

typedef struct asr_model {
    char   name[128];
    char   version[32];
    char   language[8];
    char   base_model[128];
    char   license[128];
    char   source_url[512];
    char   sha256[72];
    char   training_note[256];
    int    n_tokens;
    asr_token *tokens;
    /* acoustic backend ---------------------------------------------------
     * Exemplar model: every token owns one or more frame templates; decoding is
     * DTW word spotting over the token templates. User recordings and approved
     * corrections add exemplars, which is how the shipped recognizer improves. */
    char   backend[16];
    float *tmpl_feat;            /* concatenated per-frame features of all templates */
    struct asr_exemplar *exemplars;
    int    n_exemplars;
    int    feat_dim;             /* per-frame feature dimension (mel + delta) */
    /* optional neural acoustic model (user trained, .osam "mlp" backend) */
    void  *net2;                 /* reserved */
    int    n_exemplar_cap;
    /* acoustic net: input = ASR_CTX mel frames x OSAI_MEL_BANDS (+ deltas) */
    void  *net;                  /* nn_model* */
    int    layer_specs[4];
    int    n_specs;
    /* lexicon: token -> pronunciation pieces is not needed for a
       frame-classifier; we keep a confusion table for candidate rescoring */
    struct { char from[64], to[64]; float w; } *confusions;
    int    n_confusions;
    /* n-gram language model: bigram counts over token ids */
    struct { int a, b; float logp; } *bigrams;
    int    n_bigrams;
    float  unigram[ASR_MAX_TOKENS];
    char   path[OSAI_MAX_PATH];
    struct osa_db *db;           /* optional: models are registered in SQLite */
} asr_model;

void asr_init(void);
void asr_shutdown(void);

/* Loads an .osam package. `fallback_mode` builds the deterministic fixture model
 * when the file cannot be read (the app keeps working, clearly flagged). */
asr_model *asr_model_load(const char *path, osa_error *err);
asr_model *asr_model_fixture(const char *lang, osa_error *err);
/* Adds an exemplar (recording of one token) to a model — used by the Training
 * Center and by the "learn from my correction" flow. */
bool asr_model_add_exemplar(asr_model *m, const char *word, const float *samples, int64_t n,
                            osa_error *err);
/* Per-frame feature extraction shared with the trainer. */
float asr_debug_span_score(const asr_model *m, int ex_index, const float *feat,
                               int nframes, int s, int L);
int  asr_frame_features(const float *samples, int64_t n, float **feat, int *dim);

void       asr_model_free(asr_model *m);
bool       asr_model_save(asr_model *m, const char *path, osa_error *err);

/* Model card accessors (never NULL: return "" for an absent field). The UI and
 * the model manager use these instead of touching struct fields, so the struct
 * can grow without breaking them. */
const char *asr_model_id(const asr_model *m);
const char *asr_model_name(const asr_model *m);
const char *asr_model_language(const asr_model *m);
const char *asr_model_backend(const asr_model *m);
const char *asr_model_license(const asr_model *m);
const char *asr_model_source(const asr_model *m);
const char *asr_model_version(const asr_model *m);

/* Batch recognition of a whole file. */
typedef struct asr_segment_result {
    double start, end;
    char   text[1024];
    float  confidence;
} asr_segment_result;

typedef struct asr_result {
    char  text[8192];
    char  language[8];
    float confidence;
    double duration_s, proc_ms;
    int   n_segments;
    asr_segment_result *segments;
    const char *model_name;
    bool  used_fixture;          /* true when no trained model was available */
} asr_result;

bool asr_transcribe_file(asr_model *m, const char *audio_path, const char *force_lang,
                         asr_result *out, osa_error *err);
bool asr_transcribe_samples(asr_model *m, const float *samples, int64_t n,
                            const char *force_lang, asr_result *out, osa_error *err);
void asr_result_free(asr_result *r);

/* Language identification (fa / en / de / fr / es / ar / tr / ru). */
const char *asr_detect_language(const float *samples, int64_t n);

/* text normalization + metrics */
void asr_normalize_text(const char *in, const char *lang, char *out, size_t cap);
double asr_wer(const char *ref, const char *hyp, const char *lang);
double asr_cer(const char *ref, const char *hyp);

/* streaming (live) recognition: feed frames, poll partial hypotheses */
typedef struct asr_stream asr_stream;
asr_stream *asr_stream_start(asr_model *m, const char *lang, osa_error *err);
bool        asr_stream_feed(asr_stream *s, const float *samples, int64_t n);
const char *asr_stream_partial(asr_stream *s);
bool        asr_stream_finish(asr_stream *s, asr_result *out);
void        asr_stream_free(asr_stream *s);

#endif
