/* =====================================================================
 *  OpenSpeech AI — UI kit
 *
 *  A small immediate-mode widget layer over the canvas/text primitives.
 *  The design goal is a desktop shell that looks deliberate on both a 1080p
 *  monitor and a 1366x768 laptop, and that lays out correctly when the
 *  interface language is Persian (right-to-left) or English.
 *
 *  Everything is measured from `osa_ui.layout`, so the RTL switch flips the
 *  whole shell without per-widget special cases.
 * ===================================================================== */
#ifndef OSAI_UI_H
#define OSAI_UI_H

#include <stdbool.h>
#include "../core/osai_common.h"
#include "canvas.h"
#include "text.h"

/* ---- design tokens ------------------------------------------------ */
typedef struct ui_theme {
    osa_color bg;
    osa_color panel;
    osa_color panel_alt;
    osa_color sidebar;
    osa_color border;
    osa_color border_soft;
    osa_color text;
    osa_color text_dim;
    osa_color text_faint;
    osa_color accent;
    osa_color accent_hover;
    osa_color accent_soft;
    osa_color accent2;        /* secondary hue: translation / MT surfaces */
    osa_color danger;
    osa_color success;
    osa_color warning;
    osa_color selection;
    osa_color wave;
    osa_color wave_bg;
} ui_theme;

/* ---- layout ------------------------------------------------------- */
typedef struct ui_layout {
    bool   rtl;             /* Persian interface                    */
    float  scale;           /* 1.0 at 96 dpi, 1.5 at 144 dpi        */
    float  pad;             /* base spacing unit                    */
    float  radius;
    int    win_w, win_h;
} ui_layout;

/* ---- interaction state ------------------------------------------- */
typedef struct ui_input {
    float  mx, my;          /* current pointer                      */
    bool   down;            /* left button held                     */
    bool   clicked;         /* released this frame                  */
    float  wheel;
    int    key;             /* last key code, 0 when none           */
    bool   shift, ctrl;
    /* Text typed this frame, as Unicode scalar values. The platform layer
     * decodes WM_CHAR / XLookupString into this buffer; widgets consume it.
     * Backspace arrives as 8, Enter as 13, Tab as 9. */
    uint32_t chars[32];
    int      n_chars;
} ui_input;

typedef struct ui_context {
    osa_canvas  *c;
    osa_font    *reg;
    osa_font    *bold;
    ui_theme     t;
    ui_layout    L;
    ui_input     in;
    /* hot/active widget bookkeeping for immediate mode */
    int          hot_id;
    int          active_id;
    int          next_id;
    bool         consumed_click;
    char         tooltip[256];
} ui_context;

/* ---- lifecycle ---------------------------------------------------- */
bool ui_init(ui_context *u, osa_canvas *c);
void ui_shutdown(ui_context *u);
void ui_set_theme(ui_context *u, bool dark);
void ui_set_rtl(ui_context *u, bool rtl);
void ui_set_scale(ui_context *u, float scale);

/* ---- primitives --------------------------------------------------- */
void  ui_panel(ui_context *u, osa_rect r);
void  ui_card(ui_context *u, osa_rect r);
void  ui_separator(ui_context *u, osa_rect r);
void  ui_label(ui_context *u, osa_rect r, const char *text, float size, osa_color col, int align);
void  ui_label_wrap(ui_context *u, osa_rect r, const char *text, float size, osa_color col);
float ui_text_h(ui_context *u, float size);

/* Buttons. `id` must be unique and stable per widget. Returns true on click. */
bool  ui_button(ui_context *u, int id, osa_rect r, const char *label, bool primary);
bool  ui_button_icon(ui_context *u, int id, osa_rect r, const char *glyph,
                     const char *label, bool active);
bool  ui_toggle(ui_context *u, int id, osa_rect r, bool *value, const char *label);
bool  ui_tab(ui_context *u, int id, osa_rect r, const char *label, bool selected);

/* Vertical nav item used by the sidebar. */
bool  ui_nav_item(ui_context *u, int id, osa_rect r, const char *icon,
                  const char *label, bool selected);

/* A "chip" with a status dot, e.g. "Model ready". */
void  ui_status_chip(ui_context *u, osa_rect r, const char *label, osa_color dot);

/* Scrollable list helper: draws a rounded track and returns the thumb rect.
 * The caller owns the scroll offset. */
void  ui_scrollbar(ui_context *u, osa_rect track, int value, int total, int page);

/* Progress bar, 0..1. */
void  ui_progress(ui_context *u, osa_rect r, float frac, osa_color col);

/* Draws an audio waveform from peak data, filling `r`. */
void  ui_waveform(ui_context *u, osa_rect r, const float *peaks, int n,
                  double playhead, bool playing);

/* Centred modal dialog frame; returns the content rect. */
osa_rect ui_dialog(ui_context *u, float w, float h, const char *title);

/* Simple wrapping text with a bullet list, used by the help screen. */
void  ui_bullet_list(ui_context *u, osa_rect r, const char *const *items, int n,
                     float size, osa_color col);

/* Hit-test helper: true when the pointer is inside `r`. */
bool  ui_hit(ui_context *u, osa_rect r);

/* Row picker used by lists: draws hover/selection and returns true on click. */
bool  ui_row(ui_context *u, int id, osa_rect r, bool selected);

/* Segmented control over N labels separated by '|' ("فارسی|English"). */
bool  ui_segmented(ui_context *u, int id, osa_rect r, const char *labels, int *value, int n);

/* Numeric stepper with -/+ buttons. Returns true when the value changed. */
bool  ui_stepper(ui_context *u, int id, osa_rect r, int *value, int vmin, int vmax);

/* Horizontal slider. Returns true when the value changed. */
bool  ui_slider(ui_context *u, int id, osa_rect r, float *value, float vmin, float vmax);

/* Switch with a visible label on the leading side. */
bool  ui_toggle_switch(ui_context *u, int id, osa_rect r, bool *value);

/* Single-line text field / multi-line area. Return true when the text changed.
 * Both need a focused id so the platform can route key events. */
bool  ui_text_field(ui_context *u, int id, osa_rect r, char *buf, size_t cap, int max_chars);
bool  ui_text_area(ui_context *u, int id, osa_rect r, char *buf, size_t cap, int max_chars);
bool  ui_has_focus(ui_context *u);
int   ui_focus_id(ui_context *u);

/* Right-to-left aware horizontal split: returns the leading (sidebar) rect. */
osa_rect ui_split_leading(osa_rect *r, float w, bool rtl);

/* Formats a duration in ms as "1:23" / "1:02:03". */
void  ui_fmt_duration(double seconds, char *out, size_t cap);
/* Formats a byte count as "1.4 MB". */
void  ui_fmt_bytes(double bytes, char *out, size_t cap);

#endif /* OSAI_UI_H */
