/* =====================================================================
 *  OpenSpeech AI — آوانویسی: any supported language -> readable Persian
 *
 *  The translation screen shows a "say it like this" line under the output.
 *  It used to understand English only, and its diacritics were placed by rule of
 *  thumb — often on a letter that carries no vowel in Persian, which is exactly
 *  what makes a machine transliteration look wrong to a native reader.
 *
 *  This module reads the *script* rather than one language, and writes what a
 *  Persian speaker would say:
 *
 *    de  danke schön      ->  دانکه شِن
 *    ru  спасибо          ->  سپاسیبا
 *    el  καλημέρα         ->  کالیمِرا
 *    ar  شكرا             ->  شُکراً
 *    es  gracias          ->  گراسیاس
 *    fr  bonjour          ->  بونژور
 *
 *  Three rules make the diacritics right, and they are enforced in one place by
 *  the emitter (emit_vowel), not by hoping each table is careful:
 *
 *   1. a short vowel is a diacritic on the *preceding consonant*; if there is no
 *      consonant to carry it (word-initial, or right after a long vowel) it gets
 *      a carrier letter;
 *   2. a diacritic is never written on a long-vowel letter (ا/ی/و) and never
 *      twice on the same consonant;
 *   3. a long vowel is a letter (ا/ی/و), so "aa" is ا and not اَ.
 *
 *  Scripts with a reader here: Latin (per language, plus a generic Latin
 *  fallback), Cyrillic, Greek, Arabic and Hebrew. Scripts that cannot be read
 *  letter-by-letter without a dictionary (Han, Kana, Hangul, Devanagari, Thai,
 *  Khmer, Lao, Myanmar, Bengali, Tamil, Telugu, Kannada, Malayalam, Gujarati,
 *  Sinhala, Georgian, Armenian, Ethiopic) are reported as unsupported instead of
 *  being turned into nonsense, and the caller shows that sentence to the user.
 *
 *  No allocation beyond the caller's buffer, no model file, no network.
 * ===================================================================== */
#include "mt.h"
#include "lang_script.h"
#include "pronounce_fold.h"
#include "../core/osai_common.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- Persian letters ---------------------------------------------- */
#define L_ALEF   "ا"
#define L_BE     "ب"
#define L_PE     "پ"
#define L_TE     "ت"
#define L_SE     "ث"
#define L_JIM    "ج"
#define L_CHE    "چ"
#define L_HE     "ح"
#define L_KHE    "خ"
#define L_DAL    "د"
#define L_ZAL    "ذ"
#define L_RE     "ر"
#define L_ZE     "ز"
#define L_ZHE    "ژ"
#define L_SIN    "س"
#define L_SHIN   "ش"
#define L_SAD    "ص"
#define L_ZAD    "ض"
#define L_TA     "ط"
#define L_ZA     "ظ"
#define L_EYN    "ع"
#define L_GHEYN  "غ"
#define L_FE     "ف"
#define L_QAF    "ق"
#define L_KAF    "ک"
#define L_GAF    "گ"
#define L_LAM    "ل"
#define L_MIM    "م"
#define L_NUN    "ن"
#define L_VAV    "و"
#define L_HEH    "ه"
#define L_YE     "ی"

/* Diacritics are combining marks, so each one attaches to the letter just
 * written. */
#define D_FATH   "\u064E"   /* َ  a    */
#define D_KASR   "\u0650"   /* ِ  e/i  */
#define D_DAMM   "\u064F"   /* ُ  o/u  */
#define D_TANVIN "\u064B"   /* ً  -an  */

/* Ids for the three short vowels, so the emitter can compare them without
 * looking at UTF-8 bytes. */
enum { DIA_FATH = 1, DIA_KASR = 2, DIA_DAMM = 3 };

static int dia_id(const char *dia) {
    unsigned char a = (unsigned char)dia[0], b = (unsigned char)dia[1];
    if (a != 0xD9) return 0;
    return b == 0x8E ? DIA_FATH : b == 0x90 ? DIA_KASR : DIA_DAMM;
}
static const char *long_for(int id) {
    return id == DIA_FATH ? L_ALEF : id == DIA_KASR ? L_YE : L_VAV;
}

/* ---- consonants and vowels ---------------------------------------- */
enum cons {
    C_B = 0, C_P, C_T, C_S, C_J, C_CH, C_H, C_KH, C_D, C_Z, C_R, C_ZH, C_SH,
    C_F, C_K, C_G, C_L, C_M, C_N, C_V, C_Y, C_TH, C_DH, C_NG, C_EYN, C_COUNT
};

static const char *CONS_CHAR[C_COUNT] = {
    L_BE, L_PE, L_TE, L_SIN, L_JIM, L_CHE, L_HEH, L_KHE, L_DAL, L_ZE, L_RE, L_ZHE,
    L_SHIN, L_FE, L_KAF, L_GAF, L_LAM, L_MIM, L_NUN, L_VAV, L_YE, L_SE, L_DAL,
    "\u0646\u06AF", L_EYN
};

enum vow { V_A = 0, V_E, V_I, V_O, V_U, V_AA, V_EE, V_OO, V_AY, V_OW, V_OH,
           V_YA, V_YE, V_YO, V_YU, V_YI, V_WA, V_EI };

/* ---- the emitter --------------------------------------------------- */
typedef struct pron_out {
    char  *buf;
    size_t cap, n;
    int    last_class;      /* 0 none, 1 consonant, 2 long vowel, 3 other */
    int    last_diac;       /* has this consonant already got a diacritic? */
    int    last_diac_id;    /* which one, so "aa" can become ا */
    int    prev_vowel;      /* the previous rule was a vowel rule */
    int    full;            /* 1: a/i/o/u are full vowels (Cyrillic, Greek, Arabic) */
} pron_out;

static void put(pron_out *o, const char *s) {
    if (!s) return;
    while (*s && o->n + 1 < o->cap) o->buf[o->n++] = *s++;
    o->buf[o->n] = 0;
}

static void put_c(pron_out *o, const char *ch, int cls) {
    put(o, ch);
    o->last_class = cls;
    o->last_diac = 0;
}

static void emit_cons(pron_out *o, int c) { put_c(o, CONS_CHAR[c], 1); o->prev_vowel = 0; }
static void emit_long(pron_out *o, const char *v) { put_c(o, v, 2); o->prev_vowel = 1; }

/* One short vowel: a diacritic on the consonant just written, or a carrier when
 * there is no consonant (rule 1). */
static void emit_long(pron_out *o, const char *v);

static void emit_short(pron_out *o, const char *dia) {
    int id = dia_id(dia);
    if (o->last_class == 1) {
        if (o->last_diac) {
            /* The same vowel twice is one long vowel, not two diacritics:
             * "päivää" ends in ا, not in a doubled fatha. This only applies to
             * vowels written next to each other — "grazie" keeps its kasra
             * because a consonant stands between the a and the i. */
            if (o->prev_vowel && o->last_diac_id != id) {
                /* Two different vowels written together are a diphthong, not a
                 * replacement: "ai" is اَی, not اَ. */
                emit_long(o, long_for(id));
                return;
            }
            if (o->last_diac_id == id && o->prev_vowel) {
                size_t dl = strlen(dia);
                if (o->n >= dl) o->n -= dl;
                o->buf[o->n] = 0;
                emit_long(o, long_for(id));
            }
            return;
        }
        put(o, dia);
        o->last_diac = 1;
        o->last_diac_id = id;
        return;
    }
    if (o->last_class == 2) {
        /* A vowel right after a long vowel has no consonant to sit on. Dropping
         * it would swallow a syllable ("gra-cia-s"), so it becomes a letter —
         * except when it simply repeats the long vowel already written. */
        const char *v = long_for(id);
        if (o->n == 0 || o->buf[o->n - 1] != (char)v[0] || o->n < 2) put_c(o, v, 2);
        return;
    }
    put_c(o, L_ALEF, 1);                          /* carrier */
    put(o, dia);
    o->last_diac = 1;
    o->last_diac_id = id;
}

