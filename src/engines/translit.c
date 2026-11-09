/* =====================================================================
 *  OpenSpeech AI — آوانویسی: English → Persian letters, with diacritics
 *
 *  Turns an English word or sentence into readable Persian letters so the user
 *  can say a translation out loud:
 *
 *      nice to meet you     →  نایس تُو میت یُو
 *      hello                →  هِلُو
 *      speech recognition   →  اِسپیچ رِکاگنیشِن
 *      good morning         →  گود مورنینگ
 *
 *  Two layers:
 *
 *   1. a pronunciation dictionary for the words that occur in every sentence
 *      ("the", "you", "to", "meet", …). These are irregular in English and no
 *      rule table gets them right, and getting them wrong makes the whole
 *      feature look broken.
 *
 *   2. a grapheme reader for everything else: the usual English digraphs
 *      (sh/ch/th/ph/ee/oo/ea/ai), r-coloured vowels (ar/er/or), the "magic e"
 *      that lengthens the vowel before a final silent e (nice, note, huge),
 *      silent letters (kn-, wr-, -mb, gh), and the epenthetic "اِ" that makes a
 *      two-consonant onset pronounceable (اسپیچ, استاپ).
 *
 *  Short vowels are written as Persian diacritics on the preceding consonant
 *  (فَتحه/کَسره/ضَمّه); long vowels as letters (ا/ی/و). The result is what a
 *  Persian speaker would say reading the word — not a phonetic transcription —
 *  which is what the feature is for.
 *
 *  No allocation, no model file, no network: a fixed table and a scanner.
 * ===================================================================== */
#include "mt.h"
#include "../core/osai_common.h"
#include <string.h>
#include <stdio.h>

/* ---- Persian letters ---------------------------------------------- */
#define P_ALEF   "ا"
#define P_ALEF_M "آ"
#define P_BE     "ب"
#define P_PE     "پ"
#define P_TE     "ت"
#define P_SE     "ث"
#define P_JIM    "ج"
#define P_CHE    "چ"
#define P_HE     "ح"
#define P_KHE    "خ"
#define P_DAL    "د"
#define P_ZAL    "ذ"
#define P_RE     "ر"
#define P_ZE     "ز"
#define P_ZHE    "ژ"
#define P_SIN    "س"
#define P_SHIN   "ش"
#define P_SAD    "ص"
#define P_ZAD    "ض"
#define P_TA     "ط"
#define P_ZA     "ظ"
#define P_EYN   "ع"
#define P_GHEYN  "غ"
#define P_FE     "ف"
#define P_QAF    "ق"
#define P_KAF    "ک"
#define P_GAFF   "گ"
#define P_LAM    "ل"
#define P_MIM    "م"
#define P_NUN    "ن"
#define P_VAV    "و"
#define P_HEH    "ه"
#define P_YE     "ی"

#define M_FATH  "َ"
#define M_KASR  "ِ"
#define M_DAMM  "ُ"

/* ---- the pronunciation dictionary ---------------------------------- */
typedef struct pron { const char *en; const char *fa; } pron;

