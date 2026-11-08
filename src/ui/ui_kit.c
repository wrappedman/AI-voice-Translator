/* =====================================================================
 *  OpenSpeech AI — UI kit implementation
 *
 *  Widgets draw in a fixed order inside a card: fill, border, content. Any
 *  widget that can be clicked reports the click and marks it consumed so that
 *  exactly one widget reacts per press — the previous version let a click fall
 *  through to the card underneath, which is how pressing a button also
 *  triggered the action of the panel behind it.
 * ===================================================================== */
#include "ui_kit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef MAXF
#define MAXF(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MINF
#define MINF(a,b) ((a) < (b) ? (a) : (b))
#endif

/* Placeholder text for the field drawn this frame (see ui_field_hint). */
const char *g_field_hint;

/* ---- fonts -------------------------------------------------------- */
static const char *pick_font(bool bold) {
    /* prefer the bundled Vazirmatn; it covers Latin and Persian in one file so
     * mixed-script lines never fall back to a second font */
    static const char *regular[] = {
        "assets/fonts/Vazirmatn-Regular.ttf",
        "assets/Vazirmatn-Regular.ttf",
        "src/ui/assets/Vazirmatn-Regular.ttf",
        "assets/fonts/Vazirmatn-Medium.ttf",
        NULL
    };
    static const char *heavy[] = {
        "assets/fonts/Vazirmatn-Bold.ttf",
        "assets/Vazirmatn-Bold.ttf",
        "src/ui/assets/Vazirmatn-Bold.ttf",
        "assets/fonts/Vazirmatn-Medium.ttf",
        "src/ui/assets/Vazirmatn-Medium.ttf",
        "assets/fonts/Vazirmatn-Regular.ttf",
        "src/ui/assets/Vazirmatn-Regular.ttf",
        NULL
    };
    const char **list = bold ? heavy : regular;
    for (int i = 0; list[i]; i++)
        if (osa_file_exists(list[i])) return list[i];
    return NULL;
}

/* Two palettes, both built from one hue ramp so the accent is the only strong
 * colour in the interface. Dark is the default. */
void ui_set_theme(ui_context *u, bool dark) {
    ui_theme *t = &u->t;
    if (dark) {
        /* Three elevation steps are enough: the window, the cards on it, and
         * the controls inside a card. Each step is a flat fill with a hairline,
         * which reads as depth without pretending to be a photograph. */
        t->bg           = hexc(0x0E1116);
        t->bg_alt       = hexc(0x0A0D12);
        t->panel        = hexc(0x161B23);
        t->panel_alt    = hexc(0x1E2530);
        t->panel_hover  = hexc(0x283140);
        t->sidebar      = hexc(0x11151B);
        t->sidebar_active = hexc(0x1D2634);
        t->sidebar_hover  = hexc(0x1A212C);
        t->border       = hexc(0x2C3644);
        t->border_soft  = hexc(0x232C39);
        t->border_strong= hexc(0x3D4B60);
        t->text         = hexc(0xF2F5F9);
        t->text_dim     = hexc(0xAEBACB);
        t->text_mute    = hexc(0x8B98AA);
        t->text_faint   = hexc(0x6B7888);
        t->accent       = hexc(0x4C8DFF);
        t->accent_hover = hexc(0x6BA1FF);
        t->accent_soft  = hexc(0x1B2C46);
        t->accent_ring  = hexc(0x2A4570);
        t->on_accent    = hexc(0xFFFFFF);
        t->accent2      = hexc(0xA983FF);
        t->accent2_soft = hexc(0x2A2140);
        t->accent2_ring = hexc(0x453470);
        t->danger       = hexc(0xFF7A75);
        t->danger_soft  = hexc(0x3A1D20);
        t->success      = hexc(0x45D07E);
        t->success_soft = hexc(0x14301F);
        t->warning      = hexc(0xF0B54A);
        t->warning_soft = hexc(0x33280F);
        t->selection    = hexc(0x1F3452);
        t->wave         = hexc(0x5B9DFF);
        t->wave_bg      = hexc(0x121A26);
        t->shadow       = rgba(0, 0, 0, 130);
        t->overlay      = rgba(4, 7, 11, 200);
    } else {
        t->bg           = hexc(0xF6F8FB);
        t->bg_alt       = hexc(0xEEF2F7);
        t->panel        = hexc(0xFFFFFF);
        t->panel_alt    = hexc(0xF4F7FB);
        t->panel_hover  = hexc(0xE9EFF7);
        t->sidebar      = hexc(0xFBFCFE);
        t->sidebar_active = hexc(0xE7EFFD);
        t->sidebar_hover  = hexc(0xEFF3FA);
        t->border       = hexc(0xDCE3ED);
        t->border_soft  = hexc(0xE9EEF5);
        t->border_strong= hexc(0xBCC8D8);
        t->text         = hexc(0x10151C);
        t->text_dim     = hexc(0x4A5768);
        t->text_mute    = hexc(0x64718A);
        t->text_faint   = hexc(0x8492A6);
        t->accent       = hexc(0x2563EB);
        t->accent_hover = hexc(0x1D4ED8);
        t->accent_soft  = hexc(0xE4EDFE);
        t->accent_ring  = hexc(0xBFD3FA);
        t->on_accent    = hexc(0xFFFFFF);
        t->accent2      = hexc(0x7C3AED);
        t->accent2_soft = hexc(0xF1EAFE);
        t->accent2_ring = hexc(0xD9C8FB);
        t->danger       = hexc(0xD92D20);
        t->danger_soft  = hexc(0xFDECEA);
        t->success      = hexc(0x11874A);
        t->success_soft = hexc(0xE6F6ED);
        t->warning      = hexc(0xA16207);
        t->warning_soft = hexc(0xFDF4E3);
        t->selection    = hexc(0xDCE8FD);
        t->wave         = hexc(0x2563EB);
        t->wave_bg      = hexc(0xEDF2FA);
        t->shadow       = rgba(16, 30, 54, 40);
        t->overlay      = rgba(12, 20, 33, 130);
    }
}

void ui_set_rtl(ui_context *u, bool rtl) {
    u->L.rtl = rtl;
    if (u->c) u->c->rtl = rtl;
}

void ui_set_scale(ui_context *u, float s) {
    if (s < 0.75f) s = 0.75f;
    if (s > 3.0f) s = 3.0f;
    u->L.scale = s;
    u->L.pad = 8.0f * s;
    u->L.radius = 12.0f * s;
    u->L.radius_sm = 8.0f * s;
}

