/* =====================================================================
 *  OpenSpeech AI — UI kit
 *
 *  A small immediate-mode widget layer over the canvas/text primitives.
 *  The design goal is a desktop shell that looks deliberate on both a 1080p
 *  monitor and a 1366x768 laptop, and that lays out correctly when the
 *  interface language is Persian (right-to-left) or English.
 *
 *  Everything is measured from `osa_ui.layout`, so the RTL switch flips the
 *  whole shell without per-widget special cases. Sizes are written as logical
 *  pixels and multiplied by `L.scale` by the widgets themselves, so the same
 *  layout code works at 96 dpi and in a scaled window.
 * ===================================================================== */
#ifndef OSAI_UI_H
#define OSAI_UI_H

#include <stdbool.h>
#include "canvas.h"
#include "text.h"
#include "../core/osai_common.h"

/* ---- design tokens ------------------------------------------------ */
typedef struct ui_theme {
    osa_color bg;             /* window background                     */
    osa_color bg_alt;         /* recessed wells inside the background  */
    osa_color panel;          /* cards                                 */
    osa_color panel_alt;      /* controls inside cards                 */
    osa_color panel_hover;
    osa_color sidebar;        /* navigation rail                       */
    osa_color sidebar_active;
    osa_color sidebar_hover;
    osa_color accent_ring;    /* focus / selection ring                  */
    osa_color accent2_ring;
    osa_color text_mute;      /* between text and text_faint             */
    osa_color border;
    osa_color border_soft;
    osa_color border_strong;
    osa_color text;
    osa_color text_dim;
    osa_color text_faint;
    osa_color accent;         /* primary action / brand                */
    osa_color accent_hover;
    osa_color accent_soft;    /* tinted background of the accent       */
    osa_color on_accent;      /* text on top of accent                 */
    osa_color accent2;        /* secondary hue: translation surfaces   */
    osa_color accent2_soft;
    osa_color danger;
    osa_color danger_soft;
    osa_color success;
    osa_color success_soft;
    osa_color warning;
    osa_color warning_soft;
    osa_color selection;
    osa_color wave;
    osa_color wave_bg;
    osa_color shadow;
    osa_color overlay;
} ui_theme;

/* ---- layout ------------------------------------------------------- */
typedef struct ui_layout {
    bool   rtl;             /* Persian interface                    */
    float  scale;           /* 1.0 at 96 dpi, 1.5 at 144 dpi        */
    float  pad;             /* base spacing unit                    */
    float  radius;          /* card corner radius                   */
    float  radius_sm;
    int    win_w, win_h;
} ui_layout;

/* Clipboard shortcuts. The platform layer decodes the window's key events (only
 * it can read the system clipboard) and raises these as private-use scalar
 * values in ui_input.chars; the focused text widget consumes them. Choosing
 * private-use code points keeps the key path identical to typed characters, so
 * no widget needs to know about key codes or modifiers. */
#define UI_KEY_PASTE      0xE000u
#define UI_KEY_COPY       0xE001u
#define UI_KEY_CUT        0xE002u
#define UI_KEY_SELECT_ALL 0xE003u

/* ---- interaction state ------------------------------------------- */
typedef struct ui_input {
    float  mx, my;          /* current pointer                      */
    bool   down;            /* left button held                     */
    bool   clicked;         /* pressed this frame                   */
    bool   released;        /* released this frame                  */
    float  wheel;
    int    key;             /* last key code, 0 when none           */
    bool   shift, ctrl;
    /* Text typed this frame, as Unicode scalar values. The platform layer
     * decodes WM_CHAR / XLookupString into this buffer; widgets consume it.
     * Backspace arrives as 8, Enter as 13, Tab as 9. */
    uint32_t chars[64];
    int      n_chars;
} ui_input;