static const pron PRON[] = {
    /* function words — the ones that appear in every sentence */
    {"the", P_DAL M_KASR}, {"a", P_ALEF M_FATH}, {"an", P_ALEF M_FATH P_NUN},
    {"to", P_TE M_DAMM P_VAV}, {"of", P_ALEF M_DAMM P_VAV},
    {"and", P_ALEF M_FATH P_NUN P_DAL}, {"or", P_ALEF M_DAMM P_VAV P_RE},
    {"is", P_ALEF P_YE P_ZE}, {"are", P_ALEF P_RE}, {"am", P_ALEF M_FATH P_MIM},
    {"be", P_BE P_YE}, {"was", P_VAV P_ALEF P_ZE}, {"were", P_VAV M_KASR P_RE},
    {"been", P_BE P_YE P_NUN},
    {"you", P_YE M_DAMM P_VAV}, {"your", P_YE P_VAV P_RE},
    {"i", P_ALEF_M P_YE}, {"me", P_MIM P_YE}, {"my", P_MIM P_ALEF_M P_YE},
    {"we", P_VAV P_YE}, {"us", P_ALEF M_FATH P_SIN},
    {"he", P_HEH P_YE}, {"she", P_SHIN P_YE}, {"it", P_ALEF P_YE P_TE},
    {"they", P_DAL M_KASR P_YE}, {"them", P_DAL M_KASR P_MIM},
    {"their", P_DAL M_KASR P_RE}, {"there", P_DAL M_KASR P_RE},
    {"this", P_DAL P_YE P_SIN}, {"that", P_DAL M_FATH P_TE},
    {"these", P_DAL P_YE P_ZE}, {"those", P_DAL M_DAMM P_VAV P_ZE},
    {"what", P_VAV P_ALEF P_TE}, {"when", P_VAV M_KASR P_NUN},
    {"where", P_VAV M_KASR P_RE}, {"who", P_HEH M_DAMM P_VAV},
    {"why", P_VAV P_ALEF P_YE}, {"how", P_HEH P_ALEF P_VAV},
    {"which", P_VAV P_YE P_CHE},
    {"with", P_VAV P_YE P_SE}, {"without", P_VAV P_YE P_SE P_ALEF P_VAV P_TE},
    {"for", P_FE P_VAV P_RE}, {"from", P_FE P_RE P_ALEF P_MIM},
    {"on", P_ALEF P_NUN}, {"in", P_ALEF P_YE P_NUN}, {"at", P_ALEF M_FATH P_TE},
    {"by", P_BE P_ALEF P_YE}, {"about", P_ALEF M_FATH P_BE P_ALEF P_VAV P_TE},
    {"after", P_ALEF M_FATH P_FE P_TE M_KASR P_RE},
    {"before", P_BE P_YE P_FE P_VAV P_RE},
    {"not", P_NUN P_ALEF P_TE}, {"no", P_NUN M_DAMM P_VAV},
    {"yes", P_YE M_KASR P_SIN}, {"can", P_KAF M_FATH P_NUN},
    {"cannot", P_KAF M_FATH P_NUN P_ALEF P_TE},
    {"will", P_VAV P_YE P_LAM}, {"would", P_VAV M_DAMM P_DAL},
    {"should", P_SHIN M_DAMM P_DAL}, {"could", P_KAF M_DAMM P_DAL},
    {"must", P_MIM M_FATH P_SIN P_TE},
    {"do", P_DAL M_DAMM P_VAV}, {"does", P_DAL M_FATH P_ZE},
    {"too", P_TE M_DAMM P_VAV},
    {"did", P_DAL P_YE P_DAL}, {"done", P_DAL M_FATH P_NUN},
    {"have", P_HEH M_FATH P_VAV}, {"has", P_HEH M_FATH P_ZE},
    {"had", P_HEH M_FATH P_DAL},

    /* the vocabulary the application talks about */
    {"hello", P_HEH M_KASR P_LAM M_DAMM P_VAV},
    {"hi", P_HEH P_ALEF P_YE},
    {"good", P_GAFF M_DAMM P_DAL}, {"great", P_GAFF P_RE P_YE P_TE},
    {"morning", P_MIM P_VAV P_RE P_NUN P_YE P_NUN P_GAFF},
    {"evening", P_ALEF P_YE P_VAV P_NUN P_YE P_NUN P_GAFF},
    {"night", P_NUN P_ALEF P_YE P_TE}, {"day", P_DAL P_YE},
    {"today", P_TE M_DAMM P_VAV P_DAL P_YE},
    {"tomorrow", P_TE M_DAMM P_VAV P_MIM P_ALEF P_RE M_DAMM P_VAV},
    {"thanks", P_SE M_FATH P_NUN P_KAF P_SIN},
    {"thank", P_SE M_FATH P_NUN P_KAF},
    {"please", P_PE P_LAM P_YE P_ZE},
    {"sorry", P_SIN P_ALEF P_RE P_YE},
    {"excuse", P_ALEF M_KASR P_KAF P_SIN P_KAF P_YE P_VAV P_ZE},
    {"nice", P_NUN P_ALEF P_YE P_SIN},
    {"meet", P_MIM P_YE P_TE}, {"meeting", P_MIM P_YE P_TE P_YE P_NUN P_GAFF},
    {"see", P_SIN P_YE}, {"later", P_LAM P_YE P_TE M_KASR P_RE},
    {"welcome", P_VAV M_KASR P_LAM P_KAF M_FATH P_MIM},
    {"speech", P_ALEF M_KASR P_SIN P_PE P_YE P_CHE},
    {"recognition", P_RE M_KASR P_KAF P_ALEF P_GAFF P_NUN P_YE P_SHIN M_KASR P_NUN},
    {"recognize", P_RE M_KASR P_KAF P_ALEF P_GAFF P_NUN P_ALEF P_YE P_ZE},
    {"audio", P_ALEF P_DAL P_YE P_VAV},
    {"text", P_TE M_KASR P_KAF P_SIN P_TE},
    {"model", P_MIM P_ALEF P_DAL M_KASR P_LAM},
    {"models", P_MIM P_ALEF P_DAL M_KASR P_LAM P_ZE},
    {"language", P_LAM M_FATH P_NUN P_GAFF P_VAV P_YE P_JIM},
    {"translate", P_TE P_RE P_ALEF P_NUN P_SIN P_LAM P_YE P_TE},
    {"translation", P_TE P_RE P_ALEF P_NUN P_SIN P_LAM P_YE P_SHIN M_KASR P_NUN},
    {"system", P_SIN P_YE P_SIN P_TE M_KASR P_MIM},
    {"software", P_SIN P_ALEF P_FE P_TE P_VAV P_RE},
    {"computer", P_KAF M_DAMM P_MIM P_PE P_YE P_VAV P_TE M_KASR P_RE},
    {"microphone", P_MIM P_ALEF P_YE P_KAF P_RE M_DAMM P_FE P_VAV P_NUN},
    {"recording", P_RE M_KASR P_KAF P_VAV P_RE P_DAL P_YE P_NUN P_GAFF},
    {"record", P_RE M_KASR P_KAF P_VAV P_RE P_DAL},
    {"start", P_ALEF M_KASR P_SIN P_TE P_ALEF P_RE P_TE},
    {"stop", P_ALEF M_KASR P_SIN P_TE P_ALEF P_PE},
    {"open", P_ALEF M_DAMM P_VAV P_PE M_KASR P_NUN},
    {"close", P_KAF P_LAM M_DAMM P_VAV P_ZE},
    {"save", P_SIN P_YE P_VAV}, {"delete", P_DAL P_YE P_LAM P_YE P_TE},
    {"help", P_HEH M_KASR P_LAM P_PE},
    {"settings", P_SIN M_KASR P_TE P_YE P_NUN P_GAFF P_ZE},
    {"history", P_HEH P_YE P_SIN P_TE P_VAV P_RE P_YE},
    {"name", P_NUN P_YE P_MIM}, {"number", P_NUN M_FATH P_MIM P_BE M_KASR P_RE},
    {"work", P_VAV M_KASR P_RE P_KAF}, {"world", P_VAV M_KASR P_RE P_LAM P_DAL},
    {"word", P_VAV M_KASR P_RE P_DAL},
    {"question", P_KAF P_VAV M_KASR P_SIN P_TE P_CHE M_KASR P_NUN},
    {"answer", P_ALEF P_NUN P_SIN P_VAV P_RE},
    {"because", P_BE P_YE P_KAF P_ALEF P_ZE},
    {"ready", P_RE M_KASR P_DAL P_YE},
    {"download", P_DAL P_ALEF P_VAV P_NUN P_LAM P_VAV P_DAL},
    {"english", P_ALEF P_YE P_NUN P_GAFF P_LAM P_YE P_SHIN},
    {"persian", P_PE M_KASR P_RE P_SHIN M_KASR P_NUN},
    {"information", P_ALEF P_YE P_NUN P_FE P_VAV P_RE P_MIM P_YE P_SHIN M_KASR P_NUN},
    {"happy", P_HEH M_FATH P_PE P_YE},
    {"big", P_BE P_YE P_GAFF}, {"small", P_ALEF M_KASR P_SIN P_MIM P_ALEF P_LAM},
    {"fast", P_FE P_ALEF P_SIN P_TE}, {"slow", P_ALEF M_KASR P_SIN P_LAM M_DAMM P_VAV},
    {"correct", P_KAF M_DAMM P_RE M_KASR P_KAF P_TE},
    {"error", P_ALEF M_KASR P_RE P_VAV P_RE},
    {"success", P_SIN P_ALEF P_KAF P_SIN M_KASR P_SIN},
    {"intelligence", P_ALEF M_KASR P_NUN P_TE M_KASR P_LAM P_YE P_JIM M_KASR P_NUN P_SIN},
    {"artificial", P_ALEF_M P_RE P_TE M_KASR P_FE P_YE P_SHIN M_KASR P_LAM},
    {"goodbye", P_GAFF P_VAV P_DAL P_BE P_ALEF P_YE},
    {"bye", P_BE P_ALEF P_YE},
    {"file", P_FE P_ALEF P_YE P_LAM},
    {"files", P_FE P_ALEF P_YE P_LAM P_ZE},
    {"fine", P_FE P_ALEF P_YE P_NUN},
    {"weather", P_VAV M_KASR P_TE M_KASR P_RE},
    {"much", P_MIM M_FATH P_CHE},
    {"select", P_SIN M_KASR P_LAM M_FATH P_KAF P_TE},
    {"offline", P_ALEF P_FE P_LAM P_ALEF P_YE P_NUN},
    {"online", P_ALEF P_NUN P_LAM P_ALEF P_YE P_NUN},
    {"internet", P_ALEF P_YE P_NUN P_TE M_KASR P_RE P_NUN M_KASR P_TE},
    {"network", P_NUN M_KASR P_TE P_VAV P_RE P_KAF},
    {"data", P_DAL P_YE P_TE P_ALEF},
    {"version", P_VAV M_KASR P_RE P_ZHE M_KASR P_NUN},
    {"update", P_ALEF P_PE P_DAL P_YE P_TE},
    {"install", P_ALEF P_YE P_NUN P_SIN P_TE P_ALEF P_LAM},
    {"remove", P_RE P_YE P_MIM M_DAMM P_VAV},
    {"create", P_KAF P_RE P_YE P_EYN P_TE},
    {"list", P_LAM P_YE P_SIN P_TE},
    {"search", P_SIN M_KASR P_RE P_CHE},
    {"find", P_FE P_ALEF P_YE P_NUN P_DAL},
    {"use", P_YE P_VAV P_ZE}, {"user", P_YE P_VAV P_ZE M_KASR P_RE},
    {"password", P_PE M_FATH P_SIN P_VAV P_RE P_DAL},
    {"different", P_DAL P_YE P_FE P_RE M_KASR P_NUN P_TE},
    {"same", P_SIN P_YE P_MIM},
    {"all", P_ALEF P_LAM}, {"some", P_SIN M_FATH P_MIM},
    {"many", P_MIM M_FATH P_NUN P_YE}, {"more", P_MIM P_VAV P_RE},
    {"less", P_LAM P_YE P_SIN}, {"very", P_VAV M_KASR P_RE P_YE},
    {"just", P_JIM M_FATH P_SIN P_TE}, {"now", P_NUN P_ALEF P_VAV},
    {"then", P_DAL M_KASR P_NUN},
    {"always", P_ALEF P_LAM P_VAV P_YE P_ZE},
    {"never", P_NUN M_KASR P_VAV P_RE},
    {"again", P_ALEF P_GAFF P_YE P_NUN},
    {"also", P_ALEF P_LAM P_SIN M_DAMM P_VAV},
    {"only", P_ALEF M_DAMM P_NUN P_LAM P_YE},
    {"new", P_NUN P_YE P_VAV}, {"old", P_ALEF M_DAMM P_LAM P_DAL},
    {"first", P_FE M_KASR P_RE P_SIN P_TE}, {"last", P_LAM P_ALEF P_SIN P_TE},
    {"next", P_NUN M_KASR P_KAF P_SIN P_TE},
    {"one", P_VAV M_FATH P_NUN}, {"two", P_TE M_DAMM P_VAV},
    {"three", P_SE P_RE P_YE}, {"four", P_FE P_VAV P_RE},
    {"five", P_FE P_ALEF P_YE P_VAV}, {"six", P_SIN P_YE P_KAF P_SIN},
    {"seven", P_SIN M_KASR P_VAV P_NUN},
    {"eight", P_ALEF P_YE P_TE}, {"nine", P_NUN P_ALEF P_YE P_NUN},
    {"ten", P_TE M_KASR P_NUN},
    /* conjunctions, verbs and adjectives that show up in every day speech */
    {"are", P_ALEF P_RE}, {"was", P_VAV P_ALEF P_ZE},
    {"own", P_ALEF M_DAMM P_VAV P_NUN},
    {"trained", P_TE P_RE P_YE P_NUN P_DAL},
    {"train", P_TE P_RE P_YE P_NUN}, {"training", P_TE P_RE P_YE P_NUN P_YE P_NUN P_GAFF},
    {"learn", P_LAM P_RE P_NUN}, {"learning", P_LAM P_RE P_NUN P_YE P_NUN P_GAFF},
    {"teach", P_TE P_YE P_CHE},
    {"speak", P_ALEF M_KASR P_SIN P_PE P_YE P_KAF},
    {"speaking", P_ALEF M_KASR P_SIN P_PE P_YE P_KAF P_YE P_NUN P_GAFF},
    {"listen", P_LAM M_KASR P_SIN P_NUN},
    {"listening", P_LAM M_KASR P_SIN P_NUN P_YE P_NUN P_GAFF},
    {"window", P_VAV M_KASR P_NUN P_DAL M_DAMM P_VAV},
    {"button", P_BE M_FATH P_TE P_NUN},
    {"screen", P_ALEF M_KASR P_SIN P_KAF P_RE P_YE P_NUN},
    {"page", P_PE P_YE P_JIM},
    {"video", P_VAV M_KASR P_DAL P_YE P_VAV},
    {"voice", P_VAV M_DAMM P_YE P_SIN},
    {"sound", P_SIN P_ALEF P_VAV P_NUN P_DAL},
    {"noise", P_NUN M_DAMM P_YE P_ZE},
    {"music", P_MIM P_YE P_VAV P_ZE P_YE P_KAF},
    {"book", P_BE M_DAMM P_KAF},
    {"paper", P_PE P_YE P_PE M_KASR P_RE},
    {"letter", P_LAM M_KASR P_TE M_KASR P_RE},
    {"email", P_ALEF M_KASR P_YE P_MIM P_YE P_LAM},
    {"message", P_MIM M_FATH P_SIN P_ALEF P_JIM},
    {"phone", P_FE M_DAMM P_VAV P_NUN},
    {"office", P_ALEF M_DAMM P_FE P_YE P_SIN},
    {"home", P_HEH M_DAMM P_VAV P_MIM},
    {"house", P_HEH P_ALEF P_VAV P_ZE},
    {"city", P_SIN M_KASR P_TE M_KASR P_YE},
    {"country", P_KAF M_FATH P_NUN P_TE P_RE M_KASR P_YE},
    {"people", P_PE M_KASR P_YE P_PE M_KASR P_LAM},
    {"family", P_FE M_FATH P_MIM M_KASR P_LAM P_YE},
    {"friend", P_FE P_RE P_YE P_NUN P_DAL},
    {"time", P_TE M_KASR P_YE P_MIM},
    {"year", P_YE M_KASR P_RE},
    {"month", P_MIM M_FATH P_NUN P_TE},
    {"week", P_VAV P_YE P_KAF},
    {"hour", P_ALEF P_VAV P_RE},
    {"minute", P_MIM M_KASR P_NUN M_DAMM P_VAV P_TE},
    {"second", P_SIN M_KASR P_KAF P_ALEF P_NUN P_DAL},
    {"money", P_MIM M_DAMM P_VAV P_NUN P_YE},
    {"problem", P_PE P_RE M_DAMM P_PE P_LAM M_KASR P_MIM},
    {"important", P_ALEF M_KASR P_MIM P_PE P_VAV P_RE P_TE M_FATH P_NUN P_TE},
    {"possible", P_PE M_DAMM P_VAV P_SIN M_KASR P_BE P_LAM},
    {"long", P_LAM M_DAMM P_VAV P_NUN P_GAFF},
    {"short", P_SHIN M_DAMM P_VAV P_RE P_TE},
    {"easy", P_ALEF M_KASR P_YE P_ZE P_YE},
    {"hard", P_HEH P_ALEF P_RE P_DAL},
    {"beautiful", P_BE M_DAMM P_VAV P_TE M_KASR P_FE P_LAM},
    {"happy", P_HEH M_FATH P_PE P_YE},
    {"sad", P_SIN M_FATH P_DAL},
    {"cold", P_KAF M_DAMM P_VAV P_LAM P_DAL},
    {"hot", P_HEH M_DAMM P_VAV P_TE},
    {"sun", P_SIN M_DAMM P_VAV P_NUN},
    {"rain", P_RE P_YE P_NUN},
    {"water", P_VAV M_DAMM P_VAV P_TE M_KASR P_RE},
    {"food", P_FE M_DAMM P_VAV P_DAL},
    {"night", P_NUN P_ALEF M_KASR P_TE},
    {"weekend", P_VAV P_YE P_KAF P_ALEF P_NUN P_DAL},
    {"job", P_JIM M_DAMM P_VAV P_BE},
    {"school", P_ALEF M_KASR P_SIN P_KAF M_DAMM P_VAV P_LAM},
    {"university", P_YE M_DAMM P_VAV P_NUN M_KASR P_VAV P_RE P_SIN M_KASR P_TE P_YE},
    {"program", P_PE P_RE P_VAV P_GAFF P_RE P_ALEF P_MIM},
    {"application", P_ALEF M_FATH P_PE P_LAM P_YE P_KAF P_YE P_SHIN M_KASR P_NUN},
    {"device", P_DAL M_KASR P_VAV P_ALEF_M P_YE P_SIN},
    {"machine", P_MIM M_FATH P_SHIN P_YE P_NUN},
    {"engine", P_ALEF M_KASR P_NUN P_JIM P_YE P_NUN},
    {"result", P_RE P_MIM M_DAMM P_ZE P_ALEF P_LAM P_TE},
    {"input", P_ALEF M_KASR P_NUN P_PE M_DAMM P_VAV P_TE},
    {"output", P_ALEF M_DAMM P_VAV P_TE P_PE M_DAMM P_VAV P_TE},
    {"quality", P_KAF P_VAV P_ALEF P_LAM M_KASR P_TE P_YE},
    {"speed", P_ALEF M_KASR P_SIN P_PE P_YE P_DAL},
    {"size", P_SIN M_KASR P_YE P_ZE},
    {"memory", P_MIM M_FATH P_MIM P_RE P_YE},
    {"storage", P_ALEF M_KASR P_SIN P_TE P_VAV P_RE P_JIM},
    {"account", P_ALEF M_FATH P_KAF P_VAV P_NUN P_TE},
    {"ticket", P_TE M_KASR P_KAF P_TE},
    

};
#define PRON_N ((int)(sizeof PRON / sizeof PRON[0]))