void ui_set_time(ui_context *u, float seconds) {
    u->time = seconds > 0 ? seconds : 0;
    if (u->c) u->c->frame_time = seconds > 0 ? seconds : 0;
}

bool ui_init(ui_context *u, osa_canvas *c) {
    if (!u || !c) return false;
    memset(u, 0, sizeof *u);
    u->c = c;
    if (!text_init()) return false;
    const char *fp = pick_font(false);
    if (fp) u->reg = text_font_load(fp, OSA_FW_REGULAR);
    const char *bp = pick_font(true);
    if (bp) u->bold = text_font_load(bp, OSA_FW_BOLD);
    if (!u->reg) u->reg = text_font_default(OSA_FW_REGULAR);
    if (!u->bold) u->bold = u->reg;
    if (!u->reg) return false;
    ui_set_scale(u, 1.0f);
    ui_set_theme(u, true);
    u->L.rtl = false;
    u->L.win_w = c->w;
    u->L.win_h = c->h;
    u->next_id = 1;
    u->focus_id = 0;
    u->popup_id = 0;
    return true;
}

void ui_shutdown(ui_context *u) {
    if (!u) return;
    if (u->reg) text_font_release(u->reg);
    if (u->bold && u->bold != u->reg) text_font_release(u->bold);
    u->reg = u->bold = NULL;
    text_shutdown();
}

void ui_frame_begin(ui_context *u, osa_canvas *c, const ui_input *in) {
    u->value_changed = false;
    g_field_hint = NULL;
    if (c) {
        u->c = c;
        u->L.win_w = c->w;
        u->L.win_h = c->h;
        c->rtl = u->L.rtl;
        canvas_clip_reset(c);
    }
    if (in) u->in = *in;
    else memset(&u->in, 0, sizeof u->in);
    u->next_id = 1;
    u->hot_id = 0;
    u->consumed_click = false;
    u->scroll_delta = 0;
    u->tooltip[0] = 0;
    /* An active drag (slider) survives the frame only while the button is held. */
    if (!u->in.down && u->active_id > 0) u->active_id = 0;
}

/* Placeholder of the text field drawn this frame. One field has focus at a
 * time and the hint is only read in the frame it was set, so a single slot is
 * enough and the public widget signature stays unchanged. */
const char *g_field_hint;
static const char *placeholder_hint(ui_context *u, int id) {
    (void)u; (void)id;
    return g_field_hint ? g_field_hint : "";
}

/* ---- helpers ------------------------------------------------------ */

osa_rect ui_split_leading(osa_rect *r, float w, bool rtl) {
    if (!rtl) return rect_split_left(r, w);
    return rect_split_right(r, w);
}

float ui_text_h(ui_context *u, float size) { return text_line_height(u->reg, size); }

void ui_panel(ui_context *u, osa_rect r) { c_fill(u->c, r, u->t.panel); }

/* A card is a flat plane with a hairline and a one-pixel highlight along the
 * top edge. That highlight is what makes the plane read as *above* the window
 * without casting a shadow, and it costs one fill. */
void ui_card(ui_context *u, osa_rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    c_fill_round(u->c, r, u->L.radius, u->t.panel);
    c_fill_round(u->c, rectf(r.x + u->L.radius, r.y, r.w - u->L.radius * 2, 1),
                 0.5f, col_alpha(u->t.text, 0.045f));
    c_stroke_round(u->c, r, u->L.radius, 1.0f, u->t.border);
}

void ui_card_soft(ui_context *u, osa_rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    c_fill_round(u->c, r, u->L.radius, u->t.panel);
    c_stroke_round(u->c, r, u->L.radius, 1.0f, u->t.border_soft);
}

void ui_card_accent(ui_context *u, osa_rect r, osa_color tint) {
    if (r.w <= 0 || r.h <= 0) return;
    c_fill_round(u->c, r, u->L.radius, u->t.panel);
    c_stroke_round(u->c, r, u->L.radius, 1.0f, u->t.border);
    /* a 3px tinted rule on the leading edge marks the card as active */
    float w = 3 * u->L.scale;
    osa_rect bar = rectf(u->L.rtl ? r.x + r.w - w : r.x, r.y + u->L.radius,
                         w, MAXF(0.0f, r.h - u->L.radius * 2));
    c_fill_round(u->c, bar, w * 0.5f, tint);
}

void ui_section(ui_context *u, osa_rect r, osa_color tint) {
    c_fill_round(u->c, r, u->L.radius_sm, tint);
}

void ui_separator(ui_context *u, osa_rect r) {
    c_fill(u->c, rectf(r.x, r.y + r.h * 0.5f, r.w, MAXF(1.0f, u->L.scale)), u->t.border_soft);
}

void ui_label(ui_context *u, osa_rect r, const char *text, float size, osa_color col, int align) {
    if (!text || !*text || r.w <= 1) return;
    text_opts o = text_default_opts(size);
    o.align = align;
    o.max_lines = 1;
    o.ellipsis = true;
    /* Direction follows the string's own script: a Latin model name inside the
     * Persian interface must not be reordered as if it were Persian. */
    o.rtl = text_is_rtl(text);
    o.max_width = r.w;
    /* Centre the *ink* box (ascent + descent), not the line box, or every label
     * sits a few pixels low — visible immediately in a dense settings list. */
    float asc = text_ascent(u->reg, size);
    float desc = text_descent(u->reg, size);
    float ink = asc + desc;
    float top = r.y + (r.h - ink) * 0.5f;
    text_draw_box(u->c, u->reg, size, col, rectf(r.x, top, r.w, ink + size * 0.4f), text, o);
}

/* Leading-edge label: right aligned in Persian, left aligned in English. */
void ui_label_lead(ui_context *u, osa_rect r, const char *text, float size, osa_color col) {
    ui_label(u, r, text, size, col, u->L.rtl ? 2 : 0);
}

void ui_caption(ui_context *u, osa_rect r, const char *text, int align) {
    ui_label(u, r, text, UI_FS_TINY * u->L.scale, u->t.text_faint, align);
}

void ui_label_wrap(ui_context *u, osa_rect r, const char *text, float size, osa_color col) {
    if (!text || !*text) return;
    text_opts o = text_default_opts(size);
    o.max_width = r.w;
    o.rtl = text_is_rtl(text);
    o.line_height = size * 1.75f;
    text_draw_box(u->c, u->reg, size, col, r, text, o);
}

