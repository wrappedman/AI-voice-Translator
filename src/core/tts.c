#include "tts.h"
#include "audio.h"
#include <math.h>

/* ------------------------------------------------------------------
 *  Phoneme inventory (romanized) with formant targets.
 *  F1..F3 in Hz for voiced segments; `noise` adds frication; `dur` in ms.
 * ------------------------------------------------------------------ */
typedef struct phoneme {
    const char *sym;
    float f1, f2, f3;
    float voice;      /* 0..1 voicing amount */
    float noise;      /* 0..1 frication amount */
    float dur_ms;
    int   noise_hp;   /* high-pass-ish cutoff index for fricatives */
} phoneme;

static const phoneme PH[] = {
    /* vowels (long/short share targets) */
    {"aa", 730, 1090, 2440, 1.0f, 0.0f, 130, 0},
    {"a",  730, 1090, 2440, 1.0f, 0.0f,  95, 0},
    {"e",  530, 1840, 2480, 1.0f, 0.0f,  85, 0},
    {"i",  270, 2290, 3010, 1.0f, 0.0f,  80, 0},
    {"o",  570,  840, 2410, 1.0f, 0.0f,  95, 0},
    {"u",  300,  870, 2240, 1.0f, 0.0f,  85, 0},
    /* stops */
    {"b",  200,  900, 2200, 1.0f, 0.35f, 65, 0},
    {"p",  200,  900, 2200, 0.0f, 0.9f,  60, 1},
    {"d",  300, 1700, 2600, 1.0f, 0.35f, 60, 0},
    {"t",  300, 1700, 2600, 0.0f, 0.9f,  55, 1},
    {"g",  300, 2000, 2600, 1.0f, 0.3f,  60, 0},
    {"k",  300, 2000, 2600, 0.0f, 0.85f, 55, 1},
    {"q",  350, 1200, 2400, 0.6f, 0.25f, 60, 0},
    /* fricatives */
    {"f",  400, 1200, 2400, 0.0f, 1.0f,  80, 2},
    {"v",  400, 1200, 2400, 0.7f, 0.5f,  70, 2},
    {"s",  400, 1800, 2600, 0.0f, 1.0f,  85, 3},
    {"z",  400, 1800, 2600, 0.7f, 0.6f,  75, 3},
    {"sh", 400, 2200, 2800, 0.0f, 1.0f,  90, 2},
    {"zh", 400, 2200, 2800, 0.7f, 0.6f,  80, 2},
    {"kh", 500, 1500, 2500, 0.0f, 1.0f,  85, 2},
    {"gh", 500, 1500, 2500, 0.6f, 0.5f,  75, 2},
    {"h",  500, 1500, 2500, 0.4f, 0.8f,  60, 2},
    {"ch", 400, 1900, 2700, 0.0f, 1.0f,  85, 2},
    {"j",  400, 1900, 2700, 0.7f, 0.6f,  75, 2},
    /* sonorants */
    {"m",  250, 1100, 2200, 1.0f, 0.0f,  70, 0},
    {"n",  250, 1600, 2500, 1.0f, 0.0f,  65, 0},
    {"l",  350, 1300, 2500, 1.0f, 0.0f,  65, 0},
    {"r",  400, 1300, 1600, 1.0f, 0.15f, 55, 0},
    {"y",  300, 2200, 3000, 1.0f, 0.0f,  55, 0},
    {"w",  300,  700, 2200, 1.0f, 0.0f,  55, 0},
    {"th", 400, 1700, 2600, 0.0f, 0.9f,  75, 2},
    {"dh", 400, 1700, 2600, 0.8f, 0.4f,  65, 0},
};
#define PH_N ((int)(sizeof PH / sizeof PH[0]))

static const phoneme *find_ph(const char *sym, int len) {
    for (int i = 0; i < PH_N; i++) {
        if ((int)strlen(PH[i].sym) == len && strncmp(PH[i].sym, sym, (size_t)len) == 0)
            return &PH[i];
    }
    return NULL;
}

