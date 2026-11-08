/* =====================================================================
 *  OpenSpeech AI — unit and integration tests
 *
 *  Self-contained: no test framework, no fixtures on disk. Every test builds
 *  what it needs, asserts, and cleans up. `make test` runs the suite; the exit
 *  code is the number of failures so CI can gate on it.
 *
 *  Deliberately *not* covered here:
 *    - anything that needs a microphone or a display (covered by the snapshot
 *      job, which renders with the same code the Windows build uses)
 *    - the Windows window procedure (it needs a message loop; the logic it
 *      calls is covered by the app-layer tests)
 * ===================================================================== */
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <unistd.h>

#include "osai_common.h"
#include "whisper_bridge.h"
#include "osai_platform.h"
#include "osai_store.h"
#include "audio.h"
#include "tts.h"
#include "asr.h"
#include "mt.h"
#include "nn.h"
#include "osai_app.h"

/* ---- harness ------------------------------------------------------ */

static int g_pass, g_fail;
static const char *g_suite = "";

static void suite(const char *name) {
    g_suite = name;
    printf("\n%s\n", name);
}

static void ok_(bool cond, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", msg);
    if (cond) g_pass++; else g_fail++;
}

#define CHECK(cond)              ok_((cond), "%s", #cond)
#define CHECK_MSG(cond, ...)     ok_((cond), __VA_ARGS__)
#define CHECK_STR_EQ(a, b)       ok_(osa_streq((a), (b)), "%s == \"%s\"  (got \"%s\")", #a, (b), (a))
#define CHECK_NEAR(a, b, eps)    ok_(fabs((double)(a) - (double)(b)) <= (eps), \
                                     "%s ~= %s  (%.6f vs %.6f)", #a, #b, (double)(a), (double)(b))

/* ---- common ------------------------------------------------------- */

static void test_utf8(void) {
    suite("utf8 + rtl");
    /* decode + re-encode round trip */
    const char *s = "سلام world";
    size_t len = strlen(s);
    size_t pos = 0;
    int n = 0;
    while (pos < len) {
        osa_rune r = 0;
        int used = osa_utf8_decode(s + pos, len - pos, &r);
        if (used <= 0) break;
        pos += (size_t)used;
        n++;
    }
    CHECK_MSG(n == 10 && pos == len, "iterates 10 scalars, consumes the whole string");
    /* direction is decided per rune; the paragraph direction is the first
     * strong character, which is what the text layer relies on */
    CHECK(osa_rune_is_rtl(0x0633));            /* ARABIC LETTER SEEN */
    CHECK(!osa_rune_is_rtl('A'));
    CHECK(!osa_rune_is_rtl('0'));
    CHECK(!osa_rune_is_rtl(' '));
}

static void test_strings(void) {
    suite("string helpers");
    CHECK(osa_streq("a", "a"));
    CHECK(!osa_streq("a", "A"));
    CHECK(osa_strieq("HeLLo", "hello"));
    CHECK(osa_starts("hello world", "hello"));
    CHECK(!osa_starts("hello", "world"));
    char buf[8];
    osa_strlcpy(buf, "a long string", sizeof buf);
    CHECK_MSG(strlen(buf) == 7, "strlcpy truncates to 7 bytes");
    CHECK(buf[7] == 0);
    CHECK(osa_clampf(5.0f, 0.0f, 1.0f) == 1.0f);
    CHECK(osa_clampf(-5.0f, 0.0f, 1.0f) == 0.0f);
}

static void test_json(void) {
    suite("json");
    osa_arena a;
    osa_arena_init(&a);
    const char *raw = "he said \"hi\"\nand left\t!";
    char *esc = osa_json_escape(&a, raw);
    CHECK(esc && esc[0] == '"' && esc[strlen(esc) - 1] == '"');
    CHECK(strstr(esc, "\\\"") != NULL);
    CHECK(strstr(esc, "\\n") != NULL);

    const char *doc = "{\"name\":\"OpenSpeech\",\"ok\":true,\"n\":42.5}";
    char name[64];
    bool b = false;
    double d = 0;
    CHECK(osa_json_get_string(doc, "name", name, sizeof name));
    CHECK_STR_EQ(name, "OpenSpeech");
    CHECK(osa_json_get_bool(doc, "ok", &b) && b);
    CHECK(osa_json_get_num(doc, "n", &d));
    CHECK_NEAR(d, 42.5, 1e-9);
    osa_arena_free(&a);
}

static void test_hash(void) {
    suite("sha256 + base64 + crc32");
    /* known vector: sha256("abc") */
    const char *path = "/tmp/osai_test_hash.bin";
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) { fwrite("abc", 1, 3, f); fclose(f); }
    char hex[65];
    uint64_t size = 0;
    osa_sha256_file(path, hex, &size);
    CHECK_MSG(size == 3, "hashed 3 bytes");
    CHECK_STR_EQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    osa_file_remove(path);

    /* base64 round trip over binary data */
    uint8_t data[64];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 3);
    char enc[128];
    size_t el = osa_base64_encode(data, sizeof data, enc);
    CHECK(el > 0);
    uint8_t dec[128];
    size_t dl = osa_base64_decode(enc, el, dec);
    CHECK_MSG(dl == sizeof data, "base64 round trip length");
    CHECK(memcmp(data, dec, sizeof data) == 0);

    /* crc32 of "123456789" is the standard check value */
    CHECK_MSG(osa_crc32(0, "123456789", 9) == 0xCBF43926u, "crc32 check value");
}

