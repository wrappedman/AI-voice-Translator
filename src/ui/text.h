/* text.h — HarfBuzz shaping + FreeType rasterization on the software canvas.
 * Persian/Arabic joining, RTL runs, Latin, wrapping, ellipsis, mixed bidi. */
#ifndef OSAI_TEXT_H
#define OSAI_TEXT_H

#include "canvas.h"
#include "../core/osai_platform.h"

typedef struct osa_font osa_font;

typedef enum { OSA_FW_REGULAR, OSA_FW_MEDIUM, OSA_FW_BOLD, OSA_FW_MONO } osa_font_weight;

bool   text_init(void);
void   text_shutdown(void);
osa_font *text_font_load(const char *path, osa_font_weight w);
osa_font *text_font_default(osa_font_weight w);      /* auto-loads bundled Vazirmatn */
void   text_font_release(osa_font *f);

/* Text drawing. `size` is the pixel em size. Returns advance width in px. */
typedef struct text_opts {
    float    max_width;      /* 0 = unlimited; wraps on overflow */
    int      align;          /* 0 left, 1 center, 2 right */
    bool     rtl;            /* force RTL paragraph; false = auto-detect */
    float    line_height;    /* 0 = 1.45 * size */
    int      max_lines;      /* 0 = unlimited */
    bool     ellipsis;       /* add … when clipped */
    float    letter_spacing;
} text_opts;

text_opts text_default_opts(float size);
float text_measure(const osa_font *f, float size, const char *utf8);
/* Draws single-line text with baseline placed at y_baseline. */
float text_draw(osa_canvas *c, const osa_font *f, float size, osa_color col,
                float x, float y_baseline, const char *utf8);
/* Lays out (wraps) text inside `box`; returns total height used. */
float text_draw_box(osa_canvas *c, const osa_font *f, float size, osa_color col,
                    osa_rect box, const char *utf8, text_opts o);
float text_box_height(const osa_font *f, float size, osa_rect box, const char *utf8, text_opts o);
/* Truncate to fit width, appending an ellipsis rune. Writes into out. */
void  text_ellipsize(const osa_font *f, float size, const char *utf8, float max_w,
                     char *out, size_t cap);
/* Metrics */
float text_ascent(const osa_font *f, float size);
float text_descent(const osa_font *f, float size);
float text_line_height(const osa_font *f, float size);
/* Direction detection: true when the first strong character is RTL. */
bool  text_is_rtl(const char *utf8);
/* Converts ASCII digits to Persian digits when lang=="fa". */
void  text_localize_digits(const char *utf8, const char *lang, char *out, size_t cap);
/* Word-wrap the utf8 string into lines (NUL-separated in out), returns count. */
int   text_wrap_lines(const osa_font *f, float size, const char *utf8, float max_w,
                      char *out, size_t cap, int max_lines);

#endif