/* ---- interaction core --------------------------------------------- */

static bool hit(ui_context *u, int id, osa_rect r) {
    bool inside = rect_has(r, u->in.mx, u->in.my);
    if (inside) u->hot_id = id;
    bool clicked = inside && u->in.clicked && !u->consumed_click;
    if (clicked) u->consumed_click = true;
    return clicked;
}

static bool hover(ui_context *u, osa_rect r) {
    return rect_has(r, u->in.mx, u->in.my);
}

bool ui_hit(ui_context *u, osa_rect r) {
    return rect_has(r, u->in.mx, u->in.my);
}

/* ---- buttons ------------------------------------------------------ */

static void button_text(ui_context *u, osa_rect r, const char *label, osa_color fg,
                        float size) {
    text_opts o = text_default_opts(size);
    o.align = 1;
    o.max_width = r.w - 12 * u->L.scale;
    o.max_lines = 1;
    o.ellipsis = true;
    o.rtl = u->L.rtl;
    text_draw_box(u->c, u->bold, size, fg,
                  rectf(r.x, r.y + (r.h - text_line_height(u->bold, size)) * 0.5f, r.w,
                        text_line_height(u->bold, size)), label, o);
}

bool ui_button(ui_context *u, int id, osa_rect r, const char *label, bool primary) {
    bool hov = hover(u, r);
    bool pressed = hov && u->in.down;
    float rad = u->L.radius_sm;
    if (primary) {
        osa_color bg = pressed ? col_darken(u->t.accent_hover, 0.10f)
                     : hov ? u->t.accent_hover : u->t.accent;
        c_fill_round(u->c, r, rad, bg);
        /* a soft inner highlight on the upper half reads as a bevel */
        c_fill_round(u->c, rectf(r.x + 1, r.y + 1, r.w - 2, r.h * 0.46f), rad,
                     col_alpha(rgba(255, 255, 255, 255), pressed ? 0.04f : 0.10f));
        button_text(u, r, label, u->t.on_accent, UI_FS_BODY * u->L.scale);
    } else {
        c_fill_round(u->c, r, rad, pressed ? u->t.panel_alt
                                         : hov ? u->t.panel_hover : u->t.panel_alt);
        c_stroke_round(u->c, r, rad, 1.0f,
                       hov ? u->t.border_strong : u->t.border);
        button_text(u, r, label, hov ? u->t.text : col_mix(u->t.text, u->t.text_dim, 0.35f),
                    UI_FS_BODY * u->L.scale);
    }
    bool clicked = hit(u, id, r);
    if (clicked) u->active_id = id;
    return clicked;
}

bool ui_button_ghost(ui_context *u, int id, osa_rect r, const char *label) {
    bool hov = hover(u, r);
    float rad = r.h * 0.5f;
    if (hov) {
        c_fill_round(u->c, r, rad, col_alpha(u->t.accent, 0.12f));
        c_stroke_round(u->c, r, rad, 1.0f, col_alpha(u->t.accent, 0.45f));
    } else {
        c_stroke_round(u->c, r, rad, 1.0f, u->t.border);
    }
    button_text(u, r, label, hov ? u->t.accent : u->t.text_dim, UI_FS_SMALL * u->L.scale);
    return hit(u, id, r);
}

bool ui_button_danger(ui_context *u, int id, osa_rect r, const char *label) {
    bool hov = hover(u, r);
    float rad = u->L.radius_sm;
    c_fill_round(u->c, r, rad, hov ? col_alpha(u->t.danger, 0.22f) : u->t.danger_soft);
    c_stroke_round(u->c, r, rad, 1.0f, col_alpha(u->t.danger, hov ? 0.7f : 0.4f));
    button_text(u, r, label, u->t.danger, UI_FS_BODY * u->L.scale);
    return hit(u, id, r);
}

bool ui_button_busy(ui_context *u, int id, osa_rect r, const char *label,
                    bool primary, bool busy) {
    if (!busy) return ui_button(u, id, r, label, primary);
    /* Busy: draw the same pill with a spinner and swallow the click, so a
     * long operation cannot be queued twice by an impatient second press. */
    c_fill_round(u->c, r, u->L.radius_sm, primary ? col_alpha(u->t.accent, 0.65f)
                                                  : u->t.panel_alt);
    float cy = r.y + r.h * 0.5f;
    float cx = u->L.rtl ? r.x + r.w - 20 * u->L.scale : r.x + 20 * u->L.scale;
    float t = u->time * 6.0f;
    for (int i = 0; i < 8; i++) {
        float a = t + (float)i * 0.7853982f;
        float al = 0.25f + 0.75f * ((float)i / 8.0f);
        c_circle(u->c, cx + cosf(a) * 7 * u->L.scale, cy + sinf(a) * 7 * u->L.scale,
                 1.6f * u->L.scale, col_alpha(u->t.on_accent, al));
    }
    osa_rect tr = rectf(u->L.rtl ? r.x : r.x + 34 * u->L.scale, r.y,
                        r.w - 44 * u->L.scale, r.h);
    button_text(u, tr, label, col_alpha(primary ? u->t.on_accent : u->t.text, 0.75f),
                UI_FS_BODY * u->L.scale);
    hit(u, id, r);          /* consume nothing, but keep the pointer honest */
    u->consumed_click = u->consumed_click;
    return false;
}

bool ui_button_icon(ui_context *u, int id, osa_rect r, const char *glyph,
                    const char *label, bool active) {
    bool hov = hover(u, r);
    float rad = u->L.radius_sm;
    if (active) {
        c_fill_round(u->c, r, rad, u->t.accent_soft);
        c_stroke_round(u->c, r, rad, 1.0f, col_alpha(u->t.accent, 0.55f));
    } else if (hov) {
        c_fill_round(u->c, r, rad, u->t.panel_alt);
    }
    float size = UI_FS_BODY * u->L.scale;
    float glyph_w = glyph && *glyph ? text_measure(u->bold, size, glyph) + 8 * u->L.scale : 0;
    float asc = text_ascent(u->bold, size);
    float base = r.y + (r.h + asc - text_descent(u->bold, size)) * 0.5f;
    osa_color fg = active ? u->t.accent : u->t.text;
    if (glyph && *glyph) {
        if (!u->L.rtl) text_draw(u->c, u->bold, size, fg, r.x + 10 * u->L.scale, base, glyph);
        else text_draw(u->c, u->bold, size, fg,
                       r.x + r.w - 10 * u->L.scale - text_measure(u->bold, size, glyph), base, glyph);
    }
    if (label && *label) {
        float lw = r.w - glyph_w - 20 * u->L.scale;
        float lx = u->L.rtl ? r.x + 10 * u->L.scale : r.x + 10 * u->L.scale + glyph_w;
        text_opts o = text_default_opts(size);
        o.max_width = lw;
        o.max_lines = 1;
        o.ellipsis = true;
        o.align = u->L.rtl ? 2 : 0;
        o.rtl = u->L.rtl;
        text_draw_box(u->c, u->bold, size, fg, rectf(lx, r.y, lw, r.h), label, o);
    }
    return hit(u, id, r);
}