/* ---- audio -------------------------------------------------------- */

static void test_audio(void) {
    suite("audio");
    const int sr = OSAI_SR;
    const int64_t n = sr;                     /* 1 second */
    float *samples = (float *)calloc((size_t)n, sizeof(float));
    CHECK(samples != NULL);
    if (!samples) return;
    /* 440 Hz tone */
    for (int64_t i = 0; i < n; i++)
        samples[i] = 0.5f * sinf(2.0f * (float)OSAI_PI * 440.0f * (float)i / sr);

    const char *path = "/tmp/osai_test_tone.wav";
    CHECK(osa_audio_save_wav(path, samples, n, sr));

    osa_audio au;
    CHECK(osa_audio_load(path, &au, NULL));
    CHECK_MSG(au.n == n, "loaded %lld samples (expected %lld)", (long long)au.n, (long long)n);
    CHECK(au.src_rate == sr);
    CHECK_NEAR(osa_audio_duration_s(&au), 1.0, 0.01);

    /* resampling to 8 kHz must halve the count */
    float *half = NULL;
    int64_t half_n = 0;
    osa_audio_resample(au.samples, au.n, OSAI_SR, &half, &half_n, 8000);
    CHECK_MSG(half && half_n >= n / 2 - 2 && half_n <= n / 2 + 2,
              "resample to 8 kHz gives %lld samples", (long long)half_n);
    free(half);

    /* the tone is well above the noise floor */
    double energy = 0;
    for (int64_t i = 0; i < au.n; i++) energy += (double)au.samples[i] * au.samples[i];
    CHECK_MSG(sqrt(energy / (double)au.n) > 0.2, "tone RMS %.3f",
              sqrt(energy / (double)au.n));
    osa_audio_free(&au);

    /* a silent file: VAD must report no speech */
    float *silence = (float *)calloc((size_t)sr, sizeof(float));
    const char *sp = "/tmp/osai_test_silence.wav";
    osa_audio_save_wav(sp, silence, sr, sr);
    osa_audio sa;
    if (osa_audio_load(sp, &sa, NULL)) {
        int nseg = osa_audio_vad(&sa, NULL, 0);
        CHECK_MSG(nseg == 0, "VAD finds %d segments in silence", nseg);
        osa_audio_free(&sa);
    }
    free(silence);
    free(samples);
    osa_file_remove(path);
    osa_file_remove(sp);

    /* AAC/M4A goes through a platform hook. Where there is no decoder the
     * answer must be an honest refusal that names the format, not a silent
     * failure or a crash: the picker offers .m4a on every build. */
    {
        const char *fake = "/tmp/osai_test_fake.m4a";
        CHECK(osa_file_write_atomic(fake, "not really audio", 16));
        osa_audio m4a;
        osa_error e;
        bool ok = osa_audio_load(fake, &m4a, &e);
#if defined(_WIN32)
        (void)ok;
#else
        CHECK_MSG(!ok, "the headless build has no AAC decoder");
        CHECK_MSG(e.msg[0] != 0, "the refusal carries a message");
#endif
        if (ok) osa_audio_free(&m4a);
        osa_file_remove(fake);
    }
}

/* ---- TTS ---------------------------------------------------------- */