/* ---- tokens -------------------------------------------------------- */
typedef enum {
    K_NONE = 0,
    /* consonants */
    K_B, K_P, K_T, K_D, K_K, K_G, K_F, K_V, K_S, K_Z, K_SH, K_ZH, K_CH, K_J,
    K_H, K_KH, K_M, K_N, K_L, K_R, K_W, K_Y, K_TH, K_DH, K_NG,
    /* vowels: short vowels are diacritics, long ones are letters */
    K_A, K_E, K_I, K_O, K_U, K_EE, K_OO, K_AI, K_EI, K_OI, K_AU, K_AR, K_ER, K_OR,
    K_SILENT,
    K_COUNT
} tok;

static const char *TOK_FA[K_COUNT] = {
    [K_NONE] = "",
    [K_B] = P_BE, [K_P] = P_PE, [K_T] = P_TE, [K_D] = P_DAL,
    [K_K] = P_KAF, [K_G] = P_GAFF, [K_F] = P_FE, [K_V] = P_VAV,
    [K_S] = P_SIN, [K_Z] = P_ZE, [K_SH] = P_SHIN, [K_ZH] = P_ZHE,
    [K_CH] = P_CHE, [K_J] = P_JIM, [K_H] = P_HEH, [K_KH] = P_KHE,
    [K_M] = P_MIM, [K_N] = P_NUN, [K_L] = P_LAM, [K_R] = P_RE,
    [K_W] = P_VAV, [K_Y] = P_YE, [K_TH] = P_TE, [K_DH] = P_DAL,
    [K_NG] = P_NUN P_GAFF,
    [K_A] = M_FATH, [K_E] = M_KASR, [K_I] = M_KASR, [K_O] = M_DAMM, [K_U] = M_FATH,
    [K_EE] = P_YE, [K_OO] = P_VAV, [K_AI] = P_ALEF P_YE, [K_EI] = P_YE,
    [K_OI] = P_VAV P_YE, [K_AU] = P_ALEF P_VAV,
    [K_AR] = P_ALEF P_RE, [K_ER] = M_KASR P_RE, [K_OR] = P_VAV P_RE,
    [K_SILENT] = "",
};

