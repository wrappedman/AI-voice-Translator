/* =====================================================================
 *  OpenSpeech AI — translation engine implementation
 * ===================================================================== */
#include "mt.h"
#include "../core/osai_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* =====================================================================
 *  Builtin phrasebook
 *
 *  Persian and English sentences are stored as sentence pairs; a sentence
 *  that matches an entry exactly is translated as a whole, which is what
 *  gives natural output for the common UI/system phrases. Anything else
 *  falls back to word-level glossing with light morphology handling.
 * ===================================================================== */
typedef struct phrase_pair { const char *fa; const char *en; } phrase_pair;

static const phrase_pair SENTENCES[] = {
    {"سلام", "Hello"},
    {"سلام حال شما خوب است", "Hello, how are you?"},
    {"حال شما خوب است", "How are you?"},
    {"خوب هستم متشکرم", "I am fine, thank you"},
    {"صبح بخیر", "Good morning"},
    {"شب بخیر", "Good night"},
    {"خداحافظ", "Goodbye"},
    {"ممنون", "Thanks"},
    {"خیلی ممنون", "Thank you very much"},
    {"لطفا", "Please"},
    {"بله", "Yes"},
    {"نه", "No"},

    {"فایل صوتی را انتخاب کنید", "Select the audio file"},
    {"فایل صوتی", "Audio file"},
    {"میکروفون را روشن کنید", "Turn on the microphone"},
    {"ضبط صدا را شروع کنید", "Start recording the audio"},
    {"ضبط را متوقف کنید", "Stop the recording"},
    {"تشخیص گفتار", "Speech recognition"},
    {"تشخیص گفتار فارسی", "Persian speech recognition"},
    {"متن را به زبان انگلیسی ترجمه کنید", "Translate the text into English"},
    {"ترجمه متن", "Text translation"},
    {"نتیجه ترجمه", "Translation result"},
    {"نتیجه ترجمه را ذخیره کنید", "Save the translation result"},
    {"متن", "Text"},
    {"ترجمه", "Translation"},
    {"زبان", "Language"},
    {"فارسی", "Persian"},
    {"انگلیسی", "English"},
    {"ذخیره", "Save"},
    {"ذخیره کنید", "Save"},
    {"حذف", "Delete"},
    {"حذف کنید", "Delete"},
    {"تنظیمات", "Settings"},
    {"جستجو", "Search"},
    {"باز کردن", "Open"},
    {"بارگذاری", "Load"},
    {"خروج", "Exit"},

    {"مدل", "Model"},
    {"مدل زبانی", "Language model"},
    {"مدل آموزش داده شده", "Trained model"},
    {"مدل آموزش داده شده را ذخیره کنید", "Save the trained model"},
    {"آموزش", "Training"},
    {"آموزش مدل", "Model training"},
    {"آموزش داده", "Trained"},
    {"دوره آموزشی", "Training epoch"},
    {"دقت مدل", "Model accuracy"},
    {"ارزیابی", "Evaluation"},
    {"نسخه", "Version"},
    {"نسخه جدید", "New version"},
    {"نسخه قبلی", "Previous version"},
    {"بازگردانی", "Rollback"},
    {"پشتیبان", "Backup"},
    {"پشتیبان گیری", "Backup"},

    {"این یک آزمایش نرم افزار هوش مصنوعی است", "This is an artificial intelligence software test"},
    {"نرم افزار", "Software"},
    {"هوش مصنوعی", "Artificial intelligence"},
    {"هوش", "Intelligence"},
    {"مصنوعی", "Artificial"},
    {"آزمایش", "Test"},
    {"سیستم", "System"},
    {"سیستم به صورت آفلاین کار می کند", "The system works offline"},
    {"آفلاین", "Offline"},
    {"آنلاین", "Online"},
    {"اینترنت", "Internet"},
    {"بدون اینترنت", "Without internet"},
    {"کار می کند", "Works"},
    {"امروز هوا خیلی قشنگ است", "The weather is very nice today"},
    {"امروز", "Today"},
    {"هوا", "Weather"},
    {"خیلی", "Very"},
    {"قشنگ", "Nice"},
    {"خوب", "Good"},
    {"بد", "Bad"},
    {"بزرگ", "Big"},
    {"کوچک", "Small"},
    {"سریع", "Fast"},
    {"آهسته", "Slow"},
    {"دقیق", "Accurate"},
    {"دقیق است", "Is accurate"},
    {"تشخیص گفتار فارسی دقیق است", "Persian speech recognition is accurate"},

    {"کاربر", "User"},
    {"رمز عبور", "Password"},
    {"حریم خصوصی", "Privacy"},
    {"امنیت", "Security"},
    {"داده", "Data"},
    {"داده ها", "Data"},
    {"پایگاه داده", "Database"},
    {"فایل", "File"},
    {"پوشه", "Folder"},
    {"مسیر", "Path"},
    {"نام", "Name"},
    {"تاریخ", "Date"},
    {"زمان", "Time"},
    {"مدت", "Duration"},
    {"اندازه", "Size"},
    {"شماره", "Number"},

    {"گزارش", "Report"},
    {"گزارش خطا", "Error report"},
    {"خطا", "Error"},
    {"موفق", "Successful"},
    {"ناموفق", "Failed"},
    {"در حال انجام", "In progress"},
    {"تکمیل شد", "Completed"},
    {"لغو", "Cancel"},
    {"تایید", "Confirm"},
    {"بستن", "Close"},
    {"بعدی", "Next"},
    {"قبلی", "Previous"},
    {"شروع", "Start"},
    {"پایان", "End"},
    {"ادامه", "Continue"},
    {"توقف", "Stop"},
    {"صدا", "Sound"},
    {"صوتی", "Audio"},
    {"ضبط", "Recording"},
    {"میکروفون", "Microphone"},
    {"بلندگو", "Speaker"},
    {"سرعت", "Speed"},
    {"کیفیت", "Quality"},
    {"نتیجه", "Result"},
    {"ورودی", "Input"},
    {"خروجی", "Output"},
    {"شروع کنید", "Start"},
    {"فعال", "Enabled"},
    {"غیرفعال", "Disabled"},
    {"روشن", "On"},
    {"خاموش", "Off"},
    {"آماده", "Ready"},
    {"در حال بارگذاری", "Loading"},
    {"کمک", "Help"},
    {"درباره", "About"},
    {"نسخه برنامه", "Application version"},
};