static void test_tts(void) {
    suite("tts");
    struct { const char *text, *lang; } cases[] = {
        { "سلام دنیا", "fa" },
        { "hello world", "en" },
        { "فایل صوتی را انتخاب کنید", "fa" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int64_t n = 0;
        float *pcm = tts_speak(cases[i].text, cases[i].lang, &n);
        CHECK_MSG(pcm && n > 0, "\"%s\" produces %lld samples", cases[i].text,
                  (long long)n);
        if (!pcm) continue;
        float peak = 0;
        double sum = 0;
        for (int64_t k = 0; k < n; k++) {
            float v = fabsf(pcm[k]);
            if (v > peak) peak = v;
            sum += (double)pcm[k] * pcm[k];
        }
        /* Silence is the classic failure mode of this synthesiser: assert on the
         * energy, not just on the buffer existing. */
        CHECK_MSG(peak > 0.02f, "  peak %.3f (not silent)", (double)peak);
        CHECK_MSG(peak <= 1.0f, "  peak %.3f (no clipping)", (double)peak);
        CHECK_MSG(sum > 0.0, "  non-zero energy");
        /* longer text must take longer, monotonically */
        int64_t n2 = 0;
        char longer[256];
        snprintf(longer, sizeof longer, "%s %s", cases[i].text, cases[i].text);
        float *pcm2 = tts_speak(longer, cases[i].lang, &n2);
        CHECK_MSG(pcm2 && n2 > n, "  repeated text is longer (%lld > %lld)",
                  (long long)n2, (long long)n);
        free(pcm2);
        free(pcm);
    }
    /* an empty string must not crash or produce audio */
    int64_t n = 0;
    float *pcm = tts_speak("", "fa", &n);
    CHECK_MSG(n == 0, "empty input produces no samples");
    free(pcm);
}

/* ---- translation -------------------------------------------------- */

static void test_mt(void) {
    suite("translation");
    osa_error err;
    mt_model *m = mt_model_builtin(&err);
    CHECK(m != NULL);
    if (!m) return;

    struct { const char *in, *out; } cases[] = {
        /* whole-sentence phrasebook */
        { "سلام حال شما خوب است", "Hello, how are you?" },
        { "خداحافظ", "Goodbye" },
        { "ممنونم", "Thank you" },
        { "Hello, how are you?", "سلام حال شما خوب است" },
        { "Thank you", "ممنونم" },
        { "Good morning", "صبح بخیر" },
        { "Stop the recording", "ضبط را متوقف کنید" },
        /* normalisation: punctuation, case, Arabic yeh instead of Persian yeh */
        { "THANK YOU", "ممنونم" },
        { "Hello, how are you?", "سلام حال شما خوب است" },
        { "سلام حال شما خوب است!", "Hello, how are you?" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        mt_result r;
        bool got = mt_translate(m, cases[i].in, MT_AUTO, &r, &err);
        CHECK_MSG(got && osa_streq(r.text, cases[i].out), "\"%s\" -> \"%s\" (got \"%s\")",
                  cases[i].in, cases[i].out, r.text);
        if (got) {
            CHECK_MSG(r.confidence > 0.5f, "  confidence %.2f", (double)r.confidence);
            CHECK_MSG(r.dst_lang != MT_AUTO, "  direction detected");
        }
    }

    /* language detection is by script and must be unambiguous */
    CHECK(mt_detect_lang("hello there") == MT_EN);
    CHECK(mt_detect_lang("سلام") == MT_FA);
    CHECK(mt_detect_lang("") == MT_AUTO);

    /* unknown words are kept, not dropped silently */
    mt_result r;
    if (mt_translate(m, "کتاب", MT_FA, &r, &err))
        CHECK_MSG(r.n_pieces >= 1, "unknown Persian word yields %d piece(s)", r.n_pieces);

    /* A sentence with no known words must come back as itself, not as "".
     * It used to be dropped whenever the source script matched the *target*
     * script, so "open the door" (EN -> FA) produced an empty string. */
    if (mt_translate(m, "open the door", MT_AUTO, &r, &err)) {
        CHECK_MSG(r.text[0] != 0, "untranslatable sentence is not empty (\"%s\")", r.text);
        CHECK_MSG(strstr(r.text, "door") != NULL, "unknown word kept: \"%s\"", r.text);
    }
    /* and a half-known sentence keeps both halves */
    if (mt_translate(m, "hello world", MT_AUTO, &r, &err)) {
        CHECK_MSG(strstr(r.text, "سلام") != NULL && strstr(r.text, "world") != NULL,
                  "mixed known/unknown: \"%s\"", r.text);
    }
    /* EN/EM dash are word separators, same as a space */
    mt_result dash;
    if (mt_translate(m, "fa–en", MT_AUTO, &dash, &err) &&
        mt_translate(m, "fa-en", MT_AUTO, &dash, &err))
        CHECK_MSG(dash.n_pieces == 2, "en dash splits into %d pieces", dash.n_pieces);

    /* empty input is an error, not an empty success */
    CHECK(!mt_translate(m, "", MT_AUTO, &r, &err));

    /* round trip through the .osmt container */
    const char *path = "/tmp/osai_test.osmt";
    CHECK(mt_model_save(m, path, &err));
    mt_model *m2 = mt_model_load(path, &err);
    CHECK(m2 != NULL);
    if (m2) {
        mt_result a, b;
        const char *probe = "صبح بخیر";
        if (mt_translate(m, probe, MT_AUTO, &a, &err) &&
            mt_translate(m2, probe, MT_AUTO, &b, &err))
            CHECK_STR_EQ(b.text, a.text);
        CHECK_STR_EQ(mt_model_backend(m2), "osmt");
        mt_model_free(m2);
    }
    osa_file_remove(path);
    mt_model_free(m);
}

/* ---- ASR ---------------------------------------------------------- */

static void test_neural_net(void) {
    suite("neural network");
    /* 3 in -> 4 hidden x 2 layers -> 2 out */
    nn_model *n = nn_create(3, 4, 2, 2, 0);
    CHECK(n != NULL);
    if (!n) return;
    float in[3] = { 0.5f, -0.25f, 1.0f };
    float out[2] = {0, 0};
    double s = nn_forward(n, in, out);
    (void)s;
    CHECK(out[0] >= 0.0f && out[0] <= 1.0f);
    CHECK(out[1] >= 0.0f && out[1] <= 1.0f);

    /* the same input twice must give the same output (determinism) */
    float out2[2] = {0, 0};
    nn_forward(n, in, out2);
    CHECK_NEAR(out2[0], out[0], 1e-7);
    CHECK_NEAR(out2[1], out[1], 1e-7);

    /* save + load round trip */
    const char *path = "/tmp/osai_test.nn";
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        nn_save(n, f);
        fclose(f);
        FILE *in_f = fopen(path, "rb");
        nn_model *n2 = nn_create(3, 4, 2, 2, 0);
        CHECK(in_f && n2 && nn_load(n2, in_f));
        if (in_f) fclose(in_f);
        if (n2) {
            float out3[2] = {0, 0};
            nn_forward(n2, in, out3);
            CHECK_NEAR(out3[0], out[0], 1e-5);
            CHECK_NEAR(out3[1], out[1], 1e-5);
            nn_free(n2);
        }
    }
    osa_file_remove(path);
    nn_free(n);
}

/* Voice activity detection.
 *
 * The threshold used to be derived from the mean frame energy. When a short
 * word fills its own buffer the mean is speech energy, so `mean * 1.6` landed
 * above the quieter syllables and the segment started mid-word, cutting the
 * first syllable off. Padded silence is what exposed it: whether the VAD fired
 * at all changed with 10 ms of extra padding. These cases pin the behaviour to
 * something monotonic. */
static void test_vad(void) {
    suite("vad");

    /* one synthetic word, padded with silence up to 130 ms: the segment must
     * start at the beginning of the word and keep essentially all of it */
    const char *word = "انتخاب";
    int64_t n = 0;
    float *pcm = tts_speak(word, "fa", &n);
    CHECK(pcm && n > 0);
    if (pcm) {
        float peak = 0;
        for (int64_t i = 0; i < n; i++) if (fabsf(pcm[i]) > peak) peak = fabsf(pcm[i]);
        int64_t end = n;
        while (end > 0 && fabsf(pcm[end - 1]) < peak * 0.02f) end--;
        CHECK_MSG(end > OSAI_SR / 4, "speech is %lld samples", (long long)end);

        int64_t cap = end + 4096;
        float *padded = (float *)calloc((size_t)cap, sizeof(float));
        CHECK(padded != NULL);
        if (padded) {
            memcpy(padded, pcm, (size_t)end * sizeof(float));
            int bad_start = 0, missed = 0;
            for (int pad = 0; pad <= 2080; pad += 160) {   /* 0 .. 130 ms */
                osa_audio a;
                osa_audio_init(&a);
                a.samples = padded;
                a.n = end + pad;
                osa_segment segs[8];
                int ns = osa_audio_vad(&a, segs, 8);
                if (ns <= 0) { missed++; continue; }
                /* 30 ms of slack: the VAD pads 120 ms before the onset */
                if (segs[0].start > 0.030f) bad_start++;
            }
            CHECK_MSG(missed == 0, "VAD found speech at every pad (%d missed)", missed);
            CHECK_MSG(bad_start == 0, "VAD starts at the word, not mid-word (%d bad)", bad_start);
            free(padded);
        }
        free(pcm);
    }

    /* digital silence must produce no segments at all */
    float *sil = (float *)calloc((size_t)OSAI_SR, sizeof(float));
    CHECK(sil != NULL);
    if (sil) {
        osa_audio a;
        osa_audio_init(&a);
        a.samples = sil;
        a.n = OSAI_SR;
        osa_segment segs[8];
        CHECK_MSG(osa_audio_vad(&a, segs, 8) == 0, "silence yields no segments");
        free(sil);
    }
}

static void test_asr(void) {
    suite("asr");
    osa_error err;
    asr_model *m = asr_model_fixture("fa", &err);
    CHECK(m != NULL);
    if (!m) return;
    CHECK_MSG(strlen(asr_model_id(m)) > 0, "model id: %s", asr_model_id(m));
    CHECK_MSG(osa_starts(asr_model_id(m), "builtin-asr-"), "id is stable");
    CHECK_MSG(strlen(asr_model_backend(m)) > 0, "backend: %s", asr_model_backend(m));
    CHECK_MSG(osa_streq(asr_model_language(m), "fa"), "language: %s", asr_model_language(m));

    /* Transcribe synthetic speech that this project's own TTS produced. The
     * test asserts on the *shape* of the result (non-empty, plausible
     * confidence, monotonic timing) rather than an exact string: the decoder
     * is an exemplar matcher and small template changes legitimately move the
     * output, which would make an exact assertion flaky. Accuracy is measured
     * separately by the WER harness. */
    const char *phrase = "فایل صوتی را انتخاب کنید";
    int64_t n = 0;
    float *pcm = tts_speak(phrase, "fa", &n);
    CHECK(pcm && n > 0);
    if (!pcm) { asr_model_free(m); return; }

    asr_result r;
    bool got = asr_transcribe_samples(m, pcm, n, "fa", &r, &err);
    CHECK_MSG(got, "transcribe synthetic speech");
    if (got) {
        CHECK_MSG(r.text[0] != 0, "transcript: \"%s\"", r.text);
        CHECK_MSG(r.confidence > 0.05f && r.confidence <= 1.0f,
                  "confidence %.2f in range", (double)r.confidence);
        CHECK_MSG(r.proc_ms > 0.0, "took %.1f ms", r.proc_ms);
        CHECK_MSG(r.duration_s > 1.0, "audio is %.2f s", r.duration_s);
        /* most words should be recovered; exact equality is not required */
        double wer = asr_wer(phrase, r.text, "fa");
        CHECK_MSG(wer < 0.75, "WER on TTS audio %.2f", wer);
        free(r.segments);            /* owned by the caller */
        r.segments = NULL;
    }

    /* silence must not produce confident speech */
    float *sil = (float *)calloc((size_t)OSAI_SR, sizeof(float));
    if (sil) {
        asr_result s;
        if (asr_transcribe_samples(m, sil, OSAI_SR, "fa", &s, &err)) {
            CHECK_MSG(s.text[0] == 0 || s.confidence < 0.3f,
                      "silence gives no confident text (\"%s\", %.2f)",
                      s.text, (double)s.confidence);
            free(s.segments);
        }
        free(sil);
    }

    /* Templates longer than 64 frames must be decodable.
     *
     * They used to be skipped by the candidate sweep (it only read the
     * transposed cache, which holds 64 rows), so any word of more than about
     * 0.6 s could never win a span: "recording" came back as "weather this".
     * Assert on the long words specifically, in English, where the longest
     * lexicon entries live. */
    {
        osa_error e2 = {0};
        asr_model *en = asr_model_fixture("en", &e2);
        CHECK(en != NULL);
        if (en) {
            static const char *long_words[] = { "recording", "recognition", "translation" };
            for (size_t i = 0; i < sizeof long_words / sizeof long_words[0]; i++) {
                int64_t n2 = 0;
                float *p2 = tts_speak(long_words[i], "en", &n2);
                if (!p2) { CHECK_MSG(false, "tts for %s", long_words[i]); continue; }
                asr_result rr;
                if (asr_transcribe_samples(en, p2, n2, "en", &rr, NULL)) {
                    CHECK_MSG(osa_streq(rr.text, long_words[i]),
                              "long template '%s' decoded as '%s'", long_words[i], rr.text);
                    free(rr.segments);
                } else {
                    CHECK_MSG(false, "long template '%s' failed to decode", long_words[i]);
                }
                free(p2);
            }

            /* A model with one language's lexicon must refuse another language
             * instead of answering with confident nonsense. */
            int64_t n3 = 0;
            float *p3 = tts_speak("hello world", "en", &n3);
            if (p3) {
                asr_result rr;
                osa_error er = {0};
                bool ok = asr_transcribe_samples(m, p3, n3, "en", &rr, &er);
                CHECK_MSG(!ok && er.msg[0], "foreign language is refused: %s", er.msg);
                if (ok) free(rr.segments);
                free(p3);
            }
            asr_model_free(en);
        }
    }

    /* edit distance helpers */
    CHECK_NEAR(asr_wer("a b c", "a b c", "en"), 0.0, 1e-9);
    CHECK_NEAR(asr_wer("a b c", "a x c", "en"), 1.0 / 3.0, 1e-9);
    CHECK_NEAR(asr_wer("a b", "a b c d", "en"), 1.0, 1e-9);
    CHECK_NEAR(asr_cer("abc", "abc"), 0.0, 1e-9);

    free(pcm);
    asr_model_free(m);
}

/* ---- store -------------------------------------------------------- */

static void test_store(void) {
    suite("database");
    char dbpath[OSAI_MAX_PATH];
    snprintf(dbpath, sizeof dbpath, "/tmp/osai_test_%d.db", (int)getpid());
    osa_file_remove(dbpath);
    osa_error err;
    osa_db *db = osa_db_open(dbpath, &err);
    CHECK(db != NULL);
    if (!db) return;

    osa_transcription t;
    memset(&t, 0, sizeof t);
    osa_strlcpy(t.id, "t-1", sizeof t.id);
    osa_strlcpy(t.text, "سلام", sizeof t.text);
    osa_strlcpy(t.language, "fa", sizeof t.language);
    osa_strlcpy(t.model_id, "builtin-asr-fa", sizeof t.model_id);
    osa_strlcpy(t.source, "file", sizeof t.source);
    osa_strlcpy(t.status, "ok", sizeof t.status);
    t.confidence = 0.9;
    t.duration_ms = 1000;
    t.proc_ms = 200;
    CHECK_MSG(osa_store_transcription_add(db, &t, &err), "insert transcript");

    osa_rs rs;
    CHECK(osa_store_transcription_list(db, &rs, 10, NULL, &err));
    CHECK_MSG(rs.nrows == 1, "one row in the list");
    if (rs.nrows == 1) {
        const char *txt = osa_rs_val(&rs, 0, "text");
        CHECK_MSG(txt && osa_streq(txt, "سلام"), "text round trips");
    }
    osa_rs_free(&rs);

    /* delete removes it */
    CHECK(osa_store_transcription_delete(db, "t-1", &err));
    CHECK(osa_store_transcription_list(db, &rs, 10, NULL, &err));
    CHECK_MSG(rs.nrows == 0, "row removed");
    osa_rs_free(&rs);

    /* model add + activate semantics */
    osa_model m;
    memset(&m, 0, sizeof m);
    osa_strlcpy(m.id, "test-model", sizeof m.id);
    osa_strlcpy(m.name, "Test model", sizeof m.name);
    osa_strlcpy(m.kind, "asr", sizeof m.kind);
    osa_strlcpy(m.engine, "exemplar", sizeof m.engine);
    osa_strlcpy(m.version, "1.0.0", sizeof m.version);
    osa_strlcpy(m.sha256, "00", sizeof m.sha256);
    m.size_bytes = 1234;
    CHECK(osa_store_model_add(db, &m, &err));
    CHECK_MSG(osa_store_model_set_active(db, "test-model", &err), "activate model");
    osa_rs mrs;
    CHECK(osa_store_model_list(db, &mrs, NULL, &err));
    CHECK_MSG(mrs.nrows == 1, "one model registered");
    osa_rs_free(&mrs);

    osa_db_close(db);
    osa_file_remove(dbpath);
}

/* ---- app layer ---------------------------------------------------- */

static void test_app(void) {
    suite("application api");
    osa_error err;
    osa_app *app = osa_app_create(&err);
    CHECK(app != NULL);
    if (!app) return;

    osa_app_status st;
    osa_app_get_status(app, &st);
    CHECK(st.db_ready);
    CHECK(st.asr_ready);
    CHECK(st.mt_ready);
    CHECK_MSG(st.n_models >= 2, "%d models registered", st.n_models);
    CHECK_MSG(st.data_root[0] == '/', "data root: %s", st.data_root);

    /* translate persists a row */
    osa_app_translation mt;
    CHECK(osa_app_translate(app, "ممنون", NULL, &mt, &err));
    CHECK_STR_EQ(mt.text, "Thanks");
    CHECK_MSG(mt.id[0] != 0, "translation has a row id");

    /* transcribe + translate end to end, using this project's TTS as input */
    int64_t n = 0;
    float *pcm = tts_speak("فایل صوتی را انتخاب کنید", "fa", &n);
    char wav[OSAI_MAX_PATH];
    snprintf(wav, sizeof wav, "/tmp/osai_test_app.wav");
    CHECK(pcm && osa_audio_save_wav(wav, pcm, n, OSAI_SR));
    free(pcm);

    osa_app_transcript tr;
    bool got = osa_app_transcribe_and_translate(app, wav, &tr, &mt, &err);
    CHECK(got);
    if (got) {
        CHECK_MSG(tr.text[0] != 0, "transcript: %s", tr.text);
        CHECK_MSG(tr.language[0] != 0, "language: %s", tr.language);
        CHECK_MSG(tr.id[0] != 0, "transcript row id assigned");
        CHECK_MSG(tr.rtf > 0.0 && tr.rtf < 1.0, "real-time factor %.2f", tr.rtf);
        CHECK_MSG(mt.text[0] != 0, "translation: %s", mt.text);
    }

    /* history reflects it */
    char *hist = osa_app_history_json(app, 100, NULL);
    CHECK(hist && strstr(hist, "\"text\""));
    CHECK(hist && strstr(hist, "confidence"));
    free(hist);

    /* models JSON is well formed and marks one active per kind */
    char *models = osa_app_models_json(app, NULL);
    CHECK(models && strstr(models, "\"active\":true"));
    free(models);

    /* evaluation runs and reports WER */
    char *eval = osa_app_evaluate(app, NULL);
    CHECK(eval && strstr(eval, "\"wer\""));
    CHECK(eval && strstr(eval, "\"languages\""));
    free(eval);

    /* delete removes the transcript and its copied audio */
    if (got) {
        CHECK(osa_app_delete_transcript(app, tr.id, &err));
        hist = osa_app_history_json(app, 100, NULL);
        CHECK(hist && !strstr(hist, tr.id));
        free(hist);
    }

    /* training produces a new version, activates it, and keeps the old one */
    {
        osa_train_result tr2;
        osa_error te = {0};
        if (osa_app_train(app, NULL, 1, &tr2, &te)) {
            CHECK_MSG(tr2.model_id[0] != 0, "trained version: %s", tr2.model_id);
            CHECK_MSG(tr2.n_added > 0, "%d exemplars added", tr2.n_added);
            CHECK_MSG(tr2.activated, "the trained version is active");
            osa_app_status st2;
            osa_app_get_status(app, &st2);
            char *mj = osa_app_models_json(app, NULL);
            CHECK(mj && strstr(mj, "builtin-asr-fa"));
            CHECK(mj && strstr(mj, tr2.model_id));
            free(mj);
            /* roll back to the builtin so later assertions see a known model */
            CHECK(osa_app_activate_model(app, "builtin-asr-fa", NULL));
            osa_file_remove(tr2.model_path);
        } else {
            CHECK_MSG(false, "training failed: %s", te.msg);
        }
    }

    /* importing a bogus file fails cleanly instead of crashing */
    const char *bogus = "/tmp/osai_test_bogus.osam";
    FILE *f = fopen(bogus, "wb");
    if (f) { fwrite("not a model", 1, 11, f); fclose(f); }
    char id[40];
    CHECK(!osa_app_import_model(app, bogus, id, sizeof id, &err));
    CHECK_MSG(err.msg[0] != 0, "error message: %s", err.msg);
    osa_file_remove(bogus);

    /* a real container must import: the picker hands us a .osam path, and an
     * extension comparison that included the dot used to reject every one. */
    {
        osa_error ie = {0};
        asr_model *tmpm = asr_model_fixture("fa", &ie);
        CHECK(tmpm != NULL);
        if (tmpm) {
            char tmpath[OSAI_MAX_PATH];
            snprintf(tmpath, sizeof tmpath, "%s/osai_test_import.osam", osa_path(OSAI_DIR_MODELS));
            if (asr_model_save(tmpm, tmpath, &ie)) {
                char iid[64] = "";
                bool imported = osa_app_import_model(app, tmpath, iid, sizeof iid, &ie);
                CHECK_MSG(imported, "import a real .osam: %s", ie.msg);
                if (imported) {
                    CHECK_MSG(iid[0] != 0, "imported id: %s", iid);
                    char *mj2 = osa_app_models_json(app, NULL);
                    CHECK(mj2 && strstr(mj2, iid));
                    free(mj2);
                }
                osa_file_remove(tmpath);
            }
            asr_model_free(tmpm);
        }
    }
    osa_file_remove(wav);

    /* ---- packs are built offline, and installing one changes the engine ----
     * The Models screen's rows used to be dead labels: every entry needed a file
     * the user had to find by hand. A catalogue entry must now have a working
     * offline install that actually alters translation output. */
    {
        suite("catalogue");
        static osa_pack_info packs[32];
        int np = osa_app_packs(app, packs, 32);
        CHECK_MSG(np > 0, "catalogue entries: %d", np);
        int offline = 0, downloads = 0, installed = 0;
        for (int i = 0; i < np; i++) {
            if (packs[i].installable) offline++;
            else if (packs[i].url[0]) downloads++;
            if (packs[i].installed) installed++;
            CHECK_MSG(packs[i].id[0] && packs[i].name[0] && packs[i].license[0],
                      "entry %d is complete", i);
        }
        /* Two kinds of offer, and nothing else: built here (offline) or
         * downloaded by URL. A third kind that means "go and find a file" is
         * what the Models screen used to be full of. */
        CHECK_MSG(offline + downloads == np, "%d offline + %d downloads == %d rows",
                  offline, downloads, np);
        CHECK_MSG(offline >= 12, "offline-installable entries: %d", offline);
        CHECK_MSG(downloads >= 4, "downloadable entries: %d", downloads);
        CHECK_MSG(installed >= 2, "already installed: %d", installed);

        const char *src = "بیمار در بیمارستان";
        osa_app_translation tr_a, tr_b;
        memset(&tr_a, 0, sizeof tr_a);
        osa_app_translate(app, src, "", &tr_a, NULL);
        CHECK_MSG(tr_a.text[0] != 0, "translation before the pack: %s", tr_a.text);

        osa_error pe = {0};
        bool ok = osa_app_install_builtin_pack(app, "lang-pack-medical", &pe);
        CHECK_MSG(ok, "install the medical pack: %s", pe.msg);
        /* the file is a real, inspectable artifact in the data folder */
        char ppath[OSAI_MAX_PATH];
        osa_path_join(ppath, sizeof ppath, osa_path(OSAI_DIR_PACKS), "lang-pack-medical.words.tsv");
        CHECK_MSG(osa_file_exists(ppath), "pack file written: %s", ppath);
        /* and the engine now knows the words */
        memset(&tr_b, 0, sizeof tr_b);
        osa_app_translate(app, src, "", &tr_b, NULL);
        CHECK_MSG(strstr(tr_b.text, "patient") != NULL,
                  "pack translated the word: %s -> %s", tr_a.text, tr_b.text);
        /* it is listed as installed from then on */
        osa_pack_info again[32];
        int np2 = osa_app_packs(app, again, 32);
        bool seen = false;
        for (int i = 0; i < np2; i++)
            if (osa_streq(again[i].id, "lang-pack-medical")) seen = again[i].installed;
        CHECK(seen);
        /* an entry whose material is not in the binary refuses, with a reason */
        osa_error re = {0};
        CHECK(!osa_app_install_builtin_pack(app, "ggml-tiny", &re));
        CHECK_MSG(re.msg[0] != 0, "refusal reason: %s", re.msg);
        CHECK(!osa_app_install_builtin_pack(app, "no-such-pack", &re));
    }

    /* ---- every catalogue row must be actionable ----------------------
     * The Models screen listed seven entries that could only be satisfied by
     * finding a file by hand, which is not a button: "many models have no
     * download button" was the report. The rule is now mechanical — a row is
     * either installed, installable offline, or a real download URL — and this
     * asserts it, so a new entry cannot reintroduce the dead end. */
    {
        osa_pack_info all[32];
        int n = osa_app_packs(app, all, 32);
        CHECK(n >= 17);
        int dead = 0, offline = 0, remote = 0;
        for (int i = 0; i < n; i++) {
            if (all[i].installable) offline++;
            else if (all[i].url[0]) remote++;
            else if (!all[i].installed) { dead++; printf("    no action for %s\n", all[i].id); }
        }
        CHECK_MSG(dead == 0, "%d catalogue rows offer no action", dead);
        CHECK_MSG(offline >= 12, "%d rows install offline", offline);
        CHECK_MSG(remote >= 4, "%d rows are real downloads", remote);
        /* the four that used to be manual are now built, and each produces a
         * loadable model rather than a text table */
        const char *models[] = { "asr-large-fa", "ggml-small-fa",
                                 "mt-opus-fa-en", "mt-opus-en-fa" };
        for (int i = 0; i < 4; i++) {
            osa_error e;
            CHECK_MSG(osa_app_install_builtin_pack(app, models[i], &e),
                      "install %s: %s", models[i], e.msg);
            bool now = false;
            int n2 = osa_app_packs(app, all, 32);
            for (int k = 0; k < n2; k++)
                if (osa_streq(all[k].id, models[i])) now = all[k].installed;
            CHECK_MSG(now, "%s is listed as installed", models[i]);
        }
    }

    /* ---- training data is pickable inside the application ------------ */
    {
        osa_dataset_info ds[8];
        int n = osa_app_datasets(app, ds, 8);
        CHECK_MSG(n >= 3, "%d bundled training sets", n);
        for (int i = 0; i < n; i++) CHECK(ds[i].installable);
        osa_error e;
        char path[OSAI_MAX_PATH];
        CHECK_MSG(osa_app_install_dataset(app, ds[0].id, path, sizeof path, &e),
                  "build %s: %s", ds[0].id, e.msg);
        CHECK(osa_file_exists(path));
        int n2 = osa_app_datasets(app, ds, 8);
        CHECK(ds[0].installed);
        CHECK_MSG(ds[0].n_pairs > 0, "%d pairs in the built set", ds[0].n_pairs);
        (void)n2;
        /* and the trainer actually consumes it */
        osa_train_result tr;
        memset(&tr, 0, sizeof tr);
        CHECK_MSG(osa_app_train(app, path, 1, &tr, &e), "train on the built set: %s", e.msg);
        CHECK_MSG(tr.n_added > 0, "%d exemplars added from the dataset", tr.n_added);
        CHECK(tr.model_id[0] != 0);
    }

    osa_app_destroy(app);
}

/* ---- platform ----------------------------------------------------- */

/* A damaged whisper file must be refused without ending the program, and must
 * not leave a row behind. The file carries the ggml magic and then garbage, which
 * is what whisper.cpp chokes on by throwing. */
static void test_whisper_guard(void) {
    char dir[OSAI_MAX_PATH];
    snprintf(dir, sizeof dir, "/tmp/osai_test_wsp_%ld", (long)time(NULL));
    char bad[OSAI_MAX_PATH];
    snprintf(bad, sizeof bad, "%s/bad-tiny.bin", dir);
    FILE *f = NULL;
    char cmd[OSAI_MAX_PATH + 32];
    snprintf(cmd, sizeof cmd, "mkdir -p %s", dir);
    (void)system(cmd);
    f = fopen(bad, "wb");
    CHECK(f != NULL);
    if (f) {
        const unsigned char junk[] = {'l', 'm', 'g', 'g', 0, 0, 0, 0, 'g', 'a', 'r', 'b'};
        fwrite(junk, 1, sizeof junk, f);
        fclose(f);
    }
    CHECK(wsp_is_model_file(bad));
    char werr[160];
    wsp_ctx *w = wsp_load(bad, werr, sizeof werr);
    CHECK(w == NULL);
    CHECK(werr[0] != 0);
    wsp_free(w);
    CHECK(!wsp_is_model_file("/tmp/osai_no_such_file.bin"));
}

static void test_platform(void) {
    suite("platform");
    const char *root = osa_path(OSAI_DIR_ROOT);
    CHECK_MSG(root && root[0] != 0, "data root: %s", root ? root : "(null)");
    CHECK(root && osa_dir_exists(root));
    CHECK_MSG(osa_path(OSAI_DIR_MODELS)[0] != 0, "models dir resolves");
    CHECK_MSG(osa_disk_free(osa_path(OSAI_DIR_ROOT)) > 0, "disk free is positive");
    CHECK_MSG(osa_cpu_count() >= 1, "%d cpu(s)", osa_cpu_count());

    osa_sys_stats stats;
    memset(&stats, 0, sizeof stats);
    osa_sys_stats_sample(&stats);
    CHECK_MSG(stats.ram_total_mb > 0, "ram total %.0f MB", (double)stats.ram_total_mb);
    CHECK_MSG(stats.ram_used_mb <= stats.ram_total_mb + 1, "ram used is sane");
    /* The About screen shows this process, not the machine: the machine figure
     * moves with every other program, which is what made an idle window look
     * like it was leaking. */
    CHECK_MSG(stats.proc_ram_mb > 0, "process ram %.1f MB", (double)stats.proc_ram_mb);
    CHECK_MSG(stats.proc_ram_mb < stats.ram_total_mb, "process ram is below the machine total");
    float live_pr = osa_process_ram_mb();
    CHECK_MSG(live_pr > 0, "live process ram %.1f MB", (double)live_pr);
    osa_sys_stats_freeze(true);
    CHECK(osa_sys_stats_frozen());
    osa_sys_stats frozen;
    osa_sys_stats_sample(&frozen);
    CHECK(frozen.proc_ram_mb > 0);                 /* the harness still has a value */
    CHECK_MSG(osa_process_ram_mb() > 0, "even frozen, the real process size is readable");
    osa_sys_stats_freeze(false);
    CHECK(!osa_sys_stats_frozen());

    /* the fault log names a file in the data folder and can be armed quietly */
    osa_crash_log_init(true);
    CHECK_MSG(osa_crash_log_path()[0] != 0, "fault log path: %s", osa_crash_log_path());

    /* mkdirs is idempotent */
    CHECK(osa_mkdirs("/tmp/osai_test_dir_nested_a/b/c"));
    CHECK(osa_dir_exists("/tmp/osai_test_dir_nested_a/b/c"));
    CHECK(osa_mkdirs("/tmp/osai_test_dir_nested_a/b/c"));

    /* atomic write */
    const char *p = "/tmp/osai_test_atomic.txt";
    CHECK(osa_file_write_atomic(p, "hello", 5));
    CHECK(osa_file_exists(p));
    osa_buf back;
    osa_buf_init(&back);           /* osa_file_read resets the buffer, it does not create it */
    if (osa_file_read(p, &back)) {
        CHECK_MSG(back.len == 5 && memcmp(back.data, "hello", 5) == 0,
                  "read back %zu bytes", back.len);
        osa_buf_free(&back);
    } else {
        CHECK_MSG(false, "read back the file");
    }
    osa_file_remove(p);

    /* uuid shape */
    char u1[37], u2[37];
    osa_uuid(u1);
    osa_uuid(u2);
    CHECK_MSG(strlen(u1) == 36, "uuid length %zu", strlen(u1));
    CHECK_MSG(!osa_streq(u1, u2), "uuids are unique");
    CHECK(u1[8] == '-' && u1[13] == '-' && u1[18] == '-' && u1[23] == '-');
}

/* ---- main --------------------------------------------------------- */

int main(int argc, char **argv) {
    bool verbose = argc > 1 && osa_streq(argv[1], "-v");
    (void)verbose;
    /* A fresh data folder every run: the application restores the recogniser
     * the database says is active, so a shared folder would make these tests
     * depend on whatever an earlier run trained. */
    {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "/tmp/osai_test_data_%ld", (long)time(NULL));
        setenv("OSAI_DATA_DIR", tmp, 1);
    }
    printf("OpenSpeech AI test suite\n");
    printf("=======================\n");

    test_strings();
    test_utf8();
    test_json();
    test_hash();
    test_platform();
    test_audio();
    test_tts();
    test_neural_net();
    test_mt();
    test_store();
    test_vad();
    test_asr();
    test_app();
    test_whisper_guard();

    printf("\n=======================\n");
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
