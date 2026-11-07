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

/* ---- translation -------------------------------------------------- */

/* Translate `text`. `src` = MT_AUTO detects the language from the script
 * (Persian vs Latin codepoints) — reliable for this language pair.
 * `out` is fully overwritten; the caller must not free its contents. */
bool mt_translate(const mt_model *m, const char *text, mt_lang src,
                  mt_result *out, osa_error *err);

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