/* Word-level gloss, used for sentences that are not in the phrasebook.
 * Longer entries are matched first so that "هوش مصنوعی" beats "هوش". */
static const phrase_pair WORDS[] = {
    {"هوش مصنوعی", "artificial intelligence"},
    {"تشخیص گفتار", "speech recognition"},
    {"فایل صوتی", "audio file"},
    {"پایگاه داده", "database"},
    {"نرم افزار", "software"},
    {"سخت افزار", "hardware"},
    {"رمز عبور", "password"},
    {"حریم خصوصی", "privacy"},
    {"آموزش داده شده", "trained"},
    {"کار می کند", "works"},
    {"به صورت", "as"},
    {"متن", "text"}, {"ترجمه", "translation"}, {"زبان", "language"},
    {"فارسی", "Persian"}, {"انگلیسی", "English"},
    {"سلام", "hello"}, {"حال", "how"}, {"شما", "you"}, {"خوب", "good"},
    {"است", "is"}, {"هست", "is"}, {"این", "this"}, {"یک", "a"}, {"آن", "that"},
    {"آزمایش", "test"}, {"هوش", "intelligence"}, {"مصنوعی", "artificial"},
    {"سیستم", "system"}, {"آفلاین", "offline"}, {"آنلاین", "online"},
    {"کار", "work"}, {"می", ""}, {"کند", "does"}, {"کنید", "do"},
    {"امروز", "today"}, {"هوا", "weather"}, {"خیلی", "very"}, {"قشنگ", "nice"},
    {"دقیق", "accurate"}, {"مدل", "model"}, {"آموزش", "training"},
    {"داده", "data"}, {"شده", "done"}, {"ذخیره", "save"}, {"را", ""},
    {"نتیجه", "result"}, {"لطفا", "please"}, {"فایل", "file"},
    {"صوتی", "audio"}, {"انتخاب", "select"}, {"میکروفون", "microphone"},
    {"روشن", "on"}, {"ضبط", "recording"}, {"صدا", "sound"}, {"شروع", "start"},
    {"توقف", "stop"}, {"گفتار", "speech"}, {"به", "to"}, {"از", "from"},
    {"در", "in"}, {"با", "with"}, {"برای", "for"}, {"و", "and"}, {"یا", "or"},
    {"که", "that"}, {"نه", "no"}, {"بله", "yes"}, {"ممنون", "thanks"},
    {"بزرگ", "big"}, {"کوچک", "small"}, {"سریع", "fast"}, {"آهسته", "slow"},
    {"تنظیمات", "settings"}, {"گزارش", "report"}, {"خطا", "error"},
    {"زمان", "time"}, {"تاریخ", "date"}, {"نام", "name"}, {"شماره", "number"},
    {"کاربر", "user"}, {"درباره", "about"}, {"کمک", "help"},
    {"نسخه", "version"}, {"جدید", "new"}, {"قبلی", "previous"},
    {"کیفیت", "quality"}, {"سرعت", "speed"}, {"حالا", "now"},
    {"صورت", "form"}, {"وجه", "face"}, {"پیدا", "find"}, {"بیشتر", "more"},
    {"کمتر", "less"}, {"همه", "all"}, {"چند", "some"}, {"هر", "each"},
    /* frequent English function words, so EN->FA glossing stays in Persian */
    {"recording", "ضبط"}, {"recognition", "تشخیص"}, {"world", "دنیا"}, {"hello world", "سلام دنیا"},
    {"start", "شروع"}, {"stop", "توقف"}, {"select", "انتخاب"},
    {"save", "ذخیره"}, {"the", ""}, {"and", "و"}, {"or", "یا"},
    {"to", "به"}, {"from", "از"}, {"in", "در"}, {"with", "با"}, {"for", "برای"},
    {"of", "از"}, {"is", "است"}, {"are", "هستند"}, {"this", "این"}, {"that", "آن"},
    {"hello", "سلام"}, {"test", "آزمایش"}, {"speech", "گفتار"}, {"my", "مال من"},
    {"your", "شما"}, {"we", "ما"}, {"you", "شما"}, {"it", "آن"}, {"not", "نه"},
    {"very", "خیلی"}, {"good", "خوب"}, {"bad", "بد"}, {"new", "جدید"},
    {"old", "قدیمی"}, {"all", "همه"}, {"more", "بیشتر"}, {"less", "کمتر"},
    {"how", "چگونه"}, {"what", "چه"}, {"when", "کی"}, {"where", "کجا"},
    {"why", "چرا"}, {"who", "چه کسی"}, {"can", "می تواند"}, {"will", "خواهد"},
    {"audio", "صوتی"}, {"file", "فایل"}, {"model", "مدل"}, {"trained", "آموزش دیده"},
    {"text", "متن"}, {"language", "زبان"}, {"english", "انگلیسی"}, {"persian", "فارسی"},
    {"offline", "آفلاین"}, {"system", "سیستم"}, {"software", "نرم افزار"},
    {"intelligence", "هوش"}, {"artificial", "مصنوعی"}, {"result", "نتیجه"},
    {"translation", "ترجمه"}, {"save", "ذخیره"}, {"turn", "روشن کن"},
    {"microphone", "میکروفون"}, {"on", "روشن"}, {"off", "خاموش"},
};

