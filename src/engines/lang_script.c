/* =====================================================================
 *  OpenSpeech AI — language names and scripts, and the "did it translate?"
 *  check used by the translation worker.
 * ===================================================================== */
#include "lang_script.h"
#include "../core/osai_common.h"
#include <string.h>
#include <stdlib.h>

/* Local copy instead of linking the whole of osai_common.c into the worker: the
 * worker must stay small, and this is the only helper it needs. */
static bool starts_with(const char *s, const char *p) {
    return s && p && strncmp(s, p, strlen(p)) == 0;
}

typedef struct lang_row {
    const char *tag;      /* ISO 639-1, or 639-1 + "-" + region */
    const char *english;  /* used in the model prompt */
    const char *persian;  /* used in the UI and in Persian hints */
    osa_script  script;
} lang_row;

static const lang_row LANGS[] = {
    {"af", "Afrikaans",  "آفریکانس",   OSA_SCRIPT_LATIN},
    {"am", "Amharic",    "امهری",      OSA_SCRIPT_ETHIOPIC},
    {"ar", "Arabic",     "عربی",       OSA_SCRIPT_ARABIC},
    {"az", "Azerbaijani","آذری",       OSA_SCRIPT_LATIN},
    {"be", "Belarusian", "بلاروسی",    OSA_SCRIPT_CYRILLIC},
    {"bg", "Bulgarian",  "بلغاری",     OSA_SCRIPT_CYRILLIC},
    {"bn", "Bengali",    "بنگالی",     OSA_SCRIPT_BENGALI},
    {"bs", "Bosnian",    "بوسنیایی",   OSA_SCRIPT_LATIN},
    {"ca", "Catalan",    "کاتالان",    OSA_SCRIPT_LATIN},
    {"cs", "Czech",      "چکی",        OSA_SCRIPT_LATIN},
    {"cy", "Welsh",      "ولش",        OSA_SCRIPT_LATIN},
    {"da", "Danish",     "دانمارکی",   OSA_SCRIPT_LATIN},
    {"de", "German",     "آلمانی",     OSA_SCRIPT_LATIN},
    {"el", "Greek",      "یونانی",     OSA_SCRIPT_GREEK},
    {"en", "English",    "انگلیسی",    OSA_SCRIPT_LATIN},
    {"es", "Spanish",    "اسپانیایی",  OSA_SCRIPT_LATIN},
    {"et", "Estonian",   "استونیایی",  OSA_SCRIPT_LATIN},
    {"eu", "Basque",     "باسکی",      OSA_SCRIPT_LATIN},
    {"fa", "Persian",    "فارسی",      OSA_SCRIPT_ARABIC},
    {"fi", "Finnish",    "فنلاندی",    OSA_SCRIPT_LATIN},
    {"fr", "French",     "فرانسوی",    OSA_SCRIPT_LATIN},
    {"ga", "Irish",      "ایرلندی",    OSA_SCRIPT_LATIN},
    {"gl", "Galician",   "گالیسیایی",  OSA_SCRIPT_LATIN},
    {"gu", "Gujarati",   "گجراتی",     OSA_SCRIPT_GUJARATI},
    {"he", "Hebrew",     "عبری",       OSA_SCRIPT_HEBREW},
    {"hi", "Hindi",      "هندی",       OSA_SCRIPT_DEVANAGARI},
    {"hr", "Croatian",   "کروات",      OSA_SCRIPT_LATIN},
    {"hu", "Hungarian",  "مجاری",      OSA_SCRIPT_LATIN},
    {"hy", "Armenian",   "ارمنی",      OSA_SCRIPT_ARMENIAN},
    {"id", "Indonesian", "اندونزیایی", OSA_SCRIPT_LATIN},
    {"is", "Icelandic",  "ایسلندی",    OSA_SCRIPT_LATIN},
    {"it", "Italian",    "ایتالیایی",  OSA_SCRIPT_LATIN},
    {"ja", "Japanese",   "ژاپنی",      OSA_SCRIPT_KANA},
    {"ka", "Georgian",   "گرجی",       OSA_SCRIPT_GEORGIAN},
    {"kk", "Kazakh",     "قزاقی",      OSA_SCRIPT_CYRILLIC},
    {"km", "Khmer",      "خمر",        OSA_SCRIPT_KHMER},
    {"kn", "Kannada",    "کانادا",     OSA_SCRIPT_KANNADA},
    {"ko", "Korean",     "کره‌ای",     OSA_SCRIPT_HANGUL},
    {"lo", "Lao",        "لائو",       OSA_SCRIPT_LAO},
    {"lt", "Lithuanian", "لیتوانیایی", OSA_SCRIPT_LATIN},
    {"lv", "Latvian",    "لتونیایی",   OSA_SCRIPT_LATIN},
    {"mk", "Macedonian", "مقدونی",     OSA_SCRIPT_CYRILLIC},
    {"ml", "Malayalam",  "مالایالام",  OSA_SCRIPT_MALAYALAM},
    {"mn", "Mongolian",  "مغولی",      OSA_SCRIPT_CYRILLIC},
    {"mr", "Marathi",    "مراتی",      OSA_SCRIPT_DEVANAGARI},
    {"ms", "Malay",      "مالایی",     OSA_SCRIPT_LATIN},
    {"my", "Burmese",    "برمه‌ای",    OSA_SCRIPT_MYANMAR},
    {"nb", "Norwegian",  "نروژی",      OSA_SCRIPT_LATIN},
    {"ne", "Nepali",     "نپالی",      OSA_SCRIPT_DEVANAGARI},
    {"nl", "Dutch",      "هلندی",      OSA_SCRIPT_LATIN},
    {"pa", "Punjabi",    "پنجابی",     OSA_SCRIPT_GURMUKHI},
    {"pl", "Polish",     "لهستانی",    OSA_SCRIPT_LATIN},
    {"ps", "Pashto",     "پشتو",       OSA_SCRIPT_ARABIC},
    {"pt", "Portuguese", "پرتغالی",    OSA_SCRIPT_LATIN},
    {"ro", "Romanian",   "رومانیایی",  OSA_SCRIPT_LATIN},
    {"ru", "Russian",    "روسی",       OSA_SCRIPT_CYRILLIC},
    {"si", "Sinhala",    "سینهالی",    OSA_SCRIPT_SINHALA},
    {"sk", "Slovak",     "اسلواکی",    OSA_SCRIPT_LATIN},
    {"sl", "Slovenian",  "اسلوونیایی", OSA_SCRIPT_LATIN},
    {"sq", "Albanian",   "آلبانیایی",  OSA_SCRIPT_LATIN},
    {"sr", "Serbian",    "صربی",       OSA_SCRIPT_CYRILLIC},
    {"sv", "Swedish",    "سوئدی",      OSA_SCRIPT_LATIN},
    {"sw", "Swahili",    "سواحیلی",    OSA_SCRIPT_LATIN},
    {"ta", "Tamil",      "تامیل",      OSA_SCRIPT_TAMIL},
    {"te", "Telugu",     "تلوگو",      OSA_SCRIPT_TELUGU},
    {"th", "Thai",       "تایلندی",    OSA_SCRIPT_THAI},
    {"tl", "Tagalog",    "تاگالوگ",    OSA_SCRIPT_LATIN},
    {"tr", "Turkish",    "ترکی",       OSA_SCRIPT_LATIN},
    {"uk", "Ukrainian",  "اوکراینی",   OSA_SCRIPT_CYRILLIC},
    {"ur", "Urdu",       "اردو",       OSA_SCRIPT_ARABIC},
    {"uz", "Uzbek",      "ازبکی",      OSA_SCRIPT_LATIN},
    {"vi", "Vietnamese", "ویتنامی",    OSA_SCRIPT_LATIN},
    {"yi", "Yiddish",    "ییدیش",      OSA_SCRIPT_HEBREW},
    {"zh", "Chinese",    "چینی",       OSA_SCRIPT_HAN},
    {"zu", "Zulu",       "زولو",       OSA_SCRIPT_LATIN},
    {NULL, NULL, NULL, OSA_SCRIPT_NONE}
};

