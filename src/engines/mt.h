/* =====================================================================
 *  OpenSpeech AI — translation engine (fa <-> en)
 *
 *  The engine is a separate module from ASR on purpose: the domain design
 *  requires the two to be swappable and independently replaceable (Argos /
 *  OPUS-MT models are plugged in through the same .osmt container).
 *
 *  Two backends ship in the single executable:
 *
 *    "phrasebook"  A hand-curated fa<->en phrase table plus a word-level
 *                  gloss table with a small context disambiguator. Fully
 *                  offline, deterministic, zero downloads. This is what the
 *                  shipped binary uses out of the box.
 *
 *    "osmt"        A trained model container (.osmt) holding a word/phrase
 *                  lexicon with probabilities and optional reordering rules.
 *                  Loaded from a file when the user installs an Argos/OPUS-MT
 *                  derived pack, so a stronger model can replace the
 *                  phrasebook without touching ASR.
 *
 *  Nothing here phones home: translation is local, and the container format
 *  carries its licence + source URL so it can be audited.
 * ===================================================================== */
#ifndef OSAI_MT_H
#define OSAI_MT_H

#include <stdbool.h>
#include <stdint.h>
#include "../core/osai_common.h"

#define MT_MAX_SEGMENTS 128
#define MT_MAX_TEXT     2048

typedef enum mt_lang { MT_AUTO = 0, MT_FA = 1, MT_EN = 2 } mt_lang;

/* Register of the produced translation. The phrasebook keeps one entry per
 * phrase in each register; a phrase with no variant for the requested register
 * falls back to the neutral one, so the setting can never produce an empty
 * result. */
typedef enum mt_style {
    MT_STYLE_NEUTRAL = 0,   /* plain, bookish: what the phrase table lists  */
    MT_STYLE_FORMAL  = 1,   /* formal / literary: شما، خواهشمندم، نمی‌باشد  */
    MT_STYLE_CASUAL  = 2    /* everyday speech: تو، ممنون، نمیشه           */
} mt_style;

#define MT_STYLE_COUNT 3

/* UI names (Persian + English) of a style, by index. */
const char *mt_style_label(mt_style s, bool persian);
const char *mt_style_key(mt_style s);

typedef struct mt_model mt_model;

/* One translated unit. `start`/`end` are character offsets into the source
 * string, so the UI can highlight the source and its translation together. */
typedef struct mt_piece {
    int      src_start, src_end;
    char     src[128];
    char     dst[256];
    float    confidence;
} mt_piece;

typedef struct mt_result {
    char     text[MT_MAX_TEXT];
    mt_lang  src_lang, dst_lang;
    mt_piece pieces[MT_MAX_SEGMENTS];
    int      n_pieces;
    float    confidence;
    double   elapsed_ms;
    char     backend[24];
    char     model_id[64];
} mt_result;

/* ---- lifecycle ---------------------------------------------------- */

/* Builtin phrasebook model (no files required). */
mt_model *mt_model_builtin(osa_error *err);

/* Load a trained .osmt container. Returns NULL on error. */
mt_model *mt_model_load(const char *path, osa_error *err);

/* Write the current model to a .osmt container (round-trips load()). */
bool mt_model_save(const mt_model *m, const char *path, osa_error *err);

void mt_model_free(mt_model *m);

const char *mt_model_id(const mt_model *m);
const char *mt_model_name(const mt_model *m);
const char *mt_model_license(const mt_model *m);
const char *mt_model_source(const mt_model *m);
const char *mt_model_backend(const mt_model *m);

/* ---- offline word packs ------------------------------------------- *
 * The phrasebook and the word gloss table are compiled in. An installed word
 * pack (a plain UTF-8 "source<TAB>translation" file, one pair per line,
 * comments with '#') extends the gloss without replacing it: two passes are
 * made, the pack first and the builtin table second, so a pack can correct a
 * word but never remove the coverage the application ships with.
 *
 * The path becomes part of the model, which is why the app loads the packs at
 * start-up instead of translating with whatever it happened to find.
 * ------------------------------------------------------------------- */