/* English -> Persian direction uses the same tables reversed. */

/* =====================================================================
 *  Model container (.osmt)
 * ===================================================================== */
#define MT_MAGIC 0x544D534FU        /* "OSMT" little-endian */
#define MT_VERSION 1

struct mt_model {
    char     id[64];
    char     name[128];
    char     license[128];
    char     source[512];
    char     backend[24];
    char     src_tag[8];
    char     dst_tag[8];

    /* loaded lexicon (NULL => use the builtin tables) */
    phrase_pair *pairs;
    int          n_pairs;
    int          cap_pairs;
    phrase_pair *gloss;
    int          n_gloss;
    int          cap_gloss;
};

/* ---- small helpers ------------------------------------------------ */

static bool is_persian_cp(uint32_t cp) {
    return (cp >= 0x0600 && cp <= 0x06FF) ||     /* Arabic            */
           (cp >= 0xFB50 && cp <= 0xFDFF) ||     /* presentation A    */
           (cp >= 0xFE70 && cp <= 0xFEFF);       /* presentation B    */
}

static uint32_t utf8_next(const char **p) {
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

static int utf8_put(char *dst, size_t cap, size_t at, uint32_t cp) {
    char tmp[4];
    int n = 0;
    if (cp < 0x80) { tmp[n++] = (char)cp; }
    else if (cp < 0x800) { tmp[n++] = (char)(0xC0 | (cp >> 6)); tmp[n++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        tmp[n++] = (char)(0xE0 | (cp >> 12));
        tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        tmp[n++] = (char)(0xF0 | (cp >> 18));
        tmp[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[n++] = (char)(0x80 | (cp & 0x3F));
    }
    if (at + (size_t)n < cap) { memcpy(dst + at, tmp, (size_t)n); dst[at + (size_t)n] = 0; }
    return n;
}

static bool is_space_cp(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
           cp == '.' || cp == ',' || cp == '!' || cp == '?' || cp == ';' ||
           cp == ':' || cp == 0x061F || cp == 0x060C;
}

/* Copy [begin,end) of src into dst, collapsing runs of whitespace. */
static void take_trim(const char *begin, const char *end, char *dst, size_t cap) {
    size_t o = 0;
    const char *p = begin;
    while (p < end) {
        const char *save = p;
        uint32_t cp = utf8_next(&p);
        if (is_space_cp(cp)) {
            if (o && dst[o - 1] != ' ' && o + 1 < cap) dst[o++] = ' ';
            continue;
        }
        p = save;
        const unsigned char *u = (const unsigned char *)p;
        int len = 1;
        if (u[0] >= 0xC0 && u[0] < 0xE0) len = 2;
        else if (u[0] >= 0xE0 && u[0] < 0xF0) len = 3;
        else if (u[0] >= 0xF0) len = 4;
        if ((size_t)(end - p) < (size_t)len) break;
        if (o + (size_t)len < cap) { memcpy(dst + o, p, (size_t)len); o += (size_t)len; dst[o] = 0; }
        p += len;
    }
    if (o && dst[o - 1] == ' ') { o--; dst[o] = 0; }
    if (o == 0) { dst[0] = 0; }
}

/* Normalise Persian orthography: unify Arabic/Persian yeh and kaf, drop the
 * optional zero-width non-joiner, and collapse spaces. Without this the same
 * word arrives in three spellings and never matches the lexicon. */
static void normalize_fa(const char *in, char *out, size_t cap) {
    size_t o = 0;
    const char *p = in;
    out[0] = 0;
    while (*p) {
        const char *save = p;
        uint32_t cp = utf8_next(&p);
        uint32_t emit = cp;
        if (cp == 0x064A) emit = 0x06CC;          /* Arabic yeh  -> Persian yeh */
        else if (cp == 0x0643) emit = 0x06A9;      /* Arabic kaf  -> Persian kaf */
        else if (cp == 0x06C0) emit = 0x0647;      /* heh with yeh -> heh       */
        else if (cp == 0x200C || cp == 0x200D) {   /* ZWNJ / ZWJ: remove        */
            continue;
        } else if (cp == 0x0640) {                 /* tatweel: remove           */
            continue;
        }
        if (is_space_cp(emit)) {
            if (o == 0 || out[o - 1] == ' ') { p = save; continue; }
            emit = ' ';
        }
        o += (size_t)utf8_put(out, cap, o, emit);
        if (o + 8 >= cap) break;
    }
    while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
    out[o] = 0;
}

/* =====================================================================
 *  Model lifecycle
 * ===================================================================== */
const char *mt_model_id(const mt_model *m) { return m ? m->id : ""; }
const char *mt_model_name(const mt_model *m) { return m ? m->name : ""; }
const char *mt_model_license(const mt_model *m) { return m ? m->license : ""; }
const char *mt_model_source(const mt_model *m) { return m ? m->source : ""; }
const char *mt_model_backend(const mt_model *m) { return m ? m->backend : "phrasebook"; }

static mt_model *model_alloc(void) {
    mt_model *m = (mt_model *)calloc(1, sizeof *m);
    return m;
}

void mt_model_free(mt_model *m) {
    if (!m) return;
    free(m->pairs);
    free(m->gloss);
    free(m);
}

mt_model *mt_model_builtin(osa_error *err) {
    mt_model *m = model_alloc();
    if (!m) { osa_err_set(err, "out of memory"); return NULL; }
    osa_strlcpy(m->id, "builtin-phrasebook-v1", sizeof m->id);
    osa_strlcpy(m->name, "OpenSpeech AI builtin phrasebook (fa<->en)", sizeof m->name);
    osa_strlcpy(m->license, "GPL-3.0-or-later (part of OpenSpeech AI)", sizeof m->license);
    osa_strlcpy(m->source, "bundled with the application", sizeof m->source);
    osa_strlcpy(m->backend, "phrasebook", sizeof m->backend);
    osa_err_ok(err);
    return m;
}

/* ---- .osmt serialisation ------------------------------------------ */
enum { MT_HDR_ID = 64, MT_HDR_NAME = 128, MT_HDR_LIC = 128, MT_HDR_SRC = 512,
       MT_HDR_BACKEND = 24, MT_HDR_TAG = 8 };

typedef struct mt_hdr {
    uint32_t magic;
    uint32_t version;
    int32_t  n_pairs;
    int32_t  n_gloss;
    char     id[MT_HDR_ID];
    char     name[MT_HDR_NAME];
    char     license[MT_HDR_LIC];
    char     source[MT_HDR_SRC];
    char     backend[MT_HDR_BACKEND];
    char     src_tag[MT_HDR_TAG];
    char     dst_tag[MT_HDR_TAG];
} mt_hdr;

static bool write_str14(FILE *f, const char *s) {
    unsigned char len = (unsigned char)(strlen(s) > 250 ? 250 : strlen(s));
    if (fwrite(&len, 1, 1, f) != 1) return false;
    return len == 0 || fwrite(s, 1, len, f) == len;
}

static bool read_str14(FILE *f, char *dst, size_t cap) {
    unsigned char len = 0;
    if (fread(&len, 1, 1, f) != 1) return false;
    if ((size_t)len >= cap) return false;
    if (len && fread(dst, 1, len, f) != len) return false;
    dst[len] = 0;
    return true;
}

bool mt_model_save(const mt_model *m, const char *path, osa_error *err) {
    if (!m || !path) { osa_err_set(err, "bad arguments"); return false; }
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { osa_err_set(err, "cannot write %s", tmp); return false; }
    mt_hdr h;
    memset(&h, 0, sizeof h);
    h.magic = MT_MAGIC;
    h.version = MT_VERSION;
    h.n_pairs = m->pairs ? m->n_pairs : 0;
    h.n_gloss = m->gloss ? m->n_gloss : 0;
    osa_strlcpy(h.id, m->id, sizeof h.id);
    osa_strlcpy(h.name, m->name, sizeof h.name);
    osa_strlcpy(h.license, m->license, sizeof h.license);
    osa_strlcpy(h.source, m->source, sizeof h.source);
    osa_strlcpy(h.backend, m->backend, sizeof h.backend);
    osa_strlcpy(h.src_tag, m->src_tag, sizeof h.src_tag);
    osa_strlcpy(h.dst_tag, m->dst_tag, sizeof h.dst_tag);
    bool ok = fwrite(&h, sizeof h, 1, f) == 1;
    for (int i = 0; ok && i < h.n_pairs; i++)
        ok = write_str14(f, m->pairs[i].fa) && write_str14(f, m->pairs[i].en);
    for (int i = 0; ok && i < h.n_gloss; i++)
        ok = write_str14(f, m->gloss[i].fa) && write_str14(f, m->gloss[i].en);
    if (ok) ok = fflush(f) == 0;
    fclose(f);
    if (!ok) { osa_file_remove(tmp); osa_err_set(err, "write failed"); return false; }
    if (!osa_file_rename(tmp, path)) {
        osa_file_remove(tmp);
        osa_err_set(err, "cannot finalize %s", path);
        return false;
    }
    osa_err_ok(err);
    return true;
}

static bool add_pair(phrase_pair **arr, int *n, int *cap, const char *a, const char *b) {
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 64;
        phrase_pair *na = (phrase_pair *)realloc(*arr, (size_t)nc * sizeof(phrase_pair));
        if (!na) return false;
        *arr = na;
        *cap = nc;
    }
    if ((*arr)[*n].fa == NULL) { }
    size_t la = strlen(a), lb = strlen(b);
    char *pa = (char *)malloc(la + 1), *pb = (char *)malloc(lb + 1);
    if (!pa || !pb) { free(pa); free(pb); return false; }
    memcpy(pa, a, la + 1);
    memcpy(pb, b, lb + 1);
    (*arr)[*n].fa = pa;
    (*arr)[*n].en = pb;
    (*n)++;
    return true;
}

mt_model *mt_model_load(const char *path, osa_error *err) {
    FILE *f = fopen(path, "rb");
    if (!f) { osa_err_set(err, "cannot open %s", path); return NULL; }
    mt_hdr h;
    if (fread(&h, sizeof h, 1, f) != 1 || h.magic != MT_MAGIC) {
        fclose(f);
        osa_err_set(err, "not a valid .osmt model");
        return NULL;
    }
    if (h.version != MT_VERSION) {
        fclose(f);
        osa_err_set(err, "unsupported .osmt version %u", h.version);
        return NULL;
    }
    if (h.n_pairs < 0 || h.n_gloss < 0 || h.n_pairs > 2000000 || h.n_gloss > 2000000) {
        fclose(f);
        osa_err_set(err, "implausible lexicon size");
        return NULL;
    }
    mt_model *m = model_alloc();
    if (!m) { fclose(f); osa_err_set(err, "out of memory"); return NULL; }
    h.id[sizeof h.id - 1] = 0;
    h.name[sizeof h.name - 1] = 0;
    h.license[sizeof h.license - 1] = 0;
    h.source[sizeof h.source - 1] = 0;
    h.backend[sizeof h.backend - 1] = 0;
    h.src_tag[sizeof h.src_tag - 1] = 0;
    h.dst_tag[sizeof h.dst_tag - 1] = 0;
    osa_strlcpy(m->id, h.id, sizeof m->id);
    osa_strlcpy(m->name, h.name, sizeof m->name);
    osa_strlcpy(m->license, h.license, sizeof m->license);
    osa_strlcpy(m->source, h.source, sizeof m->source);
    osa_strlcpy(m->backend, h.backend[0] ? h.backend : "osmt", sizeof m->backend);
    osa_strlcpy(m->src_tag, h.src_tag, sizeof m->src_tag);
    osa_strlcpy(m->dst_tag, h.dst_tag, sizeof m->dst_tag);
    if (!m->backend[0]) osa_strlcpy(m->backend, "osmt", sizeof m->backend);
    char a[256], b[256];
    bool ok = true;
    for (int i = 0; ok && i < h.n_pairs; i++) {
        ok = read_str14(f, a, sizeof a) && read_str14(f, b, sizeof b);
        if (ok) ok = add_pair(&m->pairs, &m->n_pairs, &m->cap_pairs, a, b);
    }
    for (int i = 0; ok && i < h.n_gloss; i++) {
        ok = read_str14(f, a, sizeof a) && read_str14(f, b, sizeof b);
        if (ok) ok = add_pair(&m->gloss, &m->n_gloss, &m->cap_gloss, a, b);
    }
    fclose(f);
    if (!ok) { mt_model_free(m); osa_err_set(err, "corrupt model payload"); return NULL; }
    osa_err_ok(err);
    return m;
}

/* =====================================================================
 *  Detection and translation
 * ===================================================================== */
mt_lang mt_detect_lang(const char *text) {
    if (!text) return MT_FA;
    int fa = 0, en = 0;
    const char *p = text;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (is_persian_cp(cp)) fa++;
        else if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) en++;
    }
    if (fa == 0 && en == 0) return MT_FA;
    return fa >= en ? MT_FA : MT_EN;
}