static void emit_vowel(pron_out *o, int v) {
    int was = o->prev_vowel;
    o->prev_vowel = 1;
    (void)was;
    /* In a script whose vowels are full letters (Russian спасибо -> سپاسیبو,
     * Greek καλημέρα -> کالیمِرا), writing a kasra instead of ی would be a
     * wrong reading, so a/i/o/u become long letters there. "e" stays a kasra:
     * Persian has no long e. */
    if (o->full) {
        switch (v) {
            case V_A: emit_long(o, L_ALEF); return;
            case V_I: emit_long(o, L_YE);   return;
            case V_O:
            case V_U: emit_long(o, L_VAV);  return;
            default: break;
        }
    }
    switch (v) {
        case V_A:  emit_short(o, D_FATH); break;
        case V_E:
        case V_I:  emit_short(o, D_KASR); break;
        case V_O:
        case V_U:  emit_short(o, D_DAMM); break;
        case V_AA: emit_long(o, L_ALEF); break;
        case V_EE: emit_long(o, L_YE);   break;
        case V_OO:
        case V_OH: emit_long(o, L_VAV);  break;
        case V_AY: emit_short(o, D_FATH); emit_cons(o, C_Y); break;
        case V_EI: emit_short(o, D_KASR); emit_cons(o, C_Y); break;  /* «اِی» */
        case V_OW: emit_short(o, D_DAMM); emit_cons(o, C_V); break;
        case V_WA: emit_cons(o, C_V); emit_long(o, L_ALEF); break;  /* oi  -> وا */

        /* я/е/ё/ю are a y-sound plus a vowel: «یَ»، «یِ»، «یُ»، «یو». Writing only
         * ی lost the vowel (спасибо for "я не понимаю" came out «یِ نیِ ...»). */
        case V_YA: emit_cons(o, C_Y); emit_short(o, D_FATH); break;
        case V_YE: emit_cons(o, C_Y); emit_short(o, D_KASR); break;
        case V_YI: emit_cons(o, C_Y); emit_long(o, L_YE);   break;
        case V_YO: emit_cons(o, C_Y); emit_short(o, D_DAMM); break;
        case V_YU: emit_cons(o, C_Y); emit_long(o, L_VAV);  break;
        default: break;
    }
}

/* ---- rule tables --------------------------------------------------- */
enum rkind {
    R_SILENT = 0,   /* written, not spoken */
    R_CONS,         /* a consonant letter */
    R_VOWEL,        /* a vowel: a diacritic, or a letter in a full-vowel script */
    R_LIT,          /* a literal Persian string */
    R_FINAL,        /* a vowel, but only at the end of a word (val 0 = silent) */
    R_VOWEL_INITIAL,/* "y + vowel" at the start of a word, plain vowel inside it:
                     * «я» is «یا» in "я не понимаю" but «а» in "тям" */
    R_CONS_FINAL,   /* a consonant, only at the end of a word */
    R_INITIAL,      /* a consonant, only at the start of a word ("y" in "yes") */
    R_NASAL         /* a French nasal vowel: vowel + light n */
};

typedef struct pron_rule {
    const char *from;
    int         kind;
    int         val;
    const char *lit;
} pron_rule;

#define RULE_END { NULL, 0, 0, NULL }

static void apply_rule(pron_out *o, const pron_rule *r) {
    switch (r->kind) {
        case R_SILENT: break;
        case R_NASAL:
            /* A French nasal vowel is the vowel plus a light n: "bonjour" is
             * بونژور, so the n stays as a letter instead of a diacritic. */
            emit_vowel(o, r->val);
            emit_cons(o, C_N);
            break;
        case R_CONS:   emit_cons(o, r->val); break;
        case R_VOWEL:  emit_vowel(o, r->val); break;
        case R_VOWEL_INITIAL: emit_vowel(o, r->val); break;
        case R_FINAL:  if (r->val) emit_vowel(o, r->val); break;  /* 0 = silent */
        case R_INITIAL:
        case R_CONS_FINAL: emit_cons(o, r->val); break;
        case R_LIT:    for (const char *p = r->lit; *p; ) {
                           /* a literal is a string of Persian letters, written
                            * as whole characters */
                           int len = ((unsigned char)*p < 0xC0) ? 1 :
                                     (((unsigned char)*p & 0xE0) == 0xC0) ? 2 : 3;
                           char tmp[5] = {0};
                           memcpy(tmp, p, (size_t)len);
                           put_c(o, tmp, 1);
                           o->prev_vowel = 0;
                           p += len;
                       }
                       break;
    }
}

/* Longest match wins, so every table lists its multi-letter entries first. */
/* Same, but a rule marked R_FINAL is skipped unless it ends the word: English
 * "y" is a consonant in "yes" and a vowel in "happy". */
static const pron_rule *match_rule_at(const pron_rule *rules, const char *at,
                                      const char *word_start, int prev_vowel) {
    for (const pron_rule *r = rules; r->from; r++) {
        size_t n = strlen(r->from);
        if (strncmp(at, r->from, n) != 0) continue;
        if ((r->kind == R_FINAL || r->kind == R_CONS_FINAL) && at[n] != 0) continue;
        if (r->kind == R_INITIAL && at != word_start) continue;
        /* "я/е/ю/ё" take their y-sound at the start of a word and after a vowel
         * ("понимаю" is پونیمایو); after a consonant they are plain vowels. */
        if (r->kind == R_VOWEL_INITIAL && at != word_start && !prev_vowel) continue;
        return r;
    }
    return NULL;
}

/* ---- Latin ---------------------------------------------------------
 * Keyed on the *unaccented* letter: read_run() folds ś/š/ș/ş onto "s" before a
 * table is consulted, so one entry covers every accented form and the tables stay
 * short. A per-language table is consulted first for the letters where the
 * languages disagree; a language with no table still reads through the base.
 */
static const pron_rule LATIN_BASE[] = {
    {"sch", R_CONS,  C_SH, NULL}, {"tch", R_CONS, C_CH, NULL},
    {"sh",  R_CONS,  C_SH, NULL}, {"ch",  R_CONS, C_CH, NULL},
    {"th",  R_CONS,  C_T,  NULL}, {"ph",  R_CONS, C_F,  NULL},
    {"kh",  R_CONS,  C_KH, NULL}, {"ng",  R_CONS, C_NG, NULL},
    {"ck",  R_CONS,  C_K,  NULL}, {"qu",  R_CONS, C_K,  NULL},
    {"a",   R_VOWEL, V_A,  NULL}, {"b",   R_CONS, C_B, NULL},
    {"c",   R_CONS,  C_K,  NULL}, {"d",   R_CONS, C_D, NULL},
    {"e",   R_VOWEL, V_E,  NULL}, {"f",   R_CONS, C_F, NULL},
    {"g",   R_CONS,  C_G,  NULL}, {"h",   R_CONS, C_H, NULL},
    {"i",   R_VOWEL, V_I,  NULL}, {"j",   R_CONS, C_J, NULL},
    {"k",   R_CONS,  C_K,  NULL}, {"l",   R_CONS, C_L, NULL},
    {"m",   R_CONS,  C_M,  NULL}, {"n",   R_CONS, C_N, NULL},
    {"o",   R_VOWEL, V_O,  NULL}, {"p",   R_CONS, C_P, NULL},
    {"q",   R_CONS,  C_K,  NULL}, {"r",   R_CONS, C_R, NULL},
    {"s",   R_CONS,  C_S,  NULL}, {"t",   R_CONS, C_T, NULL},
    {"u",   R_VOWEL, V_U,  NULL}, {"v",   R_CONS, C_V, NULL},
    {"w",   R_CONS,  C_V,  NULL}, {"x",   R_LIT,  0,   "کس"},
    {"y",   R_VOWEL, V_I,  NULL}, {"z",   R_CONS, C_Z, NULL},
    RULE_END
};

/* English. Irregular common words come from the dictionary in translit.c before
 * this reader is consulted; this table handles the spellings of everything else. */
static const pron_rule LATIN_EN[] = {
    /* Suffixes first: the "e" in "-tion"/"-ture" is not a vowel of its own. */
    {"tions", R_LIT,  0,    "شِنز"},{"tional", R_LIT, 0, "شِنال"},
    {"tion", R_LIT,   0,    "شِن"},{"sion", R_LIT,  0,   "ژِن"},
    {"ture", R_LIT,   0,    "چِر"},{"ous",  R_LIT,  0,   "ِس"},
    {"eigh", R_VOWEL, V_AY, NULL}, {"ough", R_VOWEL, V_O, NULL},
    {"igh",  R_VOWEL, V_AY, NULL},
    {"ee",  R_VOWEL,  V_EE, NULL}, {"ea",   R_VOWEL, V_EE, NULL},
    {"oo",  R_VOWEL,  V_OO, NULL}, {"ou",   R_VOWEL, V_OW, NULL},
    {"ow",  R_VOWEL,  V_OW, NULL}, {"oi",   R_VOWEL, V_AY, NULL},
    {"oy",  R_VOWEL,  V_AY, NULL}, {"ai",   R_VOWEL, V_AY, NULL},
    {"ay",  R_VOWEL,  V_AY, NULL}, {"au",   R_VOWEL, V_O,  NULL},
    {"aw",  R_VOWEL,  V_O,  NULL}, {"ie",   R_VOWEL, V_EE, NULL},
    /* English "ei"/"ay" is the long a sound, «اِی» in Persian, not «اَی». */
    {"ei",  R_VOWEL,  V_EI, NULL}, {"oa",   R_VOWEL, V_OH, NULL},
    {"wh",  R_CONS,   C_V,  NULL}, {"kn",   R_CONS,  C_N,  NULL},
    {"wr",  R_CONS,   C_R,  NULL}, {"dg",   R_CONS,  C_J,  NULL},
    /* "th" is ث in English only: thank, think, three. */
    {"th",  R_CONS,   C_TH, NULL},
    {"ing", R_LIT,   0,    "ینگ"},{"ck",   R_CONS,  C_K,  NULL}, {"qu",   R_CONS,  C_K,  NULL},
    {"ph",  R_CONS,   C_F,  NULL}, {"wh",   R_CONS,  C_V,  NULL},
    {"y",   R_INITIAL, C_Y, NULL}, {"y",   R_FINAL,  V_EE, NULL},
    {"y",   R_VOWEL,  V_I,  NULL}, {"u",    R_VOWEL, V_A,  NULL},
    RULE_END
};