/* A drop-down / modal the shell owns. The widget only reports the request. */
typedef enum ui_popup_kind {
    UI_POPUP_NONE = 0,
    UI_POPUP_LIST,          /* generic single-choice list           */
    UI_POPUP_MODAL          /* message + buttons                    */
} ui_popup_kind;

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
    /* keyboard focus lives in the shell: a text field only draws a caret when
     * the shell says it has focus, otherwise clicking a button would leave the
     * caret blinking in an unfocused box. */
    int          focus_id;
    /* Set by a focused text widget when the user pressed Ctrl+C (1) or
     * Ctrl+X (2). The screen that owns the field reads it, puts the field's text
     * on the clipboard and clears it: the widget does not know what "the text of
     * this row" means for the screen's layout. */
    int          copy_request;
    char         tooltip[256];
    osa_rect     tooltip_anchor;
    /* open drop-down, owned by the shell */
    int          popup_id;
    osa_rect     popup_rect;
    int          popup_sel;
    int          popup_n;       /* total choices; items holds the visible page */
    int          popup_scroll;  /* first global index in the visible page */
    float        popup_x, popup_y;
    char         popup_items[12][64];
    /* animation clock, seconds since start (0 for stills) */
    float        time;
    /* scroll bookkeeping for the current frame's hovered region */
    float        scroll_delta;
    /* Transient "this control changed during the frame" flag. Set by the
     * stepper/slider/segmented family and cleared by ui_frame_begin, so a screen
     * can persist preferences once per frame instead of once per widget. */
    bool         value_changed;
} ui_context;

/* ---- lifecycle ---------------------------------------------------- */
bool ui_init(ui_context *u, osa_canvas *c);
void ui_shutdown(ui_context *u);
void ui_set_theme(ui_context *u, bool dark);
void ui_set_rtl(ui_context *u, bool rtl);
void ui_set_scale(ui_context *u, float scale);
/* Drives hover transitions and the live capture animation. Pass 0 for stills. */
void ui_set_time(ui_context *u, float seconds);
/* Starts a frame: clears per-frame interaction bookkeeping. */
void ui_frame_begin(ui_context *u, osa_canvas *c, const ui_input *in);

/* ---- typography --------------------------------------------------- */
/* Type ramp, in logical pixels. Call ui_* with these or with a custom size. */
#define UI_FS_DISPLAY  26.0f
#define UI_FS_H1       19.0f
#define UI_FS_H2       15.0f
#define UI_FS_BODY     13.0f
#define UI_FS_SMALL    11.5f
#define UI_FS_TINY     10.0f
#define UI_FS_MONO     11.0f

/* ---- primitives --------------------------------------------------- */
void  ui_panel(ui_context *u, osa_rect r);
void  ui_card(ui_context *u, osa_rect r);
void  ui_card_soft(ui_context *u, osa_rect r);       /* no border, faint fill */
void  ui_card_accent(ui_context *u, osa_rect r, osa_color tint);
void  ui_section(ui_context *u, osa_rect r, osa_color tint);
void  ui_separator(ui_context *u, osa_rect r);
void  ui_label(ui_context *u, osa_rect r, const char *text, float size, osa_color col, int align);
void  ui_label_wrap(ui_context *u, osa_rect r, const char *text, float size, osa_color col);
/* Small uppercase-ish caption above a group of controls. */
void  ui_caption(ui_context *u, osa_rect r, const char *text, int align);
/* Label that is right-aligned when Persian, left when English. */
void  ui_label_lead(ui_context *u, osa_rect r, const char *text, float size, osa_color col);
float ui_text_h(ui_context *u, float size);
/* Bullet / dot list of strings, returns the height used. */
float ui_bullet_list_h(ui_context *u, float size);
void  ui_bullet_list(ui_context *u, osa_rect r, const char *const *items, int n,
                     float size, osa_color col);

/* ---- buttons ------------------------------------------------------ */
bool  ui_button(ui_context *u, int id, osa_rect r, const char *label, bool primary);
bool  ui_button_ghost(ui_context *u, int id, osa_rect r, const char *label);
bool  ui_button_danger(ui_context *u, int id, osa_rect r, const char *label);
bool  ui_button_icon(ui_context *u, int id, osa_rect r, const char *glyph,
                     const char *label, bool active);