bool ui_toggle(ui_context *u, int id, osa_rect r, bool *value, const char *label) {
    float sw = 38.0f * u->L.scale, sh = 21.0f * u->L.scale;
    osa_rect track = rectf(u->L.rtl ? r.x : r.x + r.w - sw, r.y + (r.h - sh) * 0.5f, sw, sh);
    c_fill_round(u->c, track, sh * 0.5f, *value ? u->t.accent : u->t.border_strong);
    float kr = sh * 0.5f - 2.5f * u->L.scale;
    float kx = *value ? track.x + track.w - sh * 0.5f : track.x + sh * 0.5f;
    c_circle(u->c, kx, track.y + sh * 0.5f, kr, rgba(255, 255, 255, 255));
    if (label) {
        osa_rect lr = rectf(r.x, r.y, r.w - sw - 10 * u->L.scale, r.h);
        ui_label(u, lr, label, UI_FS_BODY * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    }
    if (hit(u, id, r)) { *value = !*value; return true; }
    return false;
}

bool ui_tab(ui_context *u, int id, osa_rect r, const char *label, bool selected) {
    bool hov = hover(u, r);
    if (selected) {
        c_fill(u->c, rectf(r.x, r.y + r.h - 2 * u->L.scale, r.w, 2 * u->L.scale), u->t.accent);
    } else if (hov) {
        c_fill(u->c, rectf(r.x, r.y + r.h - 2 * u->L.scale, r.w, 2 * u->L.scale), u->t.border);
    }
    ui_label(u, rectf(r.x, r.y, r.w, r.h - 4 * u->L.scale), label,
             UI_FS_BODY * u->L.scale, selected ? u->t.text : u->t.text_dim, 1);
    return hit(u, id, r);
}

/* ---- sidebar ------------------------------------------------------ */

bool ui_nav_item(ui_context *u, int id, osa_rect r, const char *icon,
                 const char *label, bool selected) {
    bool hov = hover(u, r);
    float rad = u->L.radius_sm;
    if (selected) {
        c_fill_round(u->c, r, rad, u->t.sidebar_active);
        /* 3px accent bar on the reading-start edge */
        float bw = 3 * u->L.scale;
        osa_rect bar = rectf(u->L.rtl ? r.x + r.w - bw : r.x, r.y + 9 * u->L.scale, bw,
                             r.h - 18 * u->L.scale);
        c_fill_round(u->c, bar, bw * 0.5f, u->t.accent);
    } else if (hov) {
        c_fill_round(u->c, r, rad, u->t.panel_alt);
    }
    osa_color fg = selected ? u->t.accent : u->t.text_dim;
    float size = UI_FS_BODY * u->L.scale;
    float asc = text_ascent(u->bold, size);
    float base = r.y + (r.h + asc - text_descent(u->bold, size)) * 0.5f;
    float ix = u->L.rtl ? r.x + r.w - 18 * u->L.scale : r.x + 16 * u->L.scale;
    /* square icon plate, so icons of different ink share one optical size */
    float plate = 22 * u->L.scale;
    float px = u->L.rtl ? ix - plate * 0.75f : ix - plate * 0.25f;
    osa_rect pr = rectf(px, r.y + (r.h - plate) * 0.5f, plate, plate);
    if (selected) c_fill_round(u->c, pr, plate * 0.34f, col_alpha(u->t.accent, 0.14f));
    if (icon && *icon) {
        float iw = text_measure(u->bold, size, icon);
        float ix2 = u->L.rtl ? pr.x + (plate - iw) * 0.5f : pr.x + (plate - iw) * 0.5f;
        text_draw(u->c, u->bold, size, fg, ix2, base, icon);
    }
    osa_rect lr;
    if (u->L.rtl) lr = rectf(r.x + 12 * u->L.scale, r.y, r.w - 12 * u->L.scale - plate - 22 * u->L.scale, r.h);
    else lr = rectf(r.x + plate + 22 * u->L.scale, r.y,
                    r.w - plate - 34 * u->L.scale, r.h);
    ui_label(u, lr, label, size, selected ? u->t.text : u->t.text_dim, u->L.rtl ? 2 : 0);
    return hit(u, id, r);
}

/* ---- chips and pills ---------------------------------------------- */

void ui_status_chip(ui_context *u, osa_rect r, const char *label, osa_color dot) {
    /* A chip hugs its label: callers pass the maximum width they can spare,
     * not the width the pill should pretend to have. */
    float fs = UI_FS_SMALL * u->L.scale;
    float pad = 11 * u->L.scale;
    float inset = pad + 9 * u->L.scale;
    float need = text_measure(u->reg, fs, label) + inset + 10 * u->L.scale;
    if (need < r.h + 8 * u->L.scale) need = r.h + 8 * u->L.scale;
    if (need < r.w) {                       /* shrink from the trailing edge */
        if (u->L.rtl) r.x += r.w - need;
        r.w = need;
    }
    c_fill_round(u->c, r, r.h * 0.5f, u->t.panel_alt);
    c_stroke_round(u->c, r, r.h * 0.5f, 1.0f, u->t.border_soft);
    float cy = r.y + r.h * 0.5f;
    float dx = u->L.rtl ? r.x + r.w - pad : r.x + pad;
    /* soft glow behind the dot reads as "live" without animating */
    c_circle(u->c, dx, cy, 6.0f * u->L.scale, col_alpha(dot, 0.20f));
    c_circle(u->c, dx, cy, 3.5f * u->L.scale, dot);
    osa_rect lr = rectf(u->L.rtl ? r.x + 6 * u->L.scale : r.x + inset, r.y,
                        r.w - inset - 6 * u->L.scale, r.h);
    ui_label(u, lr, label, fs, u->t.text_dim, u->L.rtl ? 2 : 0);
}

void ui_pill(ui_context *u, osa_rect r, const char *label, osa_color fg, osa_color bg) {
    c_fill_round(u->c, r, r.h * 0.5f, bg);
    ui_label(u, r, label, UI_FS_TINY * u->L.scale, fg, 1);
}

/* ---- scroll, progress, waveform ----------------------------------- */

void ui_scrollbar(ui_context *u, osa_rect track, int value, int total, int page) {
    if (total <= page || track.h <= 2) return;
    c_fill_round(u->c, track, track.w * 0.5f, u->t.panel_alt);
    float frac = (float)page / (float)total;
    float th = MAXF(track.h * frac, 24.0f * u->L.scale);
    int maxv = total - page;
    float t = maxv > 0 ? (float)value / (float)maxv : 0.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    osa_rect thumb = rectf(track.x, track.y + (track.h - th) * t, track.w, th);
    c_fill_round(u->c, thumb, track.w * 0.5f, u->t.text_faint);
}

void ui_progress(ui_context *u, osa_rect r, float frac, osa_color col) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    c_fill_round(u->c, r, r.h * 0.5f, u->t.panel_alt);
    if (frac > 0.001f) {
        float w = MAXF(r.w * frac, r.h);
        osa_rect fill = rectf(u->L.rtl ? r.x + r.w - w : r.x, r.y, w, r.h);
        c_fill_round(u->c, fill, r.h * 0.5f, col);
    }
}