static const pron_rule LATIN_DE[] = {
    {"dsch", R_CONS, C_CH, NULL},
    {"sch",  R_CONS, C_SH, NULL}, {"ch",  R_CONS, C_KH, NULL},
    {"ei",   R_VOWEL, V_AY, NULL},{"ai",  R_VOWEL, V_AY, NULL},
    {"eu",   R_VOWEL, V_OW, NULL},{"au",  R_VOWEL, V_OW, NULL},
    {"ie",   R_VOWEL, V_EE, NULL},
    /* "Kaffee", "Boot": a doubled vowel is long, which Persian writes with the
     * short reading when there is no long e. */
    {"ee",   R_VOWEL, V_E,  NULL}, {"oo",   R_VOWEL, V_O,  NULL},
    {"aa",   R_VOWEL, V_AA, NULL},
    {"ä",    R_VOWEL, V_E,  NULL}, {"ö",   R_VOWEL, V_O,  NULL},
    {"ü",    R_VOWEL, V_YU, NULL}, {"ß",   R_CONS,  C_S,  NULL},
    {"z",    R_LIT,   0,    "تس"}, {"v",   R_CONS,  C_F, NULL},
    {"w",    R_CONS,  C_V,  NULL},
    /* Final s is voiceless ("bis" is بِس); the z reading is for the middle of a
     * word, so the final rule comes first. */
    {"s",    R_CONS_FINAL, C_S, NULL}, {"s",  R_CONS,  C_Z, NULL},
    {"j",    R_VOWEL, V_YE, NULL},
    /* "danke" is دَنکه, not دَنکِه: a final e is a schwa and reads as a kasra. */
    {"e",    R_FINAL, V_E,  NULL},
    RULE_END
};

/* French: nasal vowels, a silent final e, and final consonants that the spelling
 * keeps but the voice does not. */
static const pron_rule LATIN_FR[] = {
    {"eaux", R_VOWEL, V_O,  NULL}, {"eau",  R_VOWEL, V_O,  NULL},
    {"ain",  R_NASAL, V_E,  NULL}, {"ein",  R_NASAL, V_E,  NULL},
    {"oin",  R_NASAL, V_WA, NULL}, {"ion",  R_NASAL, V_YO, NULL},
    {"an",   R_NASAL, V_A,  NULL}, {"en",   R_NASAL, V_A,  NULL},
    {"on",   R_NASAL, V_O,  NULL}, {"un",   R_NASAL, V_E,  NULL},
    {"in",   R_NASAL, V_E,  NULL},
    {"ai",   R_VOWEL, V_E,  NULL}, {"au",   R_VOWEL, V_O,  NULL},
    {"ou",   R_VOWEL, V_U,  NULL}, {"oi",   R_VOWEL, V_WA, NULL},
    {"eu",   R_VOWEL, V_O,  NULL}, {"oeu",  R_VOWEL, V_O,  NULL},
    {"ch",   R_CONS,  C_SH, NULL}, {"gn",   R_LIT,   0,    "نی"},
    {"qu",   R_CONS,  C_K,  NULL}, {"ph",   R_CONS,  C_F,  NULL},
    {"é",    R_VOWEL, V_E,  NULL}, {"è",    R_VOWEL, V_E,  NULL},
    {"ê",    R_VOWEL, V_E,  NULL}, {"à",    R_VOWEL, V_A,  NULL},
    {"â",    R_VOWEL, V_AA, NULL}, {"ô",    R_VOWEL, V_O,  NULL},
    {"û",    R_VOWEL, V_U,  NULL}, {"ù",    R_VOWEL, V_U,  NULL},
    {"î",    R_VOWEL, V_I,  NULL}, {"ï",    R_VOWEL, V_I,  NULL},
    {"ç",    R_CONS,  C_S,  NULL}, {"œ",    R_VOWEL, V_O,  NULL},
    {"h",    R_SILENT, 0,   NULL}, {"j",    R_CONS,  C_ZH, NULL},
    {"s",    R_FINAL, 0,    NULL}, {"t",    R_FINAL, 0,    NULL},
    {"d",    R_FINAL, 0,    NULL}, {"x",    R_FINAL, 0,    NULL},
    /* A final e is a schwa for a reader who has to say the word out loud. Dropping
     * it left "je ne" as «ژ ن», which is not sayable. */
    {"e",    R_FINAL, V_E,  NULL},
    RULE_END
};

static const pron_rule LATIN_ES[] = {
    {"ch",  R_CONS,  C_CH, NULL}, {"ll",  R_CONS, C_Y,  NULL},
    {"rr",  R_CONS,  C_R,  NULL}, {"qu",  R_CONS, C_K,  NULL},
    {"gu",  R_LIT,   0,    "گ"},
    /* A written accent marks the stressed vowel, which in Persian letters is a
     * long letter: "días" is دیاس, not دِاس. */
    {"á",   R_VOWEL, V_AA, NULL}, {"í",   R_VOWEL, V_EE, NULL},
    {"ó",   R_VOWEL, V_OH, NULL}, {"ú",   R_VOWEL, V_OO, NULL},
    /* ce/ci are "s" in Spain and most of Latin America. */
    {"ce",  R_LIT,   0,    "س"},  {"ci",  R_LIT,  0,    "سی"},
    {"ñ",   R_LIT,   0,    "نی"}, {"ü",   R_VOWEL, V_U,  NULL},
    {"á",   R_VOWEL, V_A,  NULL}, {"é",   R_VOWEL, V_E,  NULL},
    {"í",   R_VOWEL, V_I,  NULL}, {"ó",   R_VOWEL, V_O,  NULL},
    {"ú",   R_VOWEL, V_U,  NULL}, {"j",   R_CONS,  C_KH, NULL},
    {"h",   R_SILENT, 0,   NULL}, {"v",   R_CONS,  C_B,  NULL},
    {"z",   R_CONS,  C_S,  NULL},
    RULE_END
};

static const pron_rule LATIN_IT[] = {
    {"gli", R_LIT,   0,    "لی"}, {"gn",  R_LIT,  0,    "نی"},
    {"ch",  R_CONS,  C_K,  NULL}, {"gh",  R_CONS, C_G,  NULL},
    {"zz",  R_LIT,   0,    "تس"}, {"z",   R_LIT,  0,    "تس"},
    {"à",   R_VOWEL, V_AA, NULL}, {"è",   R_VOWEL, V_E,  NULL},
    {"é",   R_VOWEL, V_E,  NULL}, {"ì",   R_VOWEL, V_EE, NULL},
    {"ò",   R_VOWEL, V_OH, NULL}, {"ù",   R_VOWEL, V_OO, NULL},
    {"h",   R_SILENT, 0,   NULL},
    RULE_END
};

static const pron_rule LATIN_PT[] = {
    {"ão",  R_NASAL, V_A,  NULL}, {"õe",  R_NASAL, V_O, NULL},
    {"nh",  R_LIT,   0,    "نی"}, {"lh",  R_LIT,  0,   "لی"},
    {"ch",  R_CONS,  C_SH, NULL}, {"rr",  R_CONS, C_R, NULL},
    {"ss",  R_CONS,  C_S,  NULL}, {"ç",   R_CONS, C_S, NULL},
    {"á",   R_VOWEL, V_A,  NULL}, {"â",   R_VOWEL, V_A, NULL},
    {"ã",   R_NASAL, V_A,  NULL}, {"é",   R_VOWEL, V_E, NULL},
    {"ê",   R_VOWEL, V_E,  NULL}, {"í",   R_VOWEL, V_I, NULL},
    {"ó",   R_VOWEL, V_O,  NULL}, {"ô",   R_VOWEL, V_O, NULL},
    {"ú",   R_VOWEL, V_U,  NULL}, {"x",   R_LIT,  0,   "ش"},
    {"j",   R_CONS,  C_ZH, NULL},
    RULE_END
};