/* greedy romanization parse */
static const phoneme *parse_next(const char **p) {
    const char *s = *p;
    if (!*s) return NULL;
    /* two-letter digraphs first */
    static const char *digraphs[] = { "sh", "ch", "kh", "gh", "zh", "th", "dh", "aa" };
    for (int i = 0; i < 8; i++) {
        if (osa_starts(s, digraphs[i])) {
            const phoneme *ph = find_ph(digraphs[i], 2);
            if (ph) { *p = s + 2; return ph; }
        }
    }
    const phoneme *ph = find_ph(s, 1);
    if (ph) { *p = s + 1; return ph; }
    *p = s + 1;
    return NULL;   /* skip unknown character */
}

/* ------------------------------------------------------------------
 *  Cascade resonator (2-pole) and glottal source
 * ------------------------------------------------------------------ */
typedef struct resonator { float y1, y2, a1, a2, gain; } resonator;

static void res_set(resonator *r, float freq, float bw, float sr) {
    float rho = expf(-((float)OSAI_PI) * bw / sr);
    float theta = 2.0f * ((float)OSAI_PI) * freq / sr;
    r->a1 = 2.0f * rho * cosf(theta);
    r->a2 = -rho * rho;
    r->gain = 1.0f - rho;
    r->y1 = r->y2 = 0;
}