void ui_waveform(ui_context *u, osa_rect r, const float *peaks, int n,
                 double playhead, bool playing) {
    c_fill_round(u->c, r, u->L.radius_sm, u->t.wave_bg);
    if (!peaks || n <= 0) return;
    float mid = r.y + r.h * 0.5f;
    int bars = (int)(r.w / (3.0f * u->L.scale));
    if (bars < 1) return;
    if (bars > n) bars = n;
    float bw = r.w / (float)bars;
    for (int i = 0; i < bars; i++) {
        int si = (int)((long)i * n / bars);
        float p = peaks[si];
        if (p < 0.02f) p = 0.02f;
        if (p > 1.0f) p = 1.0f;
        float h = p * (r.h - 10 * u->L.scale);
        float x = r.x + (float)i * bw;
        double pos = (double)i / (double)bars;
        bool passed = playing && pos <= playhead;
        osa_color col = passed ? u->t.accent : u->t.wave;
        c_fill_round(u->c, rectf(x, mid - h * 0.5f, MAXF(1.5f, bw - 1.2f * u->L.scale), h),
                     bw * 0.45f, col_alpha(col, playing ? (passed ? 1.0f : 0.4f) : 0.75f));
    }
    if (playing) {
        float px = r.x + r.w * (float)playhead;
        c_vline(u->c, px, r.y + 3, r.y + r.h - 3, 2.0f * u->L.scale, u->t.text);
    }
}

/* ---- dialog ------------------------------------------------------- */

osa_rect ui_dialog(ui_context *u, float w, float h, const char *title) {
    float sw = (float)u->c->w, sh = (float)u->c->h;
    c_fill(u->c, rectf(0, 0, sw, sh), u->t.overlay);
    osa_rect d = rectf((sw - w) * 0.5f, (sh - h) * 0.5f, w, h);
    c_soft_shadow(u->c, d, u->L.radius, 18 * u->L.scale, 8 * u->L.scale, u->t.shadow);
    c_fill_round(u->c, d, u->L.radius, u->t.panel);
    c_stroke_round(u->c, d, u->L.radius, 1.0f, u->t.border_strong);
    if (title) {
        ui_label(u, rectf(d.x + 20 * u->L.scale, d.y + 14 * u->L.scale,
                          d.w - 40 * u->L.scale, 26 * u->L.scale),
                 title, UI_FS_H2 * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
        c_hline(u->c, d.x + 20 * u->L.scale, d.x + d.w - 20 * u->L.scale,
                d.y + 48 * u->L.scale, 1, u->t.border_soft);
    }
    return d;
}

/* ---- lists -------------------------------------------------------- */

void ui_bullet_list(ui_context *u, osa_rect r, const char *const *items, int n,
                    float size, osa_color col) {
    float lh = size * 1.75f;
    float y = r.y;
    for (int i = 0; i < n; i++) {
        text_opts o = text_default_opts(size);
        o.max_width = r.w - 16 * u->L.scale;
        o.rtl = text_is_rtl(items[i]);
        o.line_height = lh;
        float bh = text_box_height(u->reg, size, rectf(r.x, y, r.w - 16 * u->L.scale, 400),
                                   items[i], o);
        if (bh < lh) bh = lh;
        if (y + bh > r.y + r.h) break;
        c_circle(u->c, u->L.rtl ? r.x + r.w - 5 * u->L.scale : r.x + 5 * u->L.scale,
                 y + lh * 0.42f, 2.5f * u->L.scale, u->t.accent);
        osa_rect ir = u->L.rtl ? rectf(r.x, y, r.w - 16 * u->L.scale, bh)
                               : rectf(r.x + 16 * u->L.scale, y, r.w - 16 * u->L.scale, bh);
        text_draw_box(u->c, u->reg, size, col, ir, items[i], o);
        y += bh + 4 * u->L.scale;
    }
}

float ui_bullet_list_h(ui_context *u, float size) { return size * 1.75f + 4 * u->L.scale; }

void ui_fmt_duration(double seconds, char *out, size_t cap) {
    if (seconds < 0) seconds = 0;
    int total = (int)(seconds + 0.5);
    int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    if (h > 0) snprintf(out, cap, "%d:%02d:%02d", h, m, s);
    else snprintf(out, cap, "%d:%02d", m, s);
}

void ui_fmt_bytes(double bytes, char *out, size_t cap) {
    const char *unit[] = { "B", "KB", "MB", "GB", "TB" };
    int u = 0;
    double v = bytes;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0) snprintf(out, cap, "%.0f %s", v, unit[u]);
    else snprintf(out, cap, "%.1f %s", v, unit[u]);
}

/* ---- rows --------------------------------------------------------- */

bool ui_row(ui_context *u, int id, osa_rect r, bool selected) {
    bool hov = hover(u, r);
    if (hov) u->hot_id = id;
    if (selected) c_fill_round(u->c, r, u->L.radius_sm, u->t.selection);
    else if (hov) c_fill_round(u->c, r, u->L.radius_sm, u->t.panel_alt);
    if (selected) {
        float bar_w = 3 * u->L.scale;
        osa_rect bar = rectf(u->L.rtl ? r.x + r.w - bar_w - 2 : r.x + 2,
                             r.y + 8 * u->L.scale, bar_w, r.h - 16 * u->L.scale);
        c_fill_round(u->c, bar, bar_w * 0.5f, u->t.accent);
    }
    return hit(u, id, r);
}