static const pron_rule LATIN_NL[] = {
    {"sch", R_LIT,   0,    "سخ"}, {"ij",  R_VOWEL, V_AY, NULL},
    {"ei",  R_VOWEL, V_AY, NULL}, {"ou",  R_VOWEL, V_OW, NULL},
    {"au",  R_VOWEL, V_OW, NULL}, {"oe",  R_VOWEL, V_U,  NULL},
    {"uu",  R_VOWEL, V_U,  NULL}, {"aa",  R_VOWEL, V_AA, NULL},
    {"ee",  R_VOWEL, V_EE, NULL}, {"oo",  R_VOWEL, V_OH, NULL},
    {"ch",  R_CONS,  C_KH, NULL}, {"g",   R_CONS,  C_KH, NULL},
    {"j",   R_CONS,  C_Y,  NULL}, {"w",   R_CONS,  C_V,  NULL},
    {"v",   R_CONS,  C_F,  NULL},
    RULE_END
};

/* Turkish letters that do not decompose into a base letter plus a mark. */
static const pron_rule LATIN_TR[] = {
    {"ş",   R_CONS,  C_SH, NULL}, {"ç",   R_CONS, C_CH, NULL},
    {"ı",   R_VOWEL, V_U,  NULL}, {"ğ",   R_SILENT, 0,  NULL},
    {"ö",   R_VOWEL, V_O,  NULL}, {"ü",   R_VOWEL, V_U,  NULL},
    {"c",   R_CONS,  C_J,  NULL},
    RULE_END
};

static const pron_rule LATIN_PL[] = {
    {"sz",  R_CONS,  C_SH, NULL}, {"cz",  R_CONS, C_CH, NULL},
    {"rz",  R_CONS,  C_ZH, NULL}, {"ch",  R_CONS, C_KH, NULL},
    {"ą",   R_VOWEL, V_O,  NULL}, {"ę",   R_VOWEL, V_E,  NULL},
    {"ł",   R_CONS,  C_V,  NULL}, {"ó",   R_VOWEL, V_U,  NULL},
    {"ś",   R_CONS,  C_SH, NULL}, {"ź",   R_CONS, C_ZH, NULL},
    {"ż",   R_CONS,  C_ZH, NULL}, {"ć",   R_CONS, C_CH, NULL},
    {"ń",   R_LIT,   0,    "نی"},
    {"w",   R_CONS,  C_V,  NULL}, {"j",   R_CONS, C_Y,  NULL},
    {"y",   R_VOWEL, V_I,  NULL},
    RULE_END
};

static const pron_rule LATIN_CS[] = {
    {"č",   R_CONS,  C_CH, NULL}, {"š",   R_CONS, C_SH, NULL},
    {"ž",   R_CONS,  C_ZH, NULL}, {"ř",   R_CONS, C_ZH, NULL},
    {"ď",   R_CONS,  C_D,  NULL}, {"ť",   R_CONS, C_T,  NULL},
    {"ň",   R_LIT,   0,    "نی"}, {"ch",  R_CONS, C_KH, NULL},
    {"ů",   R_VOWEL, V_U,  NULL}, {"ě",   R_VOWEL, V_E,  NULL},
    {"á",   R_VOWEL, V_AA, NULL}, {"í",   R_VOWEL, V_EE, NULL},
    {"ý",   R_VOWEL, V_EE, NULL},
    {"c",   R_LIT,   0,    "تس"}, {"j",   R_CONS, C_Y,  NULL},
    {"y",   R_VOWEL, V_I,  NULL},
    RULE_END
};

static const pron_rule LATIN_SE[] = {
    {"skj", R_CONS,  C_SH, NULL}, {"sj",  R_CONS, C_SH, NULL},
    {"tj",  R_CONS,  C_SH, NULL}, {"kj",  R_CONS, C_SH, NULL},
    {"å",   R_VOWEL, V_O,  NULL}, {"ä",   R_VOWEL, V_E,  NULL},
    {"ö",   R_VOWEL, V_O,  NULL}, {"ø",   R_VOWEL, V_O,  NULL},
    {"æ",   R_VOWEL, V_E,  NULL},
    {"j",   R_CONS,  C_Y,  NULL}, {"y",   R_VOWEL, V_U,  NULL},
    RULE_END
};

static const pron_rule LATIN_FI[] = {
    {"ä",   R_VOWEL, V_A,  NULL}, {"ö",   R_VOWEL, V_O,  NULL},
    {"y",   R_VOWEL, V_U,  NULL}, {"j",   R_CONS, C_Y,  NULL},
    RULE_END
};

static const pron_rule LATIN_HU[] = {
    {"sz",  R_CONS,  C_S,  NULL}, {"zs",  R_CONS, C_ZH, NULL},
    {"cs",  R_CONS,  C_CH, NULL}, {"gy",  R_CONS, C_J,  NULL},
    {"ny",  R_LIT,   0,    "نی"},
    {"á",   R_VOWEL, V_AA, NULL}, {"é",   R_VOWEL, V_E,  NULL},
    {"í",   R_VOWEL, V_EE, NULL}, {"ö",   R_VOWEL, V_O,  NULL},
    {"ő",   R_VOWEL, V_O,  NULL}, {"ú",   R_VOWEL, V_U,  NULL},
    {"ü",   R_VOWEL, V_U,  NULL}, {"ű",   R_VOWEL, V_U,  NULL},
    {"s",   R_CONS,  C_SH, NULL}, {"gy",  R_CONS, C_J,  NULL},
    {"ny",  R_LIT,   0,    "نی"}, {"ty",  R_CONS, C_T,  NULL},
    {"ly",  R_LIT,   0,    "ی"},  {"y",   R_SILENT, 0,  NULL},
    RULE_END
};

static const pron_rule LATIN_RO[] = {
    {"ș",   R_CONS,  C_SH, NULL}, {"ş",   R_CONS, C_SH, NULL},
    {"ț",   R_LIT,   0,    "تس"}, {"ţ",   R_LIT,  0,   "تس"},
    {"ch",  R_CONS,  C_K,  NULL}, {"gh",  R_CONS, C_G, NULL},
    {"ă",   R_VOWEL, V_A,  NULL}, {"â",   R_VOWEL, V_U, NULL},
    {"î",   R_VOWEL, V_U,  NULL}, {"j",   R_CONS, C_ZH, NULL},
    {"y",   R_VOWEL, V_I,  NULL},
    RULE_END
};

static const pron_rule LATIN_VI[] = {
    {"ph",  R_CONS,  C_F,  NULL}, {"th",  R_CONS, C_T,  NULL},
    {"tr",  R_LIT,   0,    "چ"},  {"nh",  R_LIT,  0,    "نی"},
    {"gi",  R_LIT,   0,    "ز"},
    {"đ",   R_CONS,  C_D,  NULL}, {"ơ",   R_VOWEL, V_O,  NULL},
    {"ư",   R_VOWEL, V_U,  NULL}, {"ă",   R_VOWEL, V_A,  NULL},
    {"d",   R_CONS,  C_Z,  NULL}, {"x",   R_CONS, C_S,  NULL},
    RULE_END
};

/* Rules that make sense for any Latin-script language. */
static const pron_rule LATIN_INTL[] = {
    {"ss",  R_CONS,  C_S,  NULL},
    {"oe",  R_VOWEL, V_O,  NULL},
    {"ae",  R_VOWEL, V_E,  NULL},
    RULE_END
};

static const pron_rule *latin_table_for(const char *tag) {
    static const struct { const char *tag; const pron_rule *t; } T[] = {
        {"en", LATIN_EN}, {"de", LATIN_DE}, {"fr", LATIN_FR}, {"es", LATIN_ES},
        {"it", LATIN_IT}, {"pt", LATIN_PT}, {"nl", LATIN_NL}, {"tr", LATIN_TR},
        {"pl", LATIN_PL}, {"cs", LATIN_CS}, {"sk", LATIN_CS}, {"sv", LATIN_SE},
        {"da", LATIN_SE}, {"nb", LATIN_SE}, {"no", LATIN_SE}, {"fi", LATIN_FI},
        {"hu", LATIN_HU}, {"ro", LATIN_RO}, {"vi", LATIN_VI},
        /* Languages close enough to the base table that their sentences still
         * read correctly: id/ms/sw/af/ca/gl/eu/hr/sl/lt/lv/et/bs/is/sq/ga/cy/tl */
        {NULL, NULL}
    };
    if (!tag) return NULL;
    for (int i = 0; T[i].tag; i++)
        if (osa_starts(tag, T[i].tag) && (tag[2] == 0 || tag[2] == '-')) return T[i].t;
    return NULL;
}