static float res_tick(resonator *r, float x) {
    float y = r->gain * x + r->a1 * r->y1 + r->a2 * r->y2;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

/* simple LCG noise */
static float noise_sample(uint32_t *st) {
    uint32_t x = *st;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *st = x;
    return (float)((int32_t)x) / 2147483648.0f;
}

/* Per-phoneme level equalisation (see synth_phoneme). Off for the
 * lexicon-driven pronunciation, which already has natural balance. */
static bool synth_equalise = false;

typedef struct synth {
    int     sr;
    float  *out;
    int64_t n, cap;
    float   f0, phase;
    resonator r1, r2, r3, hp;
    uint32_t rng;
    float   prev_f1, prev_f2, prev_f3;
    float   noise_gain;         /* leaky noise shaping for fricatives */
} synth;

static synth *synth_new(int sr) {
    synth *s = (synth *)calloc(1, sizeof *s);
    s->sr = sr;
    s->cap = sr * 4;
    s->out = (float *)calloc((size_t)s->cap, sizeof(float));
    s->f0 = 118.0f;
    s->phase = 0;
    s->rng = 0x1234567u;
    s->prev_f1 = 500; s->prev_f2 = 1500; s->prev_f3 = 2500;
    res_set(&s->r1, 500, 90, (float)sr);
    res_set(&s->r2, 1500, 110, (float)sr);
    res_set(&s->r3, 2500, 160, (float)sr);
    res_set(&s->hp, 3000, 1500, (float)sr);
    return s;
}

static void synth_free(synth *s) { if (s) { free(s->out); free(s); } }

static void synth_push(synth *s, float v) {
    if (s->n >= s->cap) {
        s->cap *= 2;
        s->out = (float *)realloc(s->out, (size_t)s->cap * sizeof(float));
    }
    s->out[s->n++] = v;
}

static void synth_phoneme(synth *s, const phoneme *ph, float stretch) {
    int nsamp = (int)(ph->dur_ms * stretch * (float)s->sr / 1000.0f);
    if (nsamp < 8) nsamp = 8;
    const int64_t seg_start = s->n;
    /* glide formants from the previous target */
    float f1 = s->prev_f1, f2 = s->prev_f2, f3 = s->prev_f3;
    for (int i = 0; i < nsamp; i++) {
        float t = (float)i / (float)nsamp;
        /* ease-in-out glide (first 45% of the segment) */
        float g = t < 0.45f ? t / 0.45f : 1.0f;
        g = g * g * (3.0f - 2.0f * g);
        float cf1 = f1 + (ph->f1 - f1) * g;
        float cf2 = f2 + (ph->f2 - f2) * g;
        float cf3 = f3 + (ph->f3 - f3) * g;
        if ((i & 31) == 0) {
            res_set(&s->r1, cf1, 80.0f, (float)s->sr);
            res_set(&s->r2, cf2, 100.0f, (float)s->sr);
            res_set(&s->r3, cf3, 150.0f, (float)s->sr);
            res_set(&s->hp, ph->noise_hp == 3 ? 4200.0f : (ph->noise_hp == 2 ? 2600.0f : 1800.0f),
                    1500.0f, (float)s->sr);
        } else {
            s->r1.a1 = s->r1.a1;
        }
        /* glottal pulse train with slight jitter + intonation */
        float f0 = s->f0 * (1.0f + 0.02f * sinf((float)i * 0.004f));
        s->phase += f0 / (float)s->sr;
        if (s->phase >= 1.0f) s->phase -= 1.0f;
        float pulse = s->phase < 0.06f ? (1.0f - s->phase / 0.06f) : -0.06f;
        float voiced = pulse * ph->voice;
        float fric = noise_sample(&s->rng) * ph->noise * 0.6f;
        fric = res_tick(&s->hp, fric);
        float src = voiced + fric;
        float y = res_tick(&s->r1, src);
        y = res_tick(&s->r2, y);
        y = res_tick(&s->r3, y);
        /* amplitude envelope: attack 15%, release 25% */
        float env = 1.0f;
        if (t < 0.15f) env = t / 0.15f;
        else if (t > 0.75f) env = (1.0f - t) / 0.25f;
        synth_push(s, y * env * 0.9f);
    }
    s->prev_f1 = ph->f1; s->prev_f2 = ph->f2; s->prev_f3 = ph->f3;

    /* Equalise this phoneme against its neighbours.
     *
     * A two-pole cascade is extremely uneven in level: a vowel whose first
     * formant is low resonates far more than a high one, so a spelled word came
     * out with two loud letters and three inaudible ones. Normalising the clip
     * as a whole then made the quiet letters disappear. Correcting each phoneme
     * towards the same RMS, with the correction bounded to ±14 dB, keeps the
     * natural relative loudness of vowels and consonants while making every
     * letter audible. */
    if (synth_equalise) {
        double acc = 0.0;
        int64_t n = s->n - seg_start;
        if (n > 0) {
            for (int64_t i = seg_start; i < s->n; i++) acc += (double)s->out[i] * (double)s->out[i];
            float rms = (float)sqrt(acc / (double)n);
            if (rms > 1e-6f) {
                float g = 0.05f / rms;
                if (g < 0.20f) g = 0.20f;
                if (g > 5.0f) g = 5.0f;
                for (int64_t i = seg_start; i < s->n; i++) s->out[i] *= g;
            }
        }
    }
}

static void synth_silence(synth *s, int ms) {
    int n = ms * s->sr / 1000;
    for (int i = 0; i < n; i++) synth_push(s, 0.0f);
}

float *tts_speak_code(const char *code, int64_t *n_out) {
    if (n_out) *n_out = 0;
    if (!code || !*code) return NULL;
    synth *s = synth_new(OSAI_SR);
    const char *p = code;
    while (*p) {
        const phoneme *ph = parse_next(&p);
        if (ph) synth_phoneme(s, ph, 1.0f);
    }
    synth_silence(s, 60);
    /* normalize to -16 dBFS peak */
    float peak = 0;
    for (int64_t i = 0; i < s->n; i++) if (fabsf(s->out[i]) > peak) peak = fabsf(s->out[i]);
    if (peak > 1e-6f) {
        float g = 0.16f / peak;
        for (int64_t i = 0; i < s->n; i++) s->out[i] *= g;
    }
    float *out = s->out;
    int64_t n = s->n;
    s->out = NULL;
    synth_free(s);
    if (n_out) *n_out = n;
    return out;
}

/* ------------- word lookup (shares the ASR lexicon) ------------- */
bool tts_has_word(const char *word, const char *lang) {
    return tts_code_for_word(word, lang) != NULL;
}

const char *tts_code_for_word(const char *word, const char *lang) {
    int n = 0;
    const tts_word_pair *T = osai_lexicon(lang, &n);
    for (int i = 0; i < n; i++) if (osa_streq(T[i].w, word)) return T[i].code;
    return NULL;
}

/* UTF-8 aware word splitter. Plain `strtok` must not be used with Persian
 * punctuation: the bytes of "؟" (D8 9F) and "،" (D8 8C) include 0xD8, which is
 * the lead byte of most Persian letters, so strtok would shred every word. */
static int tts_is_sep(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
           cp == '.' || cp == ',' || cp == '!' || cp == '?' || cp == ';' ||
           cp == ':' || cp == '\'' || cp == '"' || cp == 0x061F || cp == 0x060C;
}

/* Decode one UTF-8 scalar, advancing *p. Invalid bytes are consumed singly. */
static uint32_t tts_utf8_next(const char **p) {
    const unsigned char *u = (const unsigned char *)*p;
    uint32_t cp = u[0];
    int len = 1;
    if (u[0] >= 0xC0 && u[0] < 0xE0 && u[1]) { cp = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F); len = 2; }
    else if (u[0] >= 0xE0 && u[0] < 0xF0 && u[1] && u[2]) {
        cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F); len = 3;
    } else if (u[0] >= 0xF0 && u[0] < 0xF8 && u[1] && u[2] && u[3]) {
        cp = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) |
             ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F); len = 4;
    }
    *p += len;
    return cp;
}