/* The row for a tag, matching on the language subtag only ("en-GB" -> "en"). */
static const lang_row *find_lang(const char *tag) {
    if (!tag || !*tag) return NULL;
    for (int i = 0; LANGS[i].tag; i++) {
        size_t n = strlen(LANGS[i].tag);
        if (starts_with(tag, LANGS[i].tag) && (tag[n] == 0 || tag[n] == '-' || tag[n] == '_'))
            return &LANGS[i];
    }
    return NULL;
}

const char *osa_language_name(const char *tag) {
    const lang_row *r = find_lang(tag);
    return r ? r->english : (tag && *tag ? tag : "the target language");
}

osa_script osa_script_for_tag(const char *tag) {
    const lang_row *r = find_lang(tag);
    return r ? r->script : OSA_SCRIPT_LATIN;
}

/* Script of one code point, for the counting below. */
static osa_script script_of(uint32_t cp) {
    if (cp < 0x0250) return OSA_SCRIPT_LATIN;              /* ASCII + Latin-1/ext */
    if (cp >= 0x0370 && cp <= 0x03FF) return OSA_SCRIPT_GREEK;
    if (cp >= 0x0400 && cp <= 0x052F) return OSA_SCRIPT_CYRILLIC;
    if (cp >= 0x0530 && cp <= 0x058F) return OSA_SCRIPT_ARMENIAN;
    if (cp >= 0x0590 && cp <= 0x05FF) return OSA_SCRIPT_HEBREW;
    if (cp >= 0x0600 && cp <= 0x06FF) return OSA_SCRIPT_ARABIC;
    if (cp >= 0x0750 && cp <= 0x077F) return OSA_SCRIPT_ARABIC;
    if (cp >= 0x0900 && cp <= 0x097F) return OSA_SCRIPT_DEVANAGARI;
    if (cp >= 0x0980 && cp <= 0x09FF) return OSA_SCRIPT_BENGALI;
    if (cp >= 0x0A00 && cp <= 0x0A7F) return OSA_SCRIPT_GURMUKHI;
    if (cp >= 0x0A80 && cp <= 0x0AFF) return OSA_SCRIPT_GUJARATI;
    if (cp >= 0x0B80 && cp <= 0x0BFF) return OSA_SCRIPT_TAMIL;
    if (cp >= 0x0C00 && cp <= 0x0C7F) return OSA_SCRIPT_TELUGU;
    if (cp >= 0x0C80 && cp <= 0x0CFF) return OSA_SCRIPT_KANNADA;
    if (cp >= 0x0D00 && cp <= 0x0D7F) return OSA_SCRIPT_MALAYALAM;
    if (cp >= 0x0D80 && cp <= 0x0DFF) return OSA_SCRIPT_SINHALA;
    if (cp >= 0x0E00 && cp <= 0x0E7F) return OSA_SCRIPT_THAI;
    if (cp >= 0x0E80 && cp <= 0x0EFF) return OSA_SCRIPT_LAO;
    if (cp >= 0x0F00 && cp <= 0x0FFF) return OSA_SCRIPT_TIBETAN;
    if (cp >= 0x1000 && cp <= 0x109F) return OSA_SCRIPT_MYANMAR;
    if (cp >= 0x10A0 && cp <= 0x10FF) return OSA_SCRIPT_GEORGIAN;
    if (cp >= 0x1200 && cp <= 0x137F) return OSA_SCRIPT_ETHIOPIC;
    if (cp >= 0x1780 && cp <= 0x17FF) return OSA_SCRIPT_KHMER;
    if (cp >= 0x1E00 && cp <= 0x1EFF) return OSA_SCRIPT_LATIN;   /* Latin ext. add. */
    if (cp >= 0x2E80 && cp <= 0x9FFF) return OSA_SCRIPT_HAN;
    if (cp >= 0x3040 && cp <= 0x30FF) return OSA_SCRIPT_KANA;
    if (cp >= 0xAC00 && cp <= 0xD7AF) return OSA_SCRIPT_HANGUL;
    if (cp >= 0xF900 && cp <= 0xFAFF) return OSA_SCRIPT_HAN;
    return OSA_SCRIPT_NONE;   /* punctuation, digits, emoji: not a letter */
}