/* ---- Cyrillic ------------------------------------------------------ */
static const pron_rule CYR_BASE[] = {
    {"дж",  R_LIT,   0,    "ج"},  {"й",   R_CONS, C_Y,  NULL},
    /* At the start of a word «я/е/ю/ё» begin with a y-sound; after a consonant
     * they are plain vowels ("не" is «نِ», not «نیِ»). */
    {"ё",   R_VOWEL_INITIAL, V_YO, NULL}, {"ю", R_VOWEL_INITIAL, V_YU, NULL},
    {"я",   R_VOWEL_INITIAL, V_YA, NULL}, {"е", R_VOWEL_INITIAL, V_YE, NULL},
    {"ё",   R_VOWEL, V_O,  NULL}, {"ю",   R_VOWEL, V_U,  NULL},
    {"я",   R_VOWEL, V_A,  NULL}, {"е",   R_FINAL, V_YE, NULL},
    {"е",   R_VOWEL, V_E,  NULL},
    {"э",   R_VOWEL, V_E,  NULL}, {"ы",   R_VOWEL, V_I,  NULL},
    {"и",   R_VOWEL, V_I,  NULL}, {"а",   R_VOWEL, V_A,  NULL},
    {"о",   R_VOWEL, V_O,  NULL}, {"у",   R_VOWEL, V_U,  NULL},
    {"б",   R_CONS,  C_B,  NULL}, {"в",   R_CONS, C_V,  NULL},
    {"г",   R_CONS,  C_G,  NULL}, {"д",   R_CONS, C_D,  NULL},
    {"ж",   R_CONS,  C_ZH, NULL}, {"з",   R_CONS, C_Z,  NULL},
    {"к",   R_CONS,  C_K,  NULL}, {"л",   R_CONS, C_L,  NULL},
    {"м",   R_CONS,  C_M,  NULL}, {"н",   R_CONS, C_N,  NULL},
    {"п",   R_CONS,  C_P,  NULL}, {"р",   R_CONS, C_R,  NULL},
    {"с",   R_CONS,  C_S,  NULL}, {"т",   R_CONS, C_T,  NULL},
    {"ф",   R_CONS,  C_F,  NULL}, {"х",   R_CONS, C_KH, NULL},
    {"ц",   R_LIT,   0,    "تس"}, {"ч",   R_CONS, C_CH, NULL},
    {"ш",   R_CONS,  C_SH, NULL}, {"щ",   R_LIT,  0,   "شچ"},
    {"ъ",   R_SILENT, 0,   NULL}, {"ь",   R_SILENT, 0, NULL},
    {"і",   R_VOWEL, V_I,  NULL}, {"ї",   R_VOWEL, V_YI, NULL},
    {"є",   R_VOWEL, V_YE, NULL}, {"ґ",   R_CONS, C_G,  NULL},
    {"ә",   R_VOWEL, V_A,  NULL}, {"ғ",   R_CONS, C_G,  NULL},
    {"қ",   R_CONS,  C_K,  NULL}, {"ң",   R_LIT,  0,    "نگ"},
    {"ө",   R_VOWEL, V_O,  NULL}, {"ұ",   R_VOWEL, V_U,  NULL},
    {"ү",   R_VOWEL, V_U,  NULL}, {"һ",   R_CONS, C_H,  NULL},
    RULE_END
};

/* ---- Greek --------------------------------------------------------- */
static const pron_rule GREEK_BASE[] = {
    {"ου",  R_VOWEL, V_U,  NULL}, {"ει",  R_VOWEL, V_I,  NULL},
    {"οι",  R_VOWEL, V_I,  NULL}, {"αι",  R_VOWEL, V_E,  NULL},
    {"αυ",  R_VOWEL, V_A,  NULL}, {"ευ",  R_VOWEL, V_E,  NULL},
    {"μπ",  R_CONS,  C_B,  NULL}, {"ντ",  R_CONS, C_D,  NULL},
    {"γκ",  R_CONS,  C_G,  NULL}, {"τζ",  R_CONS, C_J,  NULL},
    {"τσ",  R_LIT,   0,    "تس"}, {"γγ",  R_LIT,  0,   "نگ"},
    {"θ",   R_CONS,  C_TH, NULL}, {"χ",   R_CONS, C_KH, NULL},
    {"ψ",   R_LIT,   0,    "پس"}, {"ξ",   R_LIT,  0,   "کس"},
    {"β",   R_CONS,  C_V,  NULL}, {"γ",   R_CONS, C_G,  NULL},
    {"δ",   R_CONS,  C_DH, NULL}, {"ζ",   R_CONS, C_Z,  NULL},
    {"η",   R_VOWEL, V_I,  NULL}, {"ι",   R_VOWEL, V_I,  NULL},
    {"υ",   R_VOWEL, V_I,  NULL}, {"ω",   R_VOWEL, V_O,  NULL},
    {"α",   R_VOWEL, V_A,  NULL}, {"ε",   R_VOWEL, V_E,  NULL},
    {"ο",   R_VOWEL, V_O,  NULL}, {"κ",   R_CONS, C_K,  NULL},
    {"λ",   R_CONS,  C_L,  NULL}, {"μ",   R_CONS, C_M,  NULL},
    {"ν",   R_CONS,  C_N,  NULL}, {"π",   R_CONS, C_P,  NULL},
    {"ρ",   R_CONS,  C_R,  NULL}, {"σ",   R_CONS, C_S,  NULL},
    {"ς",   R_CONS,  C_S,  NULL}, {"τ",   R_CONS, C_T,  NULL},
    {"φ",   R_CONS,  C_F,  NULL},
    {"ά",   R_VOWEL, V_A,  NULL}, {"έ",   R_VOWEL, V_E,  NULL},
    {"ή",   R_VOWEL, V_I,  NULL}, {"ί",   R_VOWEL, V_I,  NULL},
    {"ό",   R_VOWEL, V_O,  NULL}, {"ύ",   R_VOWEL, V_I,  NULL},
    {"ώ",   R_VOWEL, V_O,  NULL}, {"ϊ",   R_VOWEL, V_I,  NULL},
    {"ϋ",   R_VOWEL, V_I,  NULL},
    RULE_END
};

/* ---- Arabic --------------------------------------------------------
 * Arabic already uses the output script, so this reader mostly *vocalises*: it
 * copies the letters and supplies the short vowels Arabic leaves out, which is
 * what turns شكرا into شُکراً instead of a bare skeleton.
 */
static const pron_rule AR_BASE[] = {
    {"لا",  R_LIT,   0,    "لا"},  {"ا",   R_VOWEL, V_AA, NULL},
    {"أ",   R_VOWEL, V_A,  NULL},  {"إ",   R_VOWEL, V_E,  NULL},
    {"آ",   R_VOWEL, V_AA, NULL},  {"و",   R_CONS,  C_V,  NULL},
    {"ي",   R_CONS,  C_Y,  NULL},  {"ى",   R_VOWEL, V_AA, NULL},
    {"ة",   R_CONS,  C_H,  NULL},  {"ء",   R_SILENT, 0,   NULL},
    {"ب",   R_CONS,  C_B,  NULL},  {"ت",   R_CONS,  C_T,  NULL},
    {"ث",   R_CONS,  C_S,  NULL},  {"ج",   R_CONS,  C_J,  NULL},
    {"ح",   R_CONS,  C_H,  NULL},  {"خ",   R_CONS,  C_KH, NULL},
    {"د",   R_CONS,  C_D,  NULL},  {"ذ",   R_CONS,  C_Z,  NULL},
    {"ر",   R_CONS,  C_R,  NULL},  {"ز",   R_CONS,  C_Z,  NULL},
    {"س",   R_CONS,  C_S,  NULL},  {"ش",   R_CONS,  C_SH, NULL},
    {"ص",   R_CONS,  C_S,  NULL},  {"ض",   R_CONS,  C_Z,  NULL},
    {"ط",   R_CONS,  C_T,  NULL},  {"ظ",   R_CONS,  C_Z,  NULL},
    {"ع",   R_SILENT, 0,   NULL}, {"غ",   R_CONS,  C_G,  NULL},
    {"ف",   R_CONS,  C_F,  NULL},  {"ق",   R_CONS,  C_G,  NULL},
    {"ك",   R_CONS,  C_K,  NULL},  {"ک",   R_CONS,  C_K,  NULL},
    {"ل",   R_CONS,  C_L,  NULL},  {"م",   R_CONS,  C_M,  NULL},
    {"ن",   R_CONS,  C_N,  NULL},  {"ه",   R_CONS,  C_H,  NULL},
    {"پ",   R_CONS,  C_P,  NULL},  {"چ",   R_CONS,  C_CH, NULL},
    {"ژ",   R_CONS,  C_ZH, NULL},  {"گ",   R_CONS,  C_G,  NULL},
    {"َ",   R_VOWEL, V_A,  NULL},  {"ِ",   R_VOWEL, V_E,  NULL},
    {"ُ",   R_VOWEL, V_O,  NULL},  {"ّ",   R_SILENT, 0,   NULL},
    {"ْ",   R_SILENT, 0,    NULL},
    RULE_END
};