/* Iterator over whitespace/punctuation separated words (in place, UTF-8 safe). */
static const char *tts_next_word(const char **cursor) {
    const char *p = *cursor;
    while (*p) {
        const char *save = p;
        if (!tts_is_sep(tts_utf8_next(&p))) { p = save; break; }
    }
    if (!*p) { *cursor = p; return NULL; }
    const char *start = p;
    while (*p) {
        const char *save = p;
        if (tts_is_sep(tts_utf8_next(&p))) { p = save; break; }
    }
    *cursor = p;
    return start;
}

/* A word the builtin lexicon does not know.
 *
 * The old fallback pronounced the first four *characters* and stopped, which is
 * what made the "listen" button sound like growling: "hello" became "h-e-l-l"
 * and every Persian word became a run of bare consonants, because Persian is
 * written without short vowels. Spelling letter-by-letter can never be pleasant,
 * but it can at least be recognisable, so:
 *
 *   - Latin words are spelled with the English letter *names* ("h" = "aitch",
 *     "w" = "double-u"), which is how the letters are actually said;
 *   - every consonant gets a short support vowel, so two letters never collide
 *     into one buzz;
 *   - a word over four letters is spelled in groups ("hel-lo"), with a falling
 *     pitch, instead of being cut off after four letters;
 *   - punctuation inserts a pause instead of being skipped silently.
 *
 * When Windows voices are present this path is never reached at all: it is the
 * fallback for a machine that has none. */
typedef struct letter_name { const char *sym, *spoken; } letter_name;
static const letter_name LETTERS[] = {
    {"a", "ay"},   {"b", "bee"},  {"c", "see"},  {"d", "dee"}, {"e", "ee"},
    {"f", "ef"},   {"g", "jee"},  {"h", "aitch"},{"i", "ay"},  {"j", "jay"},
    {"k", "kay"},  {"l", "el"},   {"m", "em"},   {"n", "en"},  {"o", "oh"},
    {"p", "pee"},  {"q", "kyu"},  {"r", "ar"},   {"s", "es"},  {"t", "tee"},
    {"u", "yu"},   {"v", "vee"},  {"w", "dabl-yu"}, {"x", "eks"}, {"y", "wy"},
    {"z", "zee"},
};

static const char *letter_name_for(char c) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    for (int i = 0; i < (int)(sizeof LETTERS / sizeof LETTERS[0]); i++)
        if (LETTERS[i].sym[0] == c && LETTERS[i].sym[1] == 0) return LETTERS[i].spoken;
    return NULL;
}

/* Says one romanised syllable: known digraphs first, then single phonemes. */
static void say_code(synth *s, const char *code, float stretch) {
    if (!code) return;
    const char *p = code;
    while (*p) {
        const phoneme *ph = parse_next(&p);
        if (ph) synth_phoneme(s, ph, stretch);
    }
}