static bool is_short_vowel(tok t) {
    return t == K_A || t == K_E || t == K_I || t == K_O || t == K_U;
}
static bool is_vowel(tok t) {
    return is_short_vowel(t) || t == K_EE || t == K_OO || t == K_AI || t == K_EI ||
           t == K_OI || t == K_AU || t == K_AR || t == K_ER || t == K_OR;
}
static bool is_consonant(tok t) {
    return t != K_NONE && t != K_SILENT && !is_vowel(t);
}

/* ---- spelling rules ------------------------------------------------ */
typedef struct rule { const char *pat; tok t[3]; } rule;

static const rule RULES[] = {
    /* four+ letters */
    { "ough", { K_OO } },          { "augh", { K_OO } },
    { "eigh", { K_EI } },
    /* three letters */
    { "tion", { K_SH, K_E, K_N } },{ "sion", { K_ZH, K_E, K_N } },
    { "tia", { K_SH, K_E } },     { "tio", { K_SH, K_E } },
    { "cia", { K_SH, K_E } },     { "cio", { K_SH, K_E } },
    { "cial", { K_SH, K_E, K_L } },{ "tial", { K_SH, K_E, K_L } },
    { "ture", { K_CH, K_E, K_R } },{ "ight", { K_AI } },
    { "tch",  { K_CH } },          { "dge",  { K_J } },
    { "air",  { K_ER } },          { "ear",  { K_ER } },
    { "eer",  { K_ER } },          { "oor",  { K_OR } },
    { "our",  { K_OR } },
    /* two letters — consonants */
    { "ch", { K_CH } }, { "sh", { K_SH } }, { "th", { K_TH } },
    { "ph", { K_F } },  { "wh", { K_W } },  { "ck", { K_K } },
    { "qu", { K_K } },  { "ng", { K_NG } }, { "kn", { K_N } },
    { "wr", { K_R } },  { "gh", { K_SILENT } }, { "mb", { K_M } },
    /* two letters — vowels */
    { "ee", { K_EE } }, { "ea", { K_EE } }, { "oo", { K_OO } },
    { "ou", { K_AU } }, { "ow", { K_AU } }, { "oa", { K_O } },
    { "ai", { K_AI } }, { "ay", { K_EI } }, { "ei", { K_EI } },
    { "ey", { K_EI } }, { "ie", { K_EE } }, { "oi", { K_OI } },
    { "oy", { K_OI } }, { "au", { K_AU } }, { "aw", { K_AU } },
    { "ue", { K_OO } }, { "ui", { K_OO } }, { "ar", { K_AR } },
    { "er", { K_ER } }, { "ir", { K_ER } }, { "or", { K_OR } },
    { "ur", { K_ER } },
    /* one letter */
    { "a", { K_A } }, { "b", { K_B } }, { "c", { K_K } }, { "d", { K_D } },
    { "e", { K_E } }, { "f", { K_F } }, { "g", { K_G } }, { "h", { K_H } },
    { "i", { K_I } }, { "j", { K_J } }, { "k", { K_K } }, { "l", { K_L } },
    { "m", { K_M } }, { "n", { K_N } }, { "o", { K_O } }, { "p", { K_P } },
    { "q", { K_K } }, { "r", { K_R } }, { "s", { K_S } }, { "t", { K_T } },
    { "u", { K_U } }, { "v", { K_V } }, { "w", { K_W } }, { "x", { K_K } },
    { "y", { K_Y } }, { "z", { K_Z } },
};
#define RULES_N ((int)(sizeof RULES / sizeof RULES[0]))

