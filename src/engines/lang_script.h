/* =====================================================================
 *  OpenSpeech AI — which script a language is written in
 *
 *  Two places need to know this, and they must agree:
 *    - the pronunciation reader (pronounce.c), which picks a reader by script;
 *    - the translation worker, which has to notice when a generative model
 *      answered the request instead of translating it ("Hello! How can I assist
 *      you today?" is not a Persian translation of «hello»).
 *
 *  The check is deliberately coarse: it decides whether the output is *written in
 *  the script the target language uses*, not whether the translation is good. A
 *  model that chats back fails it; a translation with imperfect wording passes it.
 * ===================================================================== */
#ifndef OSAI_LANG_SCRIPT_H
#define OSAI_LANG_SCRIPT_H
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum osa_script {
    OSA_SCRIPT_NONE = 0,
    OSA_SCRIPT_LATIN,
    OSA_SCRIPT_ARABIC,      /* Arabic, Persian, Urdu, Pashto ... */
    OSA_SCRIPT_CYRILLIC,
    OSA_SCRIPT_GREEK,
    OSA_SCRIPT_HEBREW,
    OSA_SCRIPT_HAN,
    OSA_SCRIPT_KANA,
    OSA_SCRIPT_HANGUL,
    OSA_SCRIPT_THAI,
    OSA_SCRIPT_DEVANAGARI,
    OSA_SCRIPT_BENGALI,
    OSA_SCRIPT_TAMIL,
    OSA_SCRIPT_TELUGU,
    OSA_SCRIPT_KANNADA,
    OSA_SCRIPT_MALAYALAM,
    OSA_SCRIPT_GUJARATI,
    OSA_SCRIPT_GURMUKHI,
    OSA_SCRIPT_SINHALA,
    OSA_SCRIPT_GEORGIAN,
    OSA_SCRIPT_ARMENIAN,
    OSA_SCRIPT_ETHIOPIC,
    OSA_SCRIPT_KHMER,
    OSA_SCRIPT_LAO,
    OSA_SCRIPT_MYANMAR,
    OSA_SCRIPT_TIBETAN
} osa_script;

/* The script a language is normally written in, from its ISO tag ("fa", "en-GB",
 * "zh-Hans"). OSA_SCRIPT_LATIN for every tag that is not in the table, so an
 * unknown language does not produce a false accusation. */
osa_script osa_script_for_tag(const char *tag);

/* How many letters of `text` are in `s` (0 for text without letters). */
float osa_script_share(const char *text, osa_script s);

/* True when `text` has at least one letter in `s`. */
bool osa_text_in_script(const char *text, osa_script s);

/* The check the worker runs on a translation:
 *   - the output must contain letters at all;
 *   - it must be written in the target language's script, unless the source is
 *     already in that script (a name, a number, a code, or a language pair that
 *     shares one script — the model may legitimately return the input unchanged).
 * `src` may be NULL or "auto". */
bool osa_translation_in_target_script(const char *output, const char *dst_tag,
                                      const char *input, const char *src_tag);

/* A name a person recognises, for the prompt: "Persian (fa)". Never empty. */
const char *osa_language_name(const char *tag);

#ifdef __cplusplus
}
#endif

#endif