/* Words currently loaded from packs (0 when only the builtin table is used). */
int mt_model_extra_words(const mt_model *m);

/* Add every pair in `path` to `m`'s gloss. `src` is the language of the first
 * column; a pair whose columns are the wrong way round is stored swapped, so a
 * pack cannot produce a translation in the wrong direction. Returns the number
 * of pairs added, or -1 on error. Re-adding a pair replaces it. */
int mt_model_add_word_pack(mt_model *m, const char *path, mt_lang src, osa_error *err);

/* Verify that `path` looks like a word pack for `src` (at least one usable
 * line) without changing `m`. Used by the installer before it copies a file. */
bool mt_word_pack_check(const char *path, mt_lang src, int *n_pairs, osa_error *err);

/* ---- translation -------------------------------------------------- */

/* Translate `text`. `src` = MT_AUTO detects the language from the script
 * (Persian vs Latin codepoints) — reliable for this language pair.
 * `out` is fully overwritten; the caller must not free its contents. */
bool mt_translate(const mt_model *m, const char *text, mt_lang src,
                  mt_result *out, osa_error *err);

/* Same, with an explicit register. */
bool mt_translate_styled(const mt_model *m, const char *text, mt_lang src,
                         mt_style style, mt_result *out, osa_error *err);

/* ---- pronunciation -------------------------------------------------
 * Persian transliteration of the produced text (and of any input), written in
 * Persian letters with full diacritics (اعراب) and a syllable separator, e.g.
 *   "nice to meet you"  ->  «نایس تُو میت یُو»
 * The result is produced for a Persian reader, not for a phonetician: it is
 * what the Persian interface shows under a translation so the user can say the
 * sentence out loud.
 *
 * `text` is the language the romanisation should start from; when it is MT_AUTO
 * the script of `text` decides. Returns false only for empty input.
 */
bool mt_pronounce(const char *text, mt_lang from, char *out, size_t cap);

/* The same, for a language named by its tag ("de", "ru", "el-GR", "ar" ...).
 * This is the entry point the Translate screen uses: it follows the language of
 * the translation, not one hard-coded pair, so آوانویسی works for every
 * language the app can translate.
 *
 * `unsupported` is set to true when the text is written in a script that cannot
 * be read letter by letter without a dictionary (Chinese, Japanese, Korean,
 * Thai, the Indic scripts, Georgian, Armenian, Ethiopic, Khmer, Lao, Myanmar).
 * The output then keeps those sentences as they are, and the caller shows a
 * short note instead of pretending to know how they sound. */
bool mt_pronounce_ex(const char *text, const char *tag, char *out, size_t cap,
                     bool *unsupported);

/* Which reader handles `tag`: "latin", "cyrillic", "greek", "arabic",
 * "persian", "hebrew", "unsupported", "unknown". For tests and for the note in
 * the UI. */
const char *mt_pronounce_script(const char *tag);

/* The known-pronunciation dictionary: the Persian reading of an English word, or
 * NULL when the word is not in it. Shared with the multi-script reader so both
 * read "you" the same way. */
const char *mt_pronounce_known(const char *word);

/* Language of a text, by script detection. Never MT_AUTO. */
mt_lang mt_detect_lang(const char *text);

/* ISO-ish tag for a language: "fa" / "en" / "" for MT_AUTO. */
const char *mt_lang_tag(mt_lang l);

/* ---- quality helpers ---------------------------------------------- */

/* BLEU-ish precision on whitespace tokens, in [0,1]. Used by the test-suite
 * and by the evaluation screen; 1.0 == identical token sequence. */
double mt_score(const char *reference, const char *hypothesis);

/* Human/UI name of a backend string. */
const char *mt_backend_name(const char *backend);

#endif /* OSAI_MT_H */