/* The long vowel a "magic e" produces. */
static tok lengthen(tok short_v) {
    switch (short_v) {
        case K_A:  return K_EI;    /* name  */
        case K_E:  return K_EE;    /* theme */
        case K_I:  return K_AI;    /* nice  */
        case K_O:  return K_O;     /* note  */
        case K_U:  return K_OO;    /* huge  */
        default:   return short_v;
    }
}

static int tokenize(const char *w, tok *out, int max) {
    size_t len = strlen(w);
    int n = 0;
    for (size_t i = 0; i < len && n < max; ) {
        bool matched = false;
        for (int r = 0; r < RULES_N && !matched; r++) {
            size_t pl = strlen(RULES[r].pat);
            if (i + pl > len) continue;
            if (strncmp(w + i, RULES[r].pat, pl) != 0) continue;
            for (int k = 0; k < 3; k++) {
                if (RULES[r].t[k] == K_NONE) continue;
                if (n < max) out[n++] = RULES[r].t[k];
            }
            i += pl;
            matched = true;
        }
        if (!matched) i++;
    }
    /* --- post-pass 1: a silent final "e" ---
     * "nice" arrives as n, i, c, e. The e is silent and turns the i into a long
     * "ai" sound. Double consonants (ll, ss, tt) keep the vowel short ("little",
     * "class"), which is what English actually does. */
    if (n >= 2 && out[n - 1] == K_E) {
        bool doubled = n >= 3 && out[n - 2] == out[n - 3] && is_consonant(out[n - 2]);
        if (doubled) {
            n--;                                  /* just drop the e */
        } else if (n >= 3 && is_consonant(out[n - 2]) && is_short_vowel(out[n - 3])) {
            out[n - 3] = lengthen(out[n - 3]);
            n--;                                  /* drop the e, keep the vowel change */
        } else if (n == 2) {
            n--;                                  /* "be", "he": e is silent */
        } else {
            n--;
        }
    }
    /* --- post-pass: a final "s" is a "z" sound after a vowel or a voiced
     * consonant ("files", "words", "languages"); after an unvoiced one it stays
     * an "s" ("texts", "lists"). */
    if (n >= 2 && out[n - 1] == K_S) {
        tok prev = out[n - 2];
        bool voiced = is_vowel(prev) || prev == K_B || prev == K_D || prev == K_G ||
                      prev == K_V || prev == K_L || prev == K_M || prev == K_N ||
                      prev == K_R || prev == K_NG || prev == K_J;
        if (voiced) out[n - 1] = K_Z;
    }
    /* --- post-pass 2: "-ed" is a "d" sound after most consonants --- */
    if (n >= 2 && out[n - 1] == K_D && out[n - 2] == K_E) out[n - 2] = K_SILENT;
    /* --- post-pass 3: "-es" is a "z" sound after a sibilant --- */
    if (n >= 2 && out[n - 1] == K_Z && out[n - 2] == K_E &&
        n >= 3 && (out[n - 3] == K_S || out[n - 3] == K_Z || out[n - 3] == K_SH ||
                   out[n - 3] == K_CH || out[n - 3] == K_J))
        out[n - 2] = K_SILENT;
    return n;
}