/* Equal after trimming spaces and punctuation: "OpenSpeech AI." == "OpenSpeech AI". */
static bool same_text(const char *a, const char *b) {
    if (!a || !b) return false;
    for (;;) {
        /* spaces and trailing punctuation are not a difference */
        while (*a && ((unsigned char)*a <= ' ' || strchr(".,!?;:", *a))) a++;
        while (*b && ((unsigned char)*b <= ' ' || strchr(".,!?;:", *b))) b++;
        /* One side ending early means the texts differ: "hello" is not
         * "Hello! How can I assist you today?". */
        if (!*a || !*b) return *a == 0 && *b == 0;
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++; b++;
    }
}

/* Counts letters per script. Returns the total number of letter code points. */
static int count_scripts(const char *text, int counts[], int n_scripts) {
    int total = 0;
    if (!text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ) {
        uint32_t cp;
        if (*p < 0x80) { cp = *p; p += 1; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        } else { p += 1; continue; }
        if (cp >= '0' && cp <= '9') continue;          /* digits say nothing */
        osa_script s = script_of(cp);
        if (s == OSA_SCRIPT_NONE) continue;
        if (cp >= 0x0300 && cp <= 0x036F) continue;    /* combining marks */
        if ((int)s < n_scripts) counts[s]++;
        total++;
    }
    return total;
}