/* ---- controls ------------------------------------------------------ */

bool ui_segmented(ui_context *u, int id, osa_rect r, const char *labels, int *value, int n) {
    if (n <= 0) return false;
    c_fill_round(u->c, r, r.h * 0.5f, u->t.panel_alt);
    c_stroke_round(u->c, r, r.h * 0.5f, 1.0f, u->t.border_soft);
    float seg_w = r.w / n;
    char part[128];
    const char *p = labels;
    bool changed = false;
    int shown = 0;
    for (int i = 0; i < n; i++) {
        size_t k = 0;
        while (*p && *p != '|' && k + 1 < sizeof part) part[k++] = *p++;
        part[k] = 0;
        if (*p == '|') p++;
        /* RTL mirrors the segment order so "first = leading" holds in both
         * directions (Persian lists read right to left). */
        int slot = u->L.rtl ? (n - 1 - i) : i;
        osa_rect sr = rectf(r.x + slot * seg_w + 2, r.y + 2, seg_w - 4, r.h - 4);
        bool sel = *value == i;
        if (sel) {
            c_fill_round(u->c, sr, sr.h * 0.5f, u->t.accent);
        }
        ui_label(u, sr, part, UI_FS_SMALL * u->L.scale,
                 sel ? u->t.on_accent : u->t.text_dim, 1);
        if (hover(u, sr) && u->in.clicked && !u->consumed_click && !sel) {
            u->consumed_click = true;
            *value = i;
            changed = true;
        }
        (void)id;
        shown++;
    }
    (void)shown;
    return changed;
}

/* A stepper reads in the direction of the paragraph: in RTL the decrement is
 * the first button the eye meets (right) and the increment is on the left, so
 * "+" always moves *forward* along the reading direction. The digits in the
 * middle are localised by the caller through the ui_context's RTL flag, and the
 * middle column is sized from the text so a two- or three-digit value on a wide
 * window is not a postage stamp. */
bool ui_stepper(ui_context *u, int id, osa_rect r, int *value, int vmin, int vmax) {
    bool changed = false;
    float bw = r.h;
    char num[32], loctmp[64];
    snprintf(num, sizeof num, "%d", *value);
    /* Size the field for the digits that will actually be drawn: localised
     * Persian digits are wider than Latin ones, and a field measured with "6"
     * clipped "۴" to "f". */
    if (u->L.rtl) text_localize_digits(num, "fa", loctmp, sizeof loctmp);
    else osa_strlcpy(loctmp, num, sizeof loctmp);
    float num_w = text_measure(u->bold, UI_FS_BODY * u->L.scale, loctmp) + 30 * u->L.scale;
    if (num_w < r.w * 0.34f) num_w = r.w * 0.34f;
    if (num_w > r.w - bw * 2 - 4) num_w = r.w - bw * 2 - 4;

    osa_rect minus, plus, mid;
    if (u->L.rtl) {
        minus  = rectf(r.x + r.w - bw, r.y, bw, r.h);
        plus   = rectf(r.x, r.y, bw, r.h);
        mid    = rectf(r.x + bw + 2, r.y, num_w, r.h);
    } else {
        minus  = rectf(r.x, r.y, bw, r.h);
        plus   = rectf(r.x + r.w - bw, r.y, bw, r.h);
        mid    = rectf(r.x + r.w - bw - 2 - num_w, r.y, num_w, r.h);
    }
    c_fill_round(u->c, mid, u->L.radius_sm * 0.7f, u->t.panel_alt);
    char loc[64];
    if (u->L.rtl) text_localize_digits(num, "fa", loc, sizeof loc);
    else osa_strlcpy(loc, num, sizeof loc);
    ui_label(u, mid, loc, UI_FS_BODY * u->L.scale, u->t.text, 1);

    for (int k = 0; k < 2; k++) {
        osa_rect br = k == 0 ? minus : plus;
        bool hov = hover(u, br);
        c_fill_round(u->c, br, u->L.radius_sm * 0.7f,
                     hov ? u->t.panel_hover : u->t.panel_alt);
        c_stroke_round(u->c, br, u->L.radius_sm * 0.7f, 1, u->t.border);
        osa_color fg = hov ? u->t.text : u->t.text_dim;
        c_fill(u->c, rectf(br.x + br.w * 0.3f, br.y + br.h * 0.5f - 1 * u->L.scale,
                           br.w * 0.4f, 2 * u->L.scale), fg);
        if (k == 1)
            c_fill(u->c, rectf(br.x + br.w * 0.5f - 1 * u->L.scale, br.y + br.h * 0.3f,
                               2 * u->L.scale, br.h * 0.4f), fg);
        if (hov && u->in.clicked && !u->consumed_click) {
            u->consumed_click = true;
            int nv = *value + (k == 0 ? -1 : 1);
            if (nv < vmin) nv = vmin;
            if (nv > vmax) nv = vmax;
            if (nv != *value) { *value = nv; changed = true; u->value_changed = true; }
        }
    }
    (void)id;
    return changed;
}

bool ui_slider(ui_context *u, int id, osa_rect r, float *value, float vmin, float vmax) {
    bool changed = false;
    osa_rect tr = rectf(r.x, r.y + r.h * 0.5f - 3 * u->L.scale, r.w, 6 * u->L.scale);
    c_fill_round(u->c, tr, 3 * u->L.scale, u->t.panel_alt);
    float t = (*value - vmin) / (vmax - vmin);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float knob_t = u->L.rtl ? 1.0f - t : t;
    osa_rect fill = rectf(tr.x, tr.y, tr.w * knob_t, tr.h);
    c_fill_round(u->c, fill, 3 * u->L.scale, u->t.accent);
    float kx = truncf(tr.x + tr.w * knob_t);
    bool hov = hover(u, r);
    if (hov || u->active_id == id) {
        c_soft_shadow(u->c, rectf(kx - 8 * u->L.scale, tr.y - 5 * u->L.scale,
                                  16 * u->L.scale, 16 * u->L.scale),
                      8 * u->L.scale, 5 * u->L.scale, 1 * u->L.scale,
                      col_alpha(u->t.accent, 0.4f));
    }
    c_circle(u->c, kx, tr.y + tr.h * 0.5f, 8 * u->L.scale, u->t.panel);
    c_circle(u->c, kx, tr.y + tr.h * 0.5f, 6 * u->L.scale, u->t.accent);

    if (hover(u, r) && u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        u->active_id = id;
    }
    if (u->active_id == id) {
        if (u->in.down) {
            float nt = (u->in.mx - tr.x) / tr.w;
            if (u->L.rtl) nt = 1.0f - nt;
            if (nt < 0) nt = 0;
            if (nt > 1) nt = 1;
            float nv = vmin + nt * (vmax - vmin);
            if (nv != *value) { *value = nv; changed = true; u->value_changed = true; }
        }
    }
    return changed;
}