const char *mt_lang_tag(mt_lang l) {
    switch (l) {
        case MT_FA: return "fa";
        case MT_EN: return "en";
        default:    return "";
    }
}

const char *mt_backend_name(const char *backend) {
    if (!backend) return "";
    if (osa_streq(backend, "phrasebook")) return "Builtin phrasebook";
    if (osa_streq(backend, "osmt")) return "Local model (.osmt)";
    return backend;
}

/* Exact sentence lookup, in the requested direction. */
static const char *lookup_sentence(const mt_model *m, const char *norm, mt_lang src) {
    if (m && m->pairs) {
        for (int i = 0; i < m->n_pairs; i++) {
            const char *from = src == MT_FA ? m->pairs[i].fa : m->pairs[i].en;
            const char *to = src == MT_FA ? m->pairs[i].en : m->pairs[i].fa;
            if (osa_streq(from, norm)) return to;
        }
    }
    for (size_t i = 0; i < sizeof SENTENCES / sizeof SENTENCES[0]; i++) {
        const char *from = src == MT_FA ? SENTENCES[i].fa : SENTENCES[i].en;
        const char *to = src == MT_FA ? SENTENCES[i].en : SENTENCES[i].fa;
        if (osa_streq(from, norm)) return to;
    }
    return NULL;
}