/* ---- Hebrew -------------------------------------------------------- */
static const pron_rule HEB_BASE[] = {
    {"ש",   R_CONS,  C_SH, NULL}, {"צ",   R_LIT,  0,    "تس"},
    {"א",   R_SILENT, 0,   NULL}, {"ב",   R_CONS, C_B,  NULL},
    {"ג",   R_CONS,  C_G,  NULL}, {"ד",   R_CONS, C_D,  NULL},
    {"ה",   R_CONS,  C_H,  NULL}, {"ו",   R_CONS, C_V,  NULL},
    {"ז",   R_CONS,  C_Z,  NULL}, {"ח",   R_CONS, C_KH, NULL},
    {"ט",   R_CONS,  C_T,  NULL}, {"י",   R_CONS, C_Y, NULL},
    {"כ",   R_CONS,  C_K,  NULL}, {"ך",   R_CONS, C_K,  NULL},
    {"ל",   R_CONS,  C_L,  NULL}, {"מ",   R_CONS, C_M,  NULL},
    {"ם",   R_CONS,  C_M,  NULL}, {"נ",   R_CONS, C_N,  NULL},
    {"ן",   R_CONS,  C_N,  NULL}, {"ס",   R_CONS, C_S,  NULL},
    {"ע",   R_CONS,  C_EYN, NULL},{"פ",   R_CONS, C_F,  NULL},
    {"ף",   R_CONS,  C_F,  NULL}, {"ק",   R_CONS, C_K,  NULL},
    {"ר",   R_CONS,  C_R,  NULL}, {"ת",   R_CONS, C_T,  NULL},
    RULE_END
};

/* ---- other scripts ------------------------------------------------ */

typedef enum script_id {
    SCR_UNKNOWN = 0, SCR_LATIN, SCR_CYRILLIC, SCR_GREEK, SCR_ARABIC,
    SCR_HEBREW, SCR_UNSUPPORTED
} script_id;

/* Scripts that cannot be read letter by letter: each character is a syllable or
 * a word, so a table would produce noise. Reported, never guessed. */
static bool unsupported_block(uint32_t cp) {
    return (cp >= 0x0400 && cp <= 0x04FF) ? false :   /* Cyrillic: supported  */
           (cp >= 0x0370 && cp <= 0x03FF) ? false :   /* Greek: supported     */
           (cp >= 0x0590 && cp <= 0x05FF) ? false :   /* Hebrew: supported    */
           (cp >= 0x0600 && cp <= 0x06FF) ? false :   /* Arabic: supported    */
           (cp >= 0x0750 && cp <= 0x077F) ? false :   /* Arabic supplement    */
           (cp >= 0x0900 && cp <= 0x0DFF) ? true :    /* Indic                 */
           (cp >= 0x0E00 && cp <= 0x0EFF) ? true :    /* Thai / Lao            */
           (cp >= 0x1000 && cp <= 0x109F) ? true :    /* Myanmar               */
           (cp >= 0x10A0 && cp <= 0x10FF) ? true :    /* Georgian              */
           (cp >= 0x1200 && cp <= 0x137F) ? true :    /* Ethiopic              */
           (cp >= 0x1780 && cp <= 0x17FF) ? true :    /* Khmer                 */
           (cp >= 0x0530 && cp <= 0x058F) ? true :    /* Armenian              */
           (cp >= 0x1100 && cp <= 0x11FF) ? true :    /* Hangul jamo           */
           (cp >= 0x2E80 && cp <= 0x9FFF) ? true :    /* CJK                   */
           (cp >= 0xAC00 && cp <= 0xD7AF) ? true :    /* Hangul syllables      */
           (cp >= 0xF900 && cp <= 0xFAFF) ? true :    /* CJK compatibility     */
           (cp >= 0x3040 && cp <= 0x30FF) ? true :    /* Kana                  */
           false;
}

static const pron_rule *table_for_cp(uint32_t cp) {
    if (cp >= 0x0400 && cp <= 0x04FF) return CYR_BASE;
    if (cp >= 0x0370 && cp <= 0x03FF) return GREEK_BASE;
    if (cp >= 0x0590 && cp <= 0x05FF) return HEB_BASE;
    if (cp >= 0x0600 && cp <= 0x06FF) return AR_BASE;
    if (cp >= 0x0750 && cp <= 0x077F) return AR_BASE;
    return NULL;
}

/* One UTF-8 scalar: returns the byte length and the code point. Invalid bytes are
 * consumed singly, so a damaged name never stops the whole line. */
static int utf8_next(const char *p, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)p;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && u[1]) { *cp = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F); return 2; }
    if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) {
        *cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
        *cp = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) |
              ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        return 4;
    }
    *cp = u[0];
    return 1;
}

static bool is_space(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x00A0;
}

/* ---- public API ---------------------------------------------------- */

const char *mt_pronounce_script(const char *tag) {
    if (!tag || !*tag) return "unknown";
    switch (osa_script_for_tag(tag)) {
        case OSA_SCRIPT_LATIN:    return "latin";
        case OSA_SCRIPT_CYRILLIC: return "cyrillic";
        case OSA_SCRIPT_GREEK:    return "greek";
        case OSA_SCRIPT_HEBREW:   return "hebrew";
        case OSA_SCRIPT_ARABIC:   return osa_starts(tag, "fa") ? "persian" : "arabic";
        /* Written in a script this build cannot read letter by letter: say so
         * instead of guessing at the sound. */
        case OSA_SCRIPT_NONE:     return "unknown";
        default:                  return "unsupported";
    }
}



static void read_word(char *word, const pron_rule *table, const pron_rule *extra,
                      pron_out *o);
static size_t english_shape(char *w);
static void copy_word(char *dst, size_t cap, const char *src);
static char *sanitise_diacritics(char *text, size_t cap);

/* Reads one run of one script and appends it to `out`. Returns false when there
 * was nothing it could read. */