/* A button that shows a spinner and refuses clicks while `busy`. */
bool  ui_button_busy(ui_context *u, int id, osa_rect r, const char *label,
                     bool primary, bool busy);
bool  ui_toggle(ui_context *u, int id, osa_rect r, bool *value, const char *label);
bool  ui_tab(ui_context *u, int id, osa_rect r, const char *label, bool selected);

/* Vertical nav item used by the sidebar. */
bool  ui_nav_item(ui_context *u, int id, osa_rect r, const char *icon,
                  const char *label, bool selected);

/* A "chip" with a status dot, e.g. "Model ready". */
void  ui_status_chip(ui_context *u, osa_rect r, const char *label, osa_color dot);
/* Neutral pill with no dot (counters, tags). */
void  ui_pill(ui_context *u, osa_rect r, const char *label, osa_color fg, osa_color bg);

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

/* Hit-test helper: true when the pointer is inside `r`. */
bool  ui_hit(ui_context *u, osa_rect r);

/* Row picker used by lists: draws hover/selection and returns true on click. */
bool  ui_row(ui_context *u, int id, osa_rect r, bool selected);

/* Segmented control over N labels separated by '|' ("فارسی|English"). */
bool  ui_segmented(ui_context *u, int id, osa_rect r, const char *labels, int *value, int n);
/* Same, but the segment at `disabled_slot` (0-based, -1 for none) is drawn
 * inactive and cannot be selected: for a choice this build cannot honour. */
bool  ui_segmented_disabled(ui_context *u, int id, osa_rect r, const char *labels,
                            int *value, int n, int disabled_slot);

/* Numeric stepper with -/+ buttons. Returns true when the value changed. */
bool  ui_stepper(ui_context *u, int id, osa_rect r, int *value, int vmin, int vmax);

/* Horizontal slider. Returns true when the value changed. */
bool  ui_slider(ui_context *u, int id, osa_rect r, float *value, float vmin, float vmax);

/* Switch with a visible label on the leading side. */
bool  ui_toggle_switch(ui_context *u, int id, osa_rect r, bool *value);

/* Single-line text field / multi-line area. Return true when the text changed.
 * `id` also acts as the keyboard focus id (see ui_context.focus_id). */
/* Sets the faint hint drawn inside an empty field or area. The string is not
 * copied, so pass a literal or a buffer that outlives the frame. */
void  ui_field_hint(const char *text);
bool  ui_text_field(ui_context *u, int id, osa_rect r, char *buf, size_t cap, int max_chars);
bool  ui_text_area(ui_context *u, int id, osa_rect r, char *buf, size_t cap, int max_chars);
bool  ui_has_focus(ui_context *u);
int   ui_focus_id(ui_context *u);

/* ---- popups ------------------------------------------------------- */
/* Registers a drop-down above the widget `id` which will be drawn/consumed by
 * ui_popup_draw() after the page. `items` is '|' separated. Returns true when
 * the caller should open it (a click on the trigger). */
bool  ui_dropdown(ui_context *u, int id, osa_rect r, const char *text, bool open);
void  ui_dropdown_arrow(ui_context *u, osa_rect r);
/* Draws the open list (the shell calls this last) and returns the newly
 * selected index, or -1. */
int   ui_popup_draw(ui_context *u);
bool  ui_popup_open(ui_context *u);

/* Right-to-left aware horizontal split: returns the leading (sidebar) rect. */
osa_rect ui_split_leading(osa_rect *r, float w, bool rtl);

/* Formats a duration in ms as "1:23" / "1:02:03". */
void  ui_fmt_duration(double seconds, char *out, size_t cap);
/* Formats a byte count as "1.4 MB". */
void  ui_fmt_bytes(double bytes, char *out, size_t cap);

#endif /* OSAI_UI_H */