/* Longest-prefix word gloss. Returns the number of bytes consumed. */
static int gloss_word(const mt_model *m, const char *norm, mt_lang src,
                      const char **out_en, char *buf, size_t cap) {
    size_t best_len = 0;
    const char *best_to = NULL;
    /* custom lexicon first (user-supplied packs override the builtin) */
    for (int pass = 0; pass < 2; pass++) {
        const phrase_pair *tbl = NULL;
        int n = 0;
        if (pass == 0 && m && m->gloss) { tbl = m->gloss; n = m->n_gloss; }
        else if (pass == 1) { tbl = WORDS; n = (int)(sizeof WORDS / sizeof WORDS[0]); }
        if (!tbl) continue;
        for (int i = 0; i < n; i++) {
            const char *from = src == MT_FA ? tbl[i].fa : tbl[i].en;
            size_t fl = strlen(from);
            if (fl == 0 || fl <= best_len) continue;
            if (src == MT_FA) {
                if (strncmp(norm, from, fl) != 0) continue;
            } else {
                /* Latin source: compare case-insensitively */
                int same = 1;
                for (size_t k = 0; k < fl; k++) {
                    char x = norm[k], y = from[k];
                    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
                    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
                    if (x != y) { same = 0; break; }
                }
                if (!same) continue;
            }
            if (norm[fl] != 0 && norm[fl] != ' ') continue;
            best_len = fl;
            best_to = src == MT_FA ? tbl[i].en : tbl[i].fa;
        }
        if (best_len) break;      /* a custom hit wins outright */
    }
    if (!best_len || !best_to) return 0;
    osa_strlcpy(buf, best_to, cap);
    (void)out_en;
    return (int)best_len;
}