/* ---- rendering ----------------------------------------------------- */
static void emit(char *out, size_t cap, size_t *o, const char *s) {
    if (!s || !*s) return;
    size_t l = strlen(s);
    if (*o + l + 1 > cap) return;
    memcpy(out + *o, s, l);
    *o += l;
    out[*o] = 0;
}

/* Appends `word` rendered from the rules. */
static void render_rule_word(const char *word, char *out, size_t cap, size_t *o) {
    tok t[160];
    int n = tokenize(word, t, 160);
    if (n <= 0) return;

    /* Epenthetic alef: Persian cannot start with two consonants, so "speech"
     * becomes "اِسپیچ" and "stop" becomes "اِستاپ". */
    if (n >= 2 && is_consonant(t[0]) && is_consonant(t[1]))
        emit(out, cap, o, P_ALEF M_KASR);

    for (int i = 0; i < n; i++) {
        tok cur = t[i];
        if (cur == K_SILENT || cur == K_NONE) continue;

        if (is_vowel(cur)) {
            bool lone = (i == 0);
            bool after_vowel = i > 0 && is_vowel(t[i - 1]);
            if (is_short_vowel(cur)) {
                /* a short vowel needs a consonant to carry it; a word-initial
                 * one is carried by an alef, and one after another vowel takes
                 * a hamza-free alef seat */
                if (lone || after_vowel) emit(out, cap, o, P_ALEF);
                emit(out, cap, o, TOK_FA[cur]);
            } else {
                if (lone) emit(out, cap, o, P_ALEF);
                emit(out, cap, o, TOK_FA[cur]);
            }
            continue;
        }

        /* consonant: skip a doubled letter ("little" is one ل with no geminate
         * in this reading) */
        if (i > 0 && t[i - 1] == cur) continue;

        emit(out, cap, o, TOK_FA[cur]);

        tok next = (i + 1 < n) ? t[i + 1] : K_NONE;
        if (is_short_vowel(next)) {
            emit(out, cap, o, TOK_FA[next]);
            t[i + 1] = K_SILENT;          /* consumed as a diacritic */
        } else if (is_consonant(next)) {
            /* a consonant cluster in the middle keeps a kasra so the word
             * stays readable: رِکاگنیشِن, اسپیچ */
            emit(out, cap, o, M_KASR);
        }
    }
}