static bool read_run(const char *run, size_t len, const char *tag, pron_out *o) {
    char buf[512];
    if (len >= sizeof buf) len = sizeof buf - 1;
    memcpy(buf, run, len);
    buf[len] = 0;

    /* Decide the script from the first character that carries one. */
    const char *p = buf;
    const pron_rule *table = NULL;
    const pron_rule *extra = NULL;
    while (*p) {
        uint32_t cp = 0;
        int n = utf8_next(p, &cp);
        if (cp < 0x80) { p += n; continue; }
        table = table_for_cp(cp);
        if (!table) {
            /* Latin letters with diacritics (ü, ş, ą ...) and any other non-ASCII
             * character that is not in a supported script. */
            if (unsupported_block(cp)) return false;
            table = LATIN_BASE;
            extra = latin_table_for(tag);
        }
        break;
    }
    if (!table) { table = LATIN_BASE; extra = latin_table_for(tag); }
    /* Which scripts write vowels as letters rather than as diacritics. */
    o->full = (table == CYR_BASE || table == GREEK_BASE || table == AR_BASE);

    /* Lowercase ASCII in place; the tables hold their own lowercase forms for
     * everything else. */
    for (char *c = buf; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');

    /* Fold accented Latin onto its base letter. The tables are keyed on the base
     * letter, so one entry for "s" covers ś, š, ș and ş unless the language
     * says otherwise. */
    if (table == LATIN_BASE) {
        char folded[512];
        size_t fn = 0;
        for (const char *q = buf; *q && fn + 4 < sizeof folded; ) {
            uint32_t cp = 0;
            int n = utf8_next(q, &cp);
            char base = fold_latin(cp);
            /* Unless the language has an opinion about this letter: German ü is
             * یو, Czech č is چ, Turkish ş is ش. Those rules come first. */
            char one[5] = {0};
            memcpy(one, q, (size_t)(n < 4 ? n : 4));
            if (extra && match_rule_at(extra, one, one, 0)) base = 0;
            if (cp >= 0xC0 && base) {
                folded[fn++] = base;
                q += n;
                continue;
            }
            for (int k = 0; k < n && fn + 4 < sizeof folded; k++) folded[fn++] = q[k];
            q += n;
        }
        folded[fn] = 0;
        memcpy(buf, folded, fn + 1);
    }

    /* Read word by word: a rule marked R_FINAL needs to know the end of its
     * word, and a new word may not carry its first vowel on the previous word's
     * last consonant. */
    o->last_class = 0;
    o->last_diac = 0;
    p = buf;
    while (*p) {
        char word[256];
        size_t wn = 0;
        while (*p && *p != ' ' && wn + 1 < sizeof word) word[wn++] = *p++;
        word[wn] = 0;
        /* English irregular spellings come from the dictionary first: "you" is
         * یُو, not یَؤ. The same words are read by rule in other languages, where
         * they are usually regular ("a" in Spanish, "the" nowhere else). */
        if (extra == LATIN_EN) {
            /* The dictionary is consulted on the word as *written* ("you", not
             * "yoo"); the shape rules then apply to whatever is left. */
            char plain[64];
            size_t pn = 0;
            for (size_t i = 0; word[i] && pn + 1 < sizeof plain; i++) {
                unsigned char c = (unsigned char)word[i];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-')
                    plain[pn++] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
            }
            plain[pn] = 0;
            const char *known = mt_pronounce_known(plain);
            if (known) { put(o, known); return true; }
            wn = english_shape(word);
            if (wn > sizeof word - 1) wn = sizeof word - 1;
        }
        /* Elisions: "d'accord", "l'homme" are two words. Italian "dov'è" is not:
         * there the apostrophe only marks a dropped vowel, so it is removed. */
        if (extra != LATIN_FR && extra != LATIN_EN) {
            char *q = word;
            for (char *r = word; *r; r++) if (*r != '\'') *q++ = *r;
            *q = 0;
        }
        char *ap;
        while ((ap = strchr(word, '\'')) != NULL) {
            *ap = 0;
            if (*word) read_word(word, table, extra, o);
            memmove(word, ap + 1, strlen(ap + 1) + 1);
            o->last_class = 0;
            o->last_diac = 0;
        }
        if (*word) read_word(word, table, extra, o);
        if (*p) put(o, " ");
        o->last_class = 0;      /* the carrier rule restarts at every word */
        o->last_diac = 0;
        o->prev_vowel = 0;      /* and so does the iotated-vowel rule */
        while (*p == ' ') p++;
    }
    return true;
}

/* English spellings that a letter-by-letter table cannot express, applied before
 * the rules run. Only English uses this; the other Latin languages spell their
 * vowels out.
 *
 *   make  -> mayk  -> مَیک   (the final e makes the vowel long and is not spoken)
 *   table -> tabel -> تِیبِل (-le after a consonant is a syllable of its own)
 *   city  -> sity  -> سیتی   (c before e/i/y is "s")
 *   excite-> ekssit -> اِکسِت (/ks spelled out, c softened)
 */
/* Copy a string into a fixed buffer, always terminated. */
static void copy_word(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static size_t english_shape(char *w) {
    char out[256];
    size_t n = strlen(w);

    /* -le at the end closes a syllable of its own: "table" -> "taybel",
     * "little" -> "laittel". The vowel before it takes its long reading. */
    if (n >= 4 && w[n - 1] == 'e' && w[n - 2] == 'l') {
        size_t v = n - 3;
        while (v > 0 && !strchr("aeiou", w[v])) v--;
        if (strchr("aeiou", w[v]) && v + 1 < n - 2) {
            const char *lg = w[v] == 'a' ? "ei" : w[v] == 'e' ? "ee" : w[v] == 'i' ? "ei" :
                             w[v] == 'o' ? "oa" : "oo";
            snprintf(out, sizeof out, "%.*s%s%.*sel", (int)v, w, lg, (int)(n - 2 - (v + 1)), w + v + 1);
            copy_word(w, 256, out);
            return strlen(w);
        }
    }

    /* -ed after a voiced consonant is one syllable (walked); after t/d it is two
     * (wanted) and the e is a vowel of its own, so that case is left alone. */
    if (n >= 4 && w[n - 1] == 'd' && w[n - 2] == 'e') {
        if (w[n - 3] != 't' && w[n - 3] != 'd') {
            w[n - 2] = 'd';
            w[n - 1] = 0;
            n = strlen(w);
        }
    }

    /* Magic e: vowel, one consonant, final e. The vowel is long and the e is not
     * spoken: "make" -> "mayk", "time" -> "taim", "note" -> "noat". */
    if (n >= 4 && w[n - 1] == 'e' && !strchr("aeiou", w[n - 2]) &&
        strchr("aeiou", w[n - 3]) && w[n - 2] != 'r' && w[n - 2] != 'w' &&
        w[n - 2] != 'y' && w[n - 3] != w[n - 4]) {
        char v = w[n - 3];
        /* "a" and "i" both become «ای» in Persian letters («مِیک», «تِیم»); the
         * other three have a long letter of their own. */
        const char *lg = v == 'a' ? "ei" : v == 'e' ? "ee" : v == 'i' ? "ei" :
                         v == 'o' ? "oa" : v == 'u' ? "oo" : NULL;
        if (lg) {
            size_t pre = n - 3;
            snprintf(out, sizeof out, "%.*s%s%.*s", (int)pre, w, lg, (int)(n - 1 - (pre + 1)), w + pre + 1);
            copy_word(w, 256, out);
            n = strlen(w);
        }
    }

    /* A final e after a consonant is silent ("note" is نوت, "make" is مَیک):
     * dropping it here covers the words the magic-e rule above does not touch. */
    if (n >= 3 && w[n - 1] == 'e' && !strchr("aeiou", w[n - 2])) {
        w[n - 1] = 0;
        n = strlen(w);
    }

    /* Soft c and g before e/i/y ("city" -> "sity", "page" -> "paje"), and a
     * doubled consonant gets a short vowel in front of it rather than being read
     * as one long letter: "little" -> "littel", already handled by -le. */
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < sizeof out; i++) {
        char c = w[i], next = (i + 1 < n) ? w[i + 1] : 0;
        if (c == 'c' && strchr("eiy", next)) { out[o++] = 's'; continue; }
        if (c == 'g' && strchr("eiy", next)) { out[o++] = 'j'; continue; }
        if (c == 'x') { out[o++] = 'k'; out[o++] = 's'; continue; }
        /* a double consonant after a single vowel: "coffee" -> "cofee" */
        if (i + 2 < n && c == next && !strchr("aeiou", c) &&
            i >= 1 && strchr("aeiou", w[i - 1]) && !strchr("aeiou", w[i + 1])) {
            out[o++] = c;
            i++;                       /* skip the second one */
            continue;
        }
        out[o++] = c;
    }
    out[o] = 0;
    copy_word(w, 256, out);
    return strlen(w);
}

/* One word, with the rules of one table set. */
static void read_word(char *word, const pron_rule *table, const pron_rule *extra, pron_out *o) {
    /* "Kaffee", "bitte", "letter": a doubled consonant is one sound. Persian has no
     * way to write a long consonant, and «کَففِ» reads as two f's. Only when a
     * vowel precedes it, so a digraph like "ss" in the tables still wins. */
    if (table == LATIN_BASE || extra) {
        for (char *q = word; q[0] && q[1]; q++) {
            if (q[0] == q[1] && strchr("bcdfgklmnprstvz", q[0]) &&
                (q == word || strchr("aeiou", q[-1]))) {
                memmove(q + 1, q + 2, strlen(q + 2) + 1);
            }
        }
    }
    const char *p = word;
    while (*p) {
        const pron_rule *r = extra ? match_rule_at(extra, p, word, o->prev_vowel) : NULL;
        if (!r) r = match_rule_at(LATIN_INTL, p, word, o->prev_vowel);
        if (!r) r = match_rule_at(table, p, word, o->prev_vowel);
        if (r) {
            apply_rule(o, r);
            p += strlen(r->from);
            continue;
        }
        uint32_t cp = 0;
        int n = utf8_next(p, &cp);
        if (cp == ' ' || cp == '\t' || cp == '\n') {
            put(o, " ");
        } else if (cp >= '0' && cp <= '9') {
            for (int k = 0; k < n; k++) put(o, (char[]){ p[k], 0 });
        } else if (cp < 0x80) {
            /* punctuation and anything unreadable: keep it, it is what the user
             * typed and dropping it changes the sentence */
            for (int k = 0; k < n; k++) put(o, (char[]){ p[k], 0 });
        } else if (cp >= 0x0300 && cp <= 0x036F) {
            /* a combining mark from the source spelling (ả, ệ ...): the vowel it
             * modifies has already been written, so the mark itself is not a
             * letter and must not reach the Persian line */
        } else if (cp == 0x064E) { emit_vowel(o, V_A); }
        else if (cp == 0x0650) { emit_vowel(o, V_E); }
        else if (cp == 0x064F) { emit_vowel(o, V_O); }
        else if (cp == 0x064B) { put(o, D_TANVIN); }
        else {
            /* A character no table covers: copy it rather than lose the word. */
            char tmp[5] = {0};
            memcpy(tmp, p, (size_t)n);
            put_c(o, tmp, 3);
        }
        p += n;
    }
}


/* A Persian consonant letter: what a short vowel can sit on. */

/* The last line of defence for the diacritics, applied to the whole result.
 *
 * Every reader aims to place short vowels on the consonant before them, but a
 * dictionary entry, a literal or a mixed-script sentence can still slip past. A
 * diacritic on ا/ی/و is the single thing a Persian reader notices first
 * ("تاِیم" instead of «تِیم»), so it is also corrected here, once, for the whole
 * string: the mark is dropped, or moved onto the consonant before the long vowel
 * when there is one and it can still be read that way.
 *
 * Returns `text` (edited in place) for convenience.
 */
static int is_consonant(uint32_t cp) {
    if (cp < 0x0621 || cp > 0x06D2) return 0;
    return cp != 0x0627 && cp != 0x0622 && cp != 0x0623 && cp != 0x0625 &&
           cp != 0x0624 && cp != 0x0626 && cp != 0x0648 && cp != 0x06CC &&
           cp != 0x0649 && cp != 0x0629;
}


static char *sanitise_diacritics(char *text, size_t cap) {
    size_t n = strlen(text);
    char *out = (char *)malloc(cap ? cap : 1);
    if (!out) return text;
    size_t o = 0;

    /* Walk the string and decode each character once. `prev`, `prev_len`,
     * `mark_at` and `has_mark` describe the word being built, which is everything
     * the two rules below need to know. */
    uint32_t prev = 0;        /* last character that was not a diacritic */
    size_t   prev_at = 0;     /* where it starts in `out` */
    size_t   prev_len = 0;
    int      has_mark = 0;    /* it already carries a short vowel */
    size_t   mark_at = 0;     /* where that vowel sits in `out` */

    for (size_t i = 0; i < n; ) {
        uint32_t cp = 0;
        int len = 1;
        unsigned char b = (unsigned char)text[i];
        if (b < 0x80) { cp = b; len = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = ((uint32_t)(b & 0x1FU) << 6) | ((unsigned char)text[i + 1] & 0x3F); len = 2; }
        else if ((b & 0xF0) == 0xE0) {
            cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)((unsigned char)text[i + 1] & 0x3F) << 6) |
                 ((unsigned char)text[i + 2] & 0x3F);
            len = 3;
        } else { cp = b; len = 1; }

        int is_diac = (cp >= 0x064B && cp <= 0x0652) || cp == 0x0670;

        if (is_diac) {
            if (has_mark) { i += len; continue; }        /* never doubled */
            if (prev == 0) { i += len; continue; }       /* no letter to sit on */

            /* A short vowel can sit on a consonant, or on a word-initial alef used
             * as a carrier («اِخ» for "ich"). It cannot sit on ا/ی/و when those
             * are long vowels, which is what the rule below detects. */
            int long_letter = (prev == 0x0627 || prev == 0x0622 || prev == 0x06CC || prev == 0x0648);
            if (long_letter) {
                /* The letter before the long vowel decides what the long vowel is. */
                uint32_t before = 0;
                size_t k = 0, before_end = 0;
                {
                    size_t j = prev_at;
                    while (j > 0 && ((unsigned char)out[j - 1] & 0xC0) == 0x80) j--;
                    k = j;
                    while (k > 0 && ((unsigned char)out[k - 1] & 0xC0) == 0x80) k--;
                    unsigned char c0 = (unsigned char)out[k];
                    if (c0 < 0x80) before = c0;
                    else if ((c0 & 0xE0) == 0xC0) before = ((uint32_t)(c0 & 0x1FU) << 6) | ((unsigned char)out[k + 1] & 0x3F);
                    else before = ((uint32_t)(c0 & 0x0F) << 12) |
                                  ((uint32_t)((unsigned char)out[k + 1] & 0x3F) << 6) |
                                  ((unsigned char)out[k + 2] & 0x3F);
                    before_end = k + (((c0 & 0xF0) == 0xE0) ? 3 : ((c0 & 0xE0) == 0xC0) ? 2 : 1);
                }
                if (!is_consonant(before)) {
                    /* Word-initial: the letter is a carrier, so the mark is right. */
                    if (o + (size_t)len < cap) {
                        memcpy(out + o, text + i, (size_t)len);
                        o += (size_t)len;
                        out[o] = 0;
                        mark_at = o;
                        has_mark = 1;
                    }
                    i += len;
                    continue;
                }
                if (prev == 0x0627 || prev == 0x0622) {
                    /* The alef before this mark is a long vowel (a consonant precedes
                     * it), so the vowel belongs on that consonant: «تاِیم» for
                     * "time" becomes «تِیم». */
                    if (o + (size_t)len < cap) {
                        memmove(out + before_end + len, out + before_end, o - before_end);
                        memcpy(out + before_end, text + i, (size_t)len);
                        o += (size_t)len;
                        out[o] = 0;
                        prev_at += (size_t)len;
                        mark_at = before_end;
                        has_mark = 1;
                    }
                    i += len;
                    continue;
                }
                /* ی or و after a consonant carries a variant vowel (یُ, وُ) and the
                 * mark is legitimate: "question" is «کوِستچِن», "video" is
                 * «ویدِیُو». Dropping it would lose a syllable. */
                if (o + (size_t)len < cap) {
                    memcpy(out + o, text + i, (size_t)len);
                    o += (size_t)len;
                    out[o] = 0;
                    mark_at = o;
                    has_mark = 1;
                }
                i += len;
                continue;
            }

            /* A consonant: the mark goes right after it. */
            if (o + (size_t)len < cap) {
                memcpy(out + o, text + i, (size_t)len);
                o += (size_t)len;
                out[o] = 0;
                mark_at = o;
                has_mark = 1;
            }
            i += len;
            continue;
        }

        /* A real character. */
        if (o + (size_t)len >= cap) { i += len; continue; }
        prev = cp;
        prev_at = o;
        prev_len = (size_t)len;
        has_mark = 0;
        memcpy(out + o, text + i, (size_t)len);
        o += (size_t)len;
        i += len;
    }
    out[o] = 0;
    memcpy(text, out, o + 1);
    free(out);
    (void)prev_len; (void)mark_at;
    return text;
}