double mt_score(const char *reference, const char *hypothesis) {
    if (!reference || !hypothesis) return 0.0;
    /* tokenise on whitespace */
    char ra[2048], ha[2048];
    osa_strlcpy(ra, reference, sizeof ra);
    osa_strlcpy(ha, hypothesis, sizeof ha);
    char *rt[256], *ht[256];
    int nr = 0, nh = 0;
    for (char *s = strtok(ra, " \t"); s && nr < 256; s = strtok(NULL, " \t")) rt[nr++] = s;
    for (char *s = strtok(ha, " \t"); s && nh < 256; s = strtok(NULL, " \t")) ht[nh++] = s;
    if (nr == 0) return nh == 0 ? 1.0 : 0.0;
    int used[256] = {0};
    int match = 0;
    for (int i = 0; i < nh; i++) {
        for (int j = 0; j < nr; j++) {
            if (!used[j] && osa_streq(ht[i], rt[j])) { used[j] = 1; match++; break; }
        }
    }
    double prec = nh ? (double)match / (double)nh : 0.0;
    double rec = (double)match / (double)nr;
    if (prec + rec <= 0.0) return 0.0;
    return 2.0 * prec * rec / (prec + rec);
}

bool mt_translate(const mt_model *m, const char *text, mt_lang src,
                  mt_result *out, osa_error *err) {
    if (!text || !out) { osa_err_set(err, "bad arguments"); return false; }
    clock_t t0 = clock();
    memset(out, 0, sizeof *out);
    if (src == MT_AUTO) src = mt_detect_lang(text);
    out->src_lang = src;
    out->dst_lang = src == MT_FA ? MT_EN : MT_FA;
    osa_strlcpy(out->backend, m ? m->backend : "phrasebook", sizeof out->backend);
    osa_strlcpy(out->model_id, m ? m->id : "", sizeof out->model_id);

    char norm[MT_MAX_TEXT];
    if (src == MT_FA) normalize_fa(text, norm, sizeof norm);
    else take_trim(text, text + strlen(text), norm, sizeof norm);
    if (!norm[0]) { osa_err_set(err, "empty input"); return false; }

    /* 1. whole-sentence phrasebook hit */
    const char *hit = lookup_sentence(m, norm, src);
    if (hit) {
        osa_strlcpy(out->text, hit, sizeof out->text);
        out->n_pieces = 1;
        osa_strlcpy(out->pieces[0].src, norm, sizeof out->pieces[0].src);
        osa_strlcpy(out->pieces[0].dst, hit, sizeof out->pieces[0].dst);
        out->pieces[0].src_start = 0;
        out->pieces[0].src_end = (int)strlen(text);
        out->pieces[0].confidence = 0.98f;
        out->confidence = 0.98f;
        out->elapsed_ms = 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
        osa_err_ok(err);
        return true;
    }

    /* 2. word-level gloss, longest match first, preserving unknown words so the
     *    user sees exactly what the engine could not translate */
    size_t o = 0;
    out->text[0] = 0;
    const char *p = norm;
    float conf_sum = 0;
    int conf_n = 0;
    while (*p && out->n_pieces < MT_MAX_SEGMENTS) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *chain = *p ? strchr(p, ' ') : NULL;
        size_t chain_len = chain ? (size_t)(chain - p) : strlen(p);
        /* try the longest prefix of the remaining text, then backtrack to the
         * current token boundary */
        char dst[256];
        int consumed = gloss_word(m, p, src, NULL, dst, sizeof dst);
        if (consumed == 0 || (size_t)consumed > chain_len) {
            /* single token: try just the token, else pass it through */
            char tok[256];
            size_t tl = chain_len < sizeof tok - 1 ? chain_len : sizeof tok - 1;
            memcpy(tok, p, tl); tok[tl] = 0;
            int c2 = gloss_word(m, tok, src, NULL, dst, sizeof dst);
            if (c2 > 0 && (size_t)c2 == tl) { consumed = (int)tl; }
            else {
                /* Unknown token: keep Latin words only in Latin output and
                 * Persian words only in Persian output, so the result is not a
                 * script salad. The piece still records the source text. */
                bool target_is_fa = (out->dst_lang == MT_FA);
                bool latin = false;
                for (const char *q = tok; *q; q++)
                    if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')) { latin = true; break; }
                if (target_is_fa == latin) dst[0] = 0;
                else osa_strlcpy(dst, tok, sizeof dst);
                consumed = (int)tl;
            }
            if (consumed == 0) break;
        }
        /* piece record */
        mt_piece *pc = &out->pieces[out->n_pieces++];
        pc->src_start = (int)(p - norm);
        pc->src_end = (int)(p - norm) + consumed;
        size_t sl = (size_t)consumed < sizeof pc->src - 1 ? (size_t)consumed : sizeof pc->src - 1;
        memcpy(pc->src, p, sl); pc->src[sl] = 0;
        osa_strlcpy(pc->dst, dst, sizeof pc->dst);
        pc->confidence = consumed > 0 && dst[0] ? 0.62f : 0.30f;
        conf_sum += pc->confidence;
        conf_n++;
        if (dst[0]) {
            if (o) { if (o + 1 < sizeof out->text) out->text[o++] = ' '; }
            size_t dl = strlen(dst);
            if (o + dl < sizeof out->text) { memcpy(out->text + o, dst, dl); o += dl; out->text[o] = 0; }
        }
        p += consumed;
        while (*p == ' ') p++;
    }
    if (out->n_pieces == 0) { osa_err_set(err, "nothing to translate"); return false; }
    out->confidence = conf_n ? conf_sum / (float)conf_n : 0.0f;
    out->elapsed_ms = 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
    osa_err_ok(err);
    return true;
}