/* A dictionary word, or NULL. Used by the reader in pronounce.c for English
 * irregular spellings; keeping one dictionary in one place means the two readers
 * cannot drift apart. */
const char *mt_pronounce_known(const char *word) {
    if (!word) return NULL;
    for (int i = 0; i < PRON_N; i++)
        if (osa_streq(PRON[i].en, word)) return PRON[i].fa;
    return NULL;
}

/* ---------------------------------------------------------------------
 *  Public entry point
 * ------------------------------------------------------------------- */
bool mt_pronounce(const char *text, mt_lang from, char *out, size_t cap) {
    if (!out || cap < 8) return false;
    out[0] = 0;
    if (!text || !*text) return false;
    if (from == MT_AUTO) from = mt_detect_lang(text);
    /* Persian input needs no transliteration: it is already readable */
    if (from == MT_FA) return false;

    size_t o = 0;
    char word[96];
    int wl = 0;
    for (const char *p = text;; p++) {
        unsigned char ch = (unsigned char)*p;
        bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (letter && wl < (int)sizeof word - 1) {
            word[wl++] = (char)((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
            continue;
        }
        if (wl > 0) {
            word[wl] = 0;
            const char *known = NULL;
            for (int i = 0; i < PRON_N; i++)
                if (osa_streq(PRON[i].en, word)) { known = PRON[i].fa; break; }
            if (known) emit(out, cap, &o, known);
            else render_rule_word(word, out, cap, &o);
            wl = 0;
        }
        if (!ch) break;
        if (ch == ' ' || ch == '\t' || ch == '\n') {
            if (o > 0 && out[o - 1] != ' ') emit(out, cap, &o, " ");
        } else if (ch < 0x80) {
            char one[2] = { (char)ch, 0 };
            emit(out, cap, &o, one);
        } else {
            /* copy a complete UTF-8 sequence (names, symbols in mixed text) */
            int need = 1;
            if ((ch & 0xE0) == 0xC0) need = 2;
            else if ((ch & 0xF0) == 0xE0) need = 3;
            else if ((ch & 0xF8) == 0xF0) need = 4;
            char tmp[6] = { 0 };
            for (int k = 0; k < need && p[k]; k++) tmp[k] = p[k];
            emit(out, cap, &o, tmp);
            p += need - 1;
        }
    }
    while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
    return o > 0;
}