bool mt_pronounce_ex(const char *text, const char *tag, char *out, size_t cap,
                     bool *unsupported) {
    if (unsupported) *unsupported = false;
    if (!out || cap < 8) return false;
    out[0] = 0;
    if (!text || !*text) return false;

    /* Persian is already what the line is for: nothing to transliterate. */
    if (tag && osa_starts(tag, "fa")) return false;

    pron_out o;
    memset(&o, 0, sizeof o);
    o.buf = out;
    o.cap = cap;

    const char *p = text;
    bool any = false, skipped = false;
    while (*p) {
        /* Skip runs that are pure whitespace or punctuation. */
        uint32_t cp = 0;
        int n = utf8_next(p, &cp);
        if (is_space(cp) || cp == '\n') {
            if (o.n > 0 && out[o.n - 1] != ' ') put(&o, " ");
            p += n;
            continue;
        }
        /* Collect one run of the same *script family*: a word may mix Latin with
         * digits ("GPT-4") and runs must not be re-decided per character. */
        const char *start = p;
        uint32_t first = cp;
        bool latin_first = (first < 0x80) || (!table_for_cp(first) && !unsupported_block(first));
        while (*p) {
            uint32_t c2 = 0;
            int n2 = utf8_next(p, &c2);
            if (is_space(c2)) break;
            bool latin_here = (c2 < 0x80) || (!table_for_cp(c2) && !unsupported_block(c2));
            if (latin_here != latin_first && c2 >= 0x80) break;
            p += n2;
        }
        size_t len = (size_t)(p - start);
        /* A CJK/Thai/... run is not readable here: mark it and keep the text so
         * the caller can tell the user which sentence is not supported. */
        if (!latin_first) {
            uint32_t probe = 0;
            utf8_next(start, &probe);
            if (unsupported_block(probe)) {
                skipped = true;
                if (unsupported) *unsupported = true;
                /* Keep the original word: a Persian reader at least sees which
                 * sentence this is. */
                for (size_t i = 0; i < len; i++) { char t[2] = { start[i], 0 }; put(&o, t); }
                if (o.n > 0 && out[o.n - 1] != ' ') put(&o, " ");
                continue;
            }
        }
        if (read_run(start, len, tag, &o)) any = true;
        if (o.n > 0 && out[o.n - 1] != ' ') put(&o, " ");
    }
    while (o.n > 0 && out[o.n - 1] == ' ') out[--o.n] = 0;
    (void)skipped;
    if (!any || o.n == 0) return false;
    sanitise_diacritics(out, cap);
    return true;
}
