/* bidi.h — Unicode Bidirectional Algorithm (UAX #9), UI subset.
 * Handles Persian/Arabic (AL), Hebrew (R), Latin (L), European/Arabic numbers,
 * neutrals, and explicit embeddings/overrides. Produces visual-order runs. */
#ifndef OSAI_BIDI_H
#define OSAI_BIDI_H

#include "osai_common.h"

#define BIDI_MAX_RUNES 4096
#define BIDI_MAX_RUNS  512

typedef struct bidi_run {
    size_t   start;     /* byte offset in source */
    size_t   len;       /* byte length */
    int      level;     /* resolved embedding level; odd = RTL */
    bool     rtl;
} bidi_run;

/* Resolves the paragraph. Runs are returned in visual order (left to right).
 * Returns the run count (0 when the string is empty). */
int  bidi_paragraph(const char *utf8, bidi_run *runs, int max_runs, bool *out_para_rtl);
bool bidi_is_paragraph_rtl(const char *utf8);

#endif