bool ui_toggle_switch(ui_context *u, int id, osa_rect r, bool *value) {
    bool hov = hover(u, r);
    if (hov) u->hot_id = id;
    bool on = *value;
    osa_color track = on ? u->t.accent : u->t.border_strong;
    if (hov) track = on ? u->t.accent_hover : col_lighten(u->t.border_strong, 0.08f);
    c_fill_round(u->c, r, r.h * 0.5f, track);
    float kr = r.h * 0.5f - 3 * u->L.scale;
    float kx = on ? (u->L.rtl ? r.x + kr + 3 * u->L.scale : r.x + r.w - kr - 3 * u->L.scale)
                  : (u->L.rtl ? r.x + r.w - kr - 3 * u->L.scale : r.x + kr + 3 * u->L.scale);
    c_circle(u->c, kx, r.y + r.h * 0.5f, kr, rgba(255, 255, 255, 255));
    bool clicked = hov && u->in.clicked && !u->consumed_click;
    if (clicked) {
        u->consumed_click = true;
        *value = !on;
    }
    return clicked;
}

/* ---- text input ---------------------------------------------------- */

void ui_field_hint(const char *text) { g_field_hint = text; }

/* Appends the typed scalars to `buf`. Returns true when the buffer changed. */
static bool text_input_apply(ui_context *u, char *buf, size_t cap, bool multiline) {
    size_t n = strlen(buf);
    bool changed = false;
    for (int i = 0; i < u->in.n_chars; i++) {
        unsigned cp = u->in.chars[i];
        if (cp == 8) {
            if (n > 0) {
                size_t k = n - 1;
                while (k > 0 && ((unsigned char)buf[k] & 0xC0) == 0x80) k--;
                buf[k] = 0;
                n = k;
                changed = true;
            }
        } else if (cp == 13) {
            if (multiline) {
                if (n + 2 < cap) { buf[n++] = '\n'; buf[n] = 0; changed = true; }
            } else {
                continue;
            }
        } else if (cp == 9 || cp == 27 || cp < 32) {
            continue;
        } else {
            char tmp[4];
            int k = 0;
            if (cp < 0x80) tmp[k++] = (char)cp;
            else if (cp < 0x800) { tmp[k++] = (char)(0xC0 | (cp >> 6)); tmp[k++] = (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) {
                tmp[k++] = (char)(0xE0 | (cp >> 12));
                tmp[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                tmp[k++] = (char)(0x80 | (cp & 0x3F));
            } else {
                tmp[k++] = (char)(0xF0 | (cp >> 18));
                tmp[k++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                tmp[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                tmp[k++] = (char)(0x80 | (cp & 0x3F));
            }
            if (n + (size_t)k + 1 >= cap) break;
            memcpy(buf + n, tmp, (size_t)k);
            n += (size_t)k;
            buf[n] = 0;
            changed = true;
        }
    }
    return changed;
}

static void draw_caret(ui_context *u, float x, float y0, float y1) {
    /* Only animate when a clock is running; stills get a solid caret so the
     * snapshots stay byte-identical. */
    float a = 1.0f;
    if (u->time > 0) a = (fmodf(u->time, 1.06f) < 0.53f) ? 1.0f : 0.15f;
    c_fill(u->c, rectf(x, y0, MAXF(1.5f, 1.5f * u->L.scale), y1 - y0),
           col_alpha(u->t.accent, a));
}

bool ui_text_field(ui_context *u, int id, osa_rect r, char *buf, size_t cap, int max_chars) {
    (void)max_chars;
    bool hov = hover(u, r);
    bool focused = (u->focus_id == id);
    if (u->active_id == id && !u->in.down) u->active_id = 0;
    c_fill_round(u->c, r, u->L.radius_sm, u->t.panel_alt);
    c_stroke_round(u->c, r, u->L.radius_sm, focused ? 1.6f : 1.0f,
                   focused ? u->t.accent : (hov ? u->t.border_strong : u->t.border));
    if (hov && u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        u->focus_id = id;
        focused = true;
    }
    bool changed = false;
    if (focused) changed = text_input_apply(u, buf, cap, false);
    osa_rect tr = rect_inset(r, 11 * u->L.scale);
    bool rtl = text_is_rtl(buf);
    float size = UI_FS_BODY * u->L.scale;
    float asc = text_ascent(u->reg, size);
    float base = tr.y + (tr.h + asc - text_descent(u->reg, size)) * 0.5f;
    float w = text_measure(u->reg, size, buf);
    float avail = tr.w;
    if (w > avail) w = avail;
    if (rtl) {
        float x = tr.x + MAXF(0.0f, tr.w - w);
        text_draw(u->c, u->reg, size, u->t.text, x, base, buf);
        if (focused) draw_caret(u, x - 2 * u->L.scale, tr.y + 3, tr.y + tr.h - 3);
    } else {
        text_draw(u->c, u->reg, size, u->t.text, tr.x, base, buf);
        if (focused) draw_caret(u, tr.x + w + 1, tr.y + 3, tr.y + tr.h - 3);
    }
    return changed;
}

/* Multi-line editor with wrapping and a caret on the last line. */
bool ui_text_area(ui_context *u, int id, osa_rect r, char *buf, size_t cap, int max_chars) {
    (void)max_chars;
    bool hov = hover(u, r);
    bool focused = (u->focus_id == id);
    c_fill_round(u->c, r, u->L.radius_sm, u->t.panel_alt);
    c_stroke_round(u->c, r, u->L.radius_sm, focused ? 1.6f : 1.0f,
                   focused ? u->t.accent : (hov ? u->t.border_strong : u->t.border));
    if (hov && u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        u->focus_id = id;
        focused = true;
    }
    bool changed = false;
    if (focused) changed = text_input_apply(u, buf, cap, true);
    osa_rect tr = rect_inset(r, 12 * u->L.scale);
    float size = UI_FS_H2 * u->L.scale;
    text_opts o = text_default_opts(size);
    o.max_width = tr.w;
    o.line_height = size * 1.85f;
    o.rtl = text_is_rtl(buf);
    o.align = o.rtl ? 2 : 0;
    if (buf[0]) text_draw_box(u->c, u->reg, size, u->t.text, tr, buf, o);
    else if (!focused)
        ui_label(u, rectf(tr.x, tr.y + 2 * u->L.scale, tr.w, o.line_height),
                 placeholder_hint(u, id), size, u->t.text_faint, o.rtl ? 2 : 0);
    if (focused) {
        /* caret after the last line: measure the trailing line to place it */
        const char *last = buf;
        for (const char *p = buf; *p; p++) if (*p == '\n') last = p + 1;
        float w = text_measure(u->reg, size, last);
        if (w > tr.w) w = tr.w;
        float lines = 0;
        float lh = size * 1.85f;
        /* count wrapped lines the same way text_draw_box does */
        char tmp[16384];
        int n = text_wrap_lines(u->reg, size, buf, tr.w, tmp, sizeof tmp, 0);
        lines = (float)(n > 0 ? n - 1 : 0);
        float y = tr.y + lines * lh;
        float x = o.rtl ? tr.x + tr.w - w : tr.x + w;
        if (y + lh < tr.y + tr.h + lh * 0.5f)
            draw_caret(u, x, y + 3 * u->L.scale, y + lh - 3 * u->L.scale);
    }
    return changed;
}

bool ui_has_focus(ui_context *u) { return u->focus_id != 0; }
int  ui_focus_id(ui_context *u) { return u->focus_id; }

/* ---- drop-down ----------------------------------------------------- */

void ui_dropdown_arrow(ui_context *u, osa_rect r) {
    /* a small chevron drawn with two lines, no glyph needed */
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    float w = 4.0f * u->L.scale, h = 2.2f * u->L.scale;
    c_line(u->c, cx - w, cy - h * 0.5f, cx, cy + h, 1.6f * u->L.scale, u->t.text_dim);
    c_line(u->c, cx, cy + h, cx + w, cy - h * 0.5f, 1.6f * u->L.scale, u->t.text_dim);
}

bool ui_dropdown(ui_context *u, int id, osa_rect r, const char *text, bool open) {
    bool hov = hover(u, r);
    c_fill_round(u->c, r, u->L.radius_sm, hov || open ? u->t.panel_hover : u->t.panel_alt);
    c_stroke_round(u->c, r, u->L.radius_sm, open ? 1.6f : 1.0f,
                   open ? u->t.accent : (hov ? u->t.border_strong : u->t.border));
    float aw = 22 * u->L.scale;
    osa_rect ar = rectf(u->L.rtl ? r.x + 4 * u->L.scale : r.x + r.w - aw - 4 * u->L.scale,
                        r.y, aw, r.h);
    osa_rect tr = rectf(u->L.rtl ? r.x + aw : r.x + 12 * u->L.scale, r.y,
                        r.w - aw - 12 * u->L.scale, r.h);
    ui_label(u, tr, text, UI_FS_BODY * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    ui_dropdown_arrow(u, ar);
    return hit(u, id, r);
}

bool ui_popup_open(ui_context *u) { return u->popup_id != 0; }

int ui_popup_draw(ui_context *u) {
    if (!u->popup_id) return -1;
    int n = u->popup_n;
    if (n <= 0) { u->popup_id = 0; return -1; }
    float row_h = 34 * u->L.scale;
    float pad = 6 * u->L.scale;
    float h = row_h * n + pad * 2;
    float w = MAXF(u->popup_rect.w, 160 * u->L.scale);
    float x = u->popup_rect.x;
    float y = u->popup_rect.y + u->popup_rect.h + 6 * u->L.scale;
    if (y + h > u->c->h - 8) y = MAXF(8.0f, u->popup_rect.y - h - 6 * u->L.scale);
    /* keep it on screen horizontally, remembering RTL's leading edge */
    if (x + w > u->c->w - 8) x = u->c->w - w - 8;
    if (x < 8) x = 8;
    osa_rect box = rectf(x, y, w, h);
    c_soft_shadow(u->c, box, u->L.radius_sm, 16 * u->L.scale, 6 * u->L.scale, u->t.shadow);
    c_fill_round(u->c, box, u->L.radius_sm, u->t.panel_hover);
    c_stroke_round(u->c, box, u->L.radius_sm, 1.0f, u->t.border_strong);
    int result = -1;
    for (int i = 0; i < n; i++) {
        osa_rect rr = rectf(box.x + pad, box.y + pad + i * row_h, box.w - pad * 2, row_h);
        bool hov = hover(u, rr);
        bool sel = (i == u->popup_sel);
        if (hov) c_fill_round(u->c, rr, u->L.radius_sm * 0.7f, u->t.panel_alt);
        if (sel) {
            c_fill_round(u->c, rr, u->L.radius_sm * 0.7f,
                         col_alpha(u->t.accent, hov ? 0.30f : 0.20f));
        }
        ui_label(u, rectf(rr.x + 12 * u->L.scale, rr.y, rr.w - 40 * u->L.scale, rr.h),
                 u->popup_items[i], UI_FS_BODY * u->L.scale,
                 sel ? u->t.accent : u->t.text, u->L.rtl ? 2 : 0);
        if (sel) {
            /* check mark on the trailing edge */
            float cx = u->L.rtl ? rr.x + 18 * u->L.scale : rr.x + rr.w - 18 * u->L.scale;
            float cy = rr.y + rr.h * 0.5f;
            float s = 3.4f * u->L.scale;
            c_line(u->c, cx - s, cy, cx, cy + s * 0.9f, 1.8f * u->L.scale, u->t.accent);
            c_line(u->c, cx, cy + s * 0.9f, cx + s * 1.3f, cy - s * 0.9f, 1.8f * u->L.scale,
                   u->t.accent);
        }
        if (hov && u->in.clicked && !u->consumed_click) {
            u->consumed_click = true;
            result = i;
        }
    }
    /* A click anywhere else dismisses the list. */
    if (u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        if (!hover(u, box)) result = -1;
        u->popup_id = 0;
        return result;
    }
    if (result >= 0) u->popup_id = 0;
    return result;
}