static void spell_word(synth *s, const char *word) {
    /* ASCII letters only: the letter names are English. */
    char cleaned[64];
    int n = 0;
    for (const char *c = word; *c && n < (int)sizeof cleaned - 1; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')) cleaned[n++] = *c;
    }
    cleaned[n] = 0;
    if (n == 0) {
        /* Persian (or any non-Latin) word with no lexicon entry. Saying nothing
         * is worse than saying a neutral placeholder, and inventing phonemes
         * from raw UTF-8 bytes is worse than both: those bytes include 0xD8 and
         * friends, which parse_next() would turn into noise. */
        synth_silence(s, 90);
        return;
    }
    synth_equalise = true;
    /* Spell in groups of four letters: a whole long word spelled out is
     * unlistenable, and groups give the ear a place to breathe. */
    for (int i = 0; i < n; i += 4) {
        int end = i + 4 < n ? i + 4 : n;
        for (int j = i; j < end; j++) {
            const char *name = letter_name_for(cleaned[j]);
            if (name) say_code(s, name, 0.85f);
            else { char ch[2] = { cleaned[j], 0 }; say_code(s, ch, 0.8f); }
            synth_silence(s, 18);
        }
        synth_silence(s, i + 4 < n ? 45 : 30);
    }
}

float *tts_speak(const char *text, const char *lang, int64_t *n_out) {
    if (n_out) *n_out = 0;
    if (!text) return NULL;
    synth *s = synth_new(OSAI_SR);
    char buf[8192];
    osa_strlcpy(buf, text, sizeof buf);
    const char *cursor = buf;
    for (;;) {
        const char *wstart = tts_next_word(&cursor);
        if (!wstart) break;
        size_t wlen = (size_t)(cursor - wstart);
        if (wlen == 0 || wlen >= 128) continue;
        char word[128];
        memcpy(word, wstart, wlen);
        word[wlen] = 0;
        const char *code = tts_code_for_word(word, lang);
        if (code) {
            const char *p = code;
            while (*p) {
                const phoneme *ph = parse_next(&p);
                if (ph) synth_phoneme(s, ph, 1.0f);
            }
            synth_silence(s, 70);
        } else {
            spell_word(s, word);
        }
    }
    /* Nothing was spoken: return no audio at all rather than a pad of silence,
     * so callers can treat "empty input" the same as "synthesis failed". */
    if (s->n == 0) {
        synth_free(s);
        return NULL;
    }
    synth_silence(s, 80);
    /* Level. Normalising on the peak alone made every clip almost inaudible:
     * the formant cascade is quiet by nature, so the single loudest click set
     * the gain and the rest of the sentence sat at a few percent — which is what
     * "only a faint growl" sounds like. Scale to a target RMS (speech is
     * around -20 dBFS RMS) and then only clamp if the peak would clip. */
    double acc = 0.0;
    float peak = 0;
    for (int64_t i = 0; i < s->n; i++) {
        float a = fabsf(s->out[i]);
        acc += (double)s->out[i] * (double)s->out[i];
        if (a > peak) peak = a;
    }
    float rms = s->n ? (float)sqrt(acc / (double)s->n) : 0.0f;
    if (rms > 1e-6f) {
        float g = 0.10f / rms;                   /* ~-20 dBFS */
        if (peak * g > 0.95f) g = 0.95f / peak;  /* never clip */
        for (int64_t i = 0; i < s->n; i++) s->out[i] *= g;
    }
    float *out = s->out;
    int64_t n = s->n;
    s->out = NULL;
    synth_free(s);
    if (n_out) *n_out = n;
    return out;
}

bool tts_speak_to_wav(const char *text, const char *lang, const char *path) {
    int64_t n = 0;
    float *s = tts_speak(text, lang, &n);
    if (!s || n <= 0) { free(s); return false; }
    bool ok = osa_audio_save_wav(path, s, n, OSAI_SR);
    free(s);
    return ok;
}