float osa_script_share(const char *text, osa_script s) {
    int counts[32] = {0};
    int total = count_scripts(text, counts, 32);
    if (total <= 0 || (int)s >= 32) return 0.0f;
    /* Japanese mixes kana with Han; a target of "ja" accepts either. */
    int hit = counts[s];
    if (s == OSA_SCRIPT_KANA) hit += counts[OSA_SCRIPT_HAN];
    if (s == OSA_SCRIPT_HAN) hit += counts[OSA_SCRIPT_KANA];
    return (float)hit / (float)total;
}

bool osa_text_in_script(const char *text, osa_script s) {
    int counts[32] = {0};
    int total = count_scripts(text, counts, 32);
    if (total <= 0) return false;
    int hit = ((int)s < 32) ? counts[s] : 0;
    if (s == OSA_SCRIPT_KANA) hit += counts[OSA_SCRIPT_HAN];
    if (s == OSA_SCRIPT_HAN) hit += counts[OSA_SCRIPT_KANA];
    return hit > 0;
}

bool osa_translation_in_target_script(const char *output, const char *dst_tag,
                                      const char *input, const char *src_tag) {
    if (!output || !*output) return false;
    int counts[32] = {0};
    int total = count_scripts(output, counts, 32);
    /* A translation of "42" is "42". Without letters there is nothing to check,
     * and refusing it would be wrong. */
    if (total == 0) return true;

    osa_script want = osa_script_for_tag(dst_tag);
    int hit = counts[want];
    if (want == OSA_SCRIPT_KANA) hit += counts[OSA_SCRIPT_HAN];
    if (want == OSA_SCRIPT_HAN) hit += counts[OSA_SCRIPT_KANA];
    if (hit * 2 >= total) return true;      /* at least half in the right script */

    /* Persian script is shared by Persian, Arabic, Urdu and Pashto. Writing a
     * Persian sentence in Arabic letters is the model doing what it was asked; it
     * is not a foreign-script answer. The same goes for the other shared cases. */
    if (want == OSA_SCRIPT_ARABIC) {
        int arabic = counts[OSA_SCRIPT_ARABIC];
        if (arabic * 2 >= total) return true;
    }

    /* A proper noun, a product name or a code stays as it is: returning the input
     * is a legitimate answer there. Anything else in the wrong script is the model
     * talking to the user, not translating. */
    if (same_text(output, input)) return true;
    return false;
}
