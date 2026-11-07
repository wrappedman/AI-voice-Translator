/* =====================================================================
 *  OpenSpeech AI — UI kit implementation
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

/* ---- fonts -------------------------------------------------------- */
static const char *pick_font(bool bold) {
    /* prefer the bundled Vazirmatn; it covers Latin and Persian in one file so
     * mixed-script lines never fall back to a second font */
    static const char *regular[] = {
        "src/ui/assets/Vazirmatn-Regular.ttf",
        "src/ui/assets/Vazirmatn-Medium.ttf",
        "assets/Vazirmatn-Regular.ttf",
        NULL
    };
    static const char *heavy[] = {
        "src/ui/assets/Vazirmatn-Bold.ttf",
        "src/ui/assets/Vazirmatn-Medium.ttf",
        "src/ui/assets/Vazirmatn-Regular.ttf",
        NULL
    };
    const char **list = bold ? heavy : regular;
    for (int i = 0; list[i]; i++)
        if (osa_file_exists(list[i])) return list[i];
    return NULL;
}

void ui_set_theme(ui_context *u, bool dark) {
    ui_theme *t = &u->t;
    if (dark) {
        t->bg          = hexc(0x0E1116);
        t->panel       = hexc(0x161B22);
        t->panel_alt   = hexc(0x1C232C);
        t->sidebar     = hexc(0x11161C);
        t->border      = hexc(0x2A313B);
        t->border_soft = hexc(0x21272F);
        t->text        = hexc(0xE9EEF5);
        t->text_dim    = hexc(0x9AA5B4);
        t->text_faint  = hexc(0x6C7787);
        t->accent      = hexc(0x4C8DFF);
        t->accent_hover= hexc(0x6BA1FF);
        t->accent_soft = hexc(0x1B3357);
        t->accent2    = hexc(0xA371F7);
        t->danger      = hexc(0xFF6B6B);
        t->success     = hexc(0x3FB950);
        t->warning     = hexc(0xD29922);
        t->selection   = hexc(0x24446F);
        t->wave        = hexc(0x5B9DFF);
        t->wave_bg     = hexc(0x1A2230);
    } else {
        t->bg          = hexc(0xF6F8FB);
        t->panel       = hexc(0xFFFFFF);
        t->panel_alt   = hexc(0xF1F4F8);
        t->sidebar     = hexc(0xFFFFFF);
        t->border      = hexc(0xDCE2EA);
        t->border_soft = hexc(0xE9EEF4);
        t->text        = hexc(0x11161C);
        t->text_dim    = hexc(0x5B6675);
        t->text_faint  = hexc(0x8B96A5);
        t->accent      = hexc(0x1F6FEB);
        t->accent_hover= hexc(0x1158C7);
        t->accent_soft = hexc(0xE3EEFF);
        t->accent2    = hexc(0x7C3AED);
        t->danger      = hexc(0xD1242F);
        t->success     = hexc(0x1A7F37);
        t->warning     = hexc(0x9A6700);
        t->selection   = hexc(0xD7E6FF);
        t->wave        = hexc(0x1F6FEB);
        t->wave_bg     = hexc(0xEDF2F9);
    }
}

void ui_set_rtl(ui_context *u, bool rtl) { u->L.rtl = rtl; }

void ui_set_scale(ui_context *u, float s) {
    if (s < 0.75f) s = 0.75f;
    if (s > 3.0f) s = 3.0f;
    u->L.scale = s;
    u->L.pad = 8.0f * s;
    u->L.radius = 10.0f * s;
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
    return true;
}

void ui_shutdown(ui_context *u) {
    if (!u) return;
    if (u->reg) text_font_release(u->reg);
    if (u->bold && u->bold != u->reg) text_font_release(u->bold);
    u->reg = u->bold = NULL;
    text_shutdown();
}

/* ---- helpers ------------------------------------------------------ */

osa_rect ui_split_leading(osa_rect *r, float w, bool rtl) {
    if (!rtl) return rect_split_left(r, w);
    return rect_split_right(r, w);
}

float ui_text_h(ui_context *u, float size) { return text_line_height(u->reg, size); }

void ui_panel(ui_context *u, osa_rect r) { c_fill(u->c, r, u->t.panel); }

void ui_card(ui_context *u, osa_rect r) {
    c_fill_round(u->c, r, u->L.radius, u->t.panel);
    c_stroke_round(u->c, r, u->L.radius, 1.0f, u->t.border);
}

void ui_separator(ui_context *u, osa_rect r) {
    c_fill(u->c, rectf(r.x, r.y + r.h * 0.5f, r.w, MAXF(1.0f, u->L.scale)), u->t.border_soft);
}

void ui_label(ui_context *u, osa_rect r, const char *text, float size, osa_color col, int align) {
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

void ui_label_wrap(ui_context *u, osa_rect r, const char *text, float size, osa_color col) {
    text_opts o = text_default_opts(size);
    o.max_width = r.w;
    o.rtl = u->L.rtl;
    text_draw_box(u->c, u->reg, size, col, r, text, o);
}

/* ---- widgets ------------------------------------------------------ */

static bool hit(ui_context *u, int id, osa_rect r) {
    bool inside = rect_has(r, u->in.mx, u->in.my);
    if (inside && !u->consumed_click) u->hot_id = id;
    bool clicked = inside && u->in.clicked && !u->consumed_click;
    if (clicked) u->consumed_click = true;
    return clicked;
}

bool ui_button(ui_context *u, int id, osa_rect r, const char *label, bool primary) {
    bool hov = rect_has(r, u->in.mx, u->in.my);
    osa_color bg = primary ? u->t.accent : u->t.panel_alt;
    if (hov) bg = primary ? u->t.accent_hover : col_lighten(u->t.panel_alt, 0.05f);
    if (u->active_id == id && u->in.down) bg = col_darken(bg, 0.12f);
    c_fill_round(u->c, r, u->L.radius * 0.7f, bg);
    if (!primary) c_stroke_round(u->c, r, u->L.radius * 0.7f, 1.0f, u->t.border);
    float size = 13.0f * u->L.scale;
    osa_color fg = primary ? rgba(255, 255, 255, 255) : u->t.text;
    text_opts o = text_default_opts(size);
    o.align = 1;
    o.max_width = r.w - 12 * u->L.scale;
    o.max_lines = 1;
    o.ellipsis = true;
    o.rtl = u->L.rtl;
    text_draw_box(u->c, u->bold, size, fg,
                  rectf(r.x, r.y + (r.h - text_line_height(u->bold, size)) * 0.5f, r.w,
                        text_line_height(u->bold, size)), label, o);
    bool clicked = hit(u, id, r);
    if (clicked) u->active_id = id;
    return clicked;
}

bool ui_button_icon(ui_context *u, int id, osa_rect r, const char *glyph,
                    const char *label, bool active) {
    bool hov = rect_has(r, u->in.mx, u->in.my);
    if (active) c_fill_round(u->c, r, u->L.radius * 0.7f, u->t.accent_soft);
    else if (hov) c_fill_round(u->c, r, u->L.radius * 0.7f, u->t.panel_alt);
    if (active) c_stroke_round(u->c, r, u->L.radius * 0.7f, 1.0f, col_alpha(u->t.accent, 0.5f));
    float size = 13.0f * u->L.scale;
    float glyph_w = glyph && *glyph ? text_measure(u->bold, size, glyph) + 8 * u->L.scale : 0;
    float asc = text_ascent(u->bold, size);
    float base = r.y + (r.h + asc - text_descent(u->bold, size)) * 0.5f;
    osa_color fg = active ? u->t.accent : u->t.text;
    if (glyph && *glyph) {
        if (!u->L.rtl) text_draw(u->c, u->bold, size, fg, r.x + 10 * u->L.scale, base, glyph);
        else text_draw(u->c, u->bold, size, fg, r.x + r.w - 10 * u->L.scale - glyph_w, base, glyph);
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
    c_fill_round(u->c, track, sh * 0.5f, *value ? u->t.accent : u->t.border);
    float kr = sh * 0.5f - 2.5f * u->L.scale;
    float kx = *value ? track.x + track.w - sh * 0.5f : track.x + sh * 0.5f;
    c_circle(u->c, kx, track.y + sh * 0.5f, kr, rgba(255, 255, 255, 255));
    if (label) {
        osa_rect lr = rectf(r.x, r.y, r.w - sw - 10 * u->L.scale, r.h);
        ui_label(u, lr, label, 13.0f * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    }
    if (hit(u, id, r)) { *value = !*value; return true; }
    return false;
}

bool ui_tab(ui_context *u, int id, osa_rect r, const char *label, bool selected) {
    bool hov = rect_has(r, u->in.mx, u->in.my);
    if (selected) {
        c_fill(u->c, rectf(r.x, r.y + r.h - 2 * u->L.scale, r.w, 2 * u->L.scale), u->t.accent);
    } else if (hov) {
        c_fill(u->c, rectf(r.x, r.y + r.h - 2 * u->L.scale, r.w, 2 * u->L.scale), u->t.border);
    }
    ui_label(u, r, label, 13.0f * u->L.scale,
             selected ? u->t.text : u->t.text_dim, 1);
    return hit(u, id, r);
}

bool ui_nav_item(ui_context *u, int id, osa_rect r, const char *icon,
                 const char *label, bool selected) {
    bool hov = rect_has(r, u->in.mx, u->in.my);
    if (selected) c_fill_round(u->c, r, u->L.radius * 0.7f, u->t.accent_soft);
    else if (hov) c_fill_round(u->c, r, u->L.radius * 0.7f, u->t.panel_alt);
    osa_color fg = selected ? u->t.accent : u->t.text_dim;
    float size = 13.0f * u->L.scale;
    float asc = text_ascent(u->bold, size);
    float base = r.y + (r.h + asc - text_descent(u->bold, size)) * 0.5f;
    float ix = u->L.rtl ? r.x + r.w - 14 * u->L.scale : r.x + 14 * u->L.scale;
    if (icon && *icon) {
        if (!u->L.rtl) text_draw(u->c, u->bold, size, fg, ix, base, icon);
        else text_draw(u->c, u->bold, size, fg, ix - text_measure(u->bold, size, icon), base, icon);
    }
    osa_rect lr = rectf(r.x + (u->L.rtl ? 10 : 38) * u->L.scale, r.y,
                        r.w - 48 * u->L.scale, r.h);
    ui_label(u, lr, label, size, selected ? u->t.text : u->t.text_dim, u->L.rtl ? 2 : 0);
    return hit(u, id, r);
}

void ui_status_chip(ui_context *u, osa_rect r, const char *label, osa_color dot) {
    /* A chip hugs its label: callers pass the maximum width they can spare,
     * not the width the pill should pretend to have. */
    float fs = 11.0f * u->L.scale;
    float pad = 11 * u->L.scale;
    float inset = pad + 9 * u->L.scale;
    float need = text_measure(u->reg, fs, label) + inset + 10 * u->L.scale;
    if (need < r.h + 8 * u->L.scale) need = r.h + 8 * u->L.scale;
    if (need < r.w) {                       /* shrink from the trailing edge */
        if (u->L.rtl) r.x += r.w - need; else r.w = need;
        if (u->L.rtl) r.w = need;
    }
    c_fill_round(u->c, r, r.h * 0.5f, u->t.panel_alt);
    float cy = r.y + r.h * 0.5f;
    float dx = u->L.rtl ? r.x + r.w - pad : r.x + pad;
    c_circle(u->c, dx, cy, 3.5f * u->L.scale, dot);
    osa_rect lr = rectf(u->L.rtl ? r.x + 6 * u->L.scale : r.x + inset, r.y,
                        r.w - inset - 6 * u->L.scale, r.h);
    ui_label(u, lr, label, fs, u->t.text_dim, u->L.rtl ? 2 : 0);
}

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
        osa_rect fill = rectf(r.x, r.y, w, r.h);
        c_fill_round(u->c, fill, r.h * 0.5f, col);
    }
}

void ui_waveform(ui_context *u, osa_rect r, const float *peaks, int n,
                 double playhead, bool playing) {
    c_fill_round(u->c, r, u->L.radius * 0.7f, u->t.wave_bg);
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
        float h = p * (r.h - 12 * u->L.scale);
        float x = r.x + (float)i * bw;
        double pos = (double)i / (double)bars;
        osa_color col = (playing && pos <= playhead) ? u->t.accent : u->t.wave;
        c_fill_round(u->c, rectf(x, mid - h * 0.5f, MAXF(1.5f, bw - 1.0f * u->L.scale), h),
                     bw * 0.4f, col_alpha(col, playing ? (pos <= playhead ? 0.95f : 0.45f) : 0.8f));
    }
    if (playing) {
        float px = r.x + r.w * (float)playhead;
        c_vline(u->c, px, r.y + 4, r.y + r.h - 4, 2.0f * u->L.scale, u->t.text);
    }
}

osa_rect ui_dialog(ui_context *u, float w, float h, const char *title) {
    float sw = u->c->w, sh = u->c->h;
    c_fill(u->c, rectf(0, 0, sw, sh), rgba(0, 0, 0, 130));
    osa_rect d = rectf((sw - w) * 0.5f, (sh - h) * 0.5f, w, h);
    c_fill_round(u->c, d, u->L.radius * 1.3f, u->t.panel);
    c_stroke_round(u->c, d, u->L.radius * 1.3f, 1.0f, u->t.border);
    if (title) {
        ui_label(u, rectf(d.x + 18 * u->L.scale, d.y + 10 * u->L.scale,
                          d.w - 36 * u->L.scale, 30 * u->L.scale),
                 title, 16.0f * u->L.scale, u->t.text, u->L.rtl ? 2 : 0);
    }
    return d;
}

void ui_bullet_list(ui_context *u, osa_rect r, const char *const *items, int n,
                    float size, osa_color col) {
    float lh = text_line_height(u->reg, size) * 1.15f;
    float y = r.y;
    for (int i = 0; i < n; i++) {
        float bh = text_box_height(u->reg, size, rectf(r.x + 14 * u->L.scale, y,
                                                      r.w - 14 * u->L.scale, 400),
                                   items[i], text_default_opts(size));
        if (bh < lh) bh = lh;
        if (y + bh > r.y + r.h) break;
        c_circle(u->c, u->L.rtl ? r.x + r.w - 6 * u->L.scale : r.x + 5 * u->L.scale,
                 y + lh * 0.42f, 2.5f * u->L.scale, u->t.accent);
        text_opts o = text_default_opts(size);
        o.max_width = r.w - 14 * u->L.scale;
        o.rtl = u->L.rtl;
        text_draw_box(u->c, u->reg, size, col,
                      rectf(r.x + 14 * u->L.scale, y, r.w - 14 * u->L.scale, bh), items[i], o);
        y += bh + 4 * u->L.scale;
    }
}

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

/* =====================================================================
 *  Interaction widgets added for the screens
 * ===================================================================== */

bool ui_hit(ui_context *u, osa_rect r) {
    return r.x <= u->in.mx && u->in.mx < r.x + r.w && r.y <= u->in.my && u->in.my < r.y + r.h;
}

bool ui_row(ui_context *u, int id, osa_rect r, bool selected) {
    bool hover = ui_hit(u, r);
    if (hover) u->hot_id = id;
    osa_color bg = selected ? col_alpha(u->t.accent, 0.14f)
                            : hover ? col_alpha(u->t.text, 0.05f) : rgba(0, 0, 0, 0);
    if (selected || hover) c_fill_round(u->c, r, u->L.radius * 0.7f, bg);
    if (selected) {
        float bar_w = 3 * u->L.scale;
        osa_rect bar = rectf(u->L.rtl ? r.x + r.w - bar_w - 1 : r.x + 1,
                             r.y + 8 * u->L.scale, bar_w, r.h - 16 * u->L.scale);
        c_fill_round(u->c, bar, bar_w * 0.5f, u->t.accent);
    }
    bool clicked = hover && u->in.clicked && !u->consumed_click;
    if (clicked) u->consumed_click = true;
    return clicked;
}

bool ui_segmented(ui_context *u, int id, osa_rect r, const char *labels, int *value, int n) {
    if (n <= 0) return false;
    c_fill_round(u->c, r, r.h * 0.5f, u->t.panel_alt);
    float seg_w = r.w / n;
    char part[128];
    const char *p = labels;
    bool changed = false;
    for (int i = 0; i < n; i++) {
        size_t k = 0;
        while (*p && *p != '|' && k + 1 < sizeof part) part[k++] = *p++;
        part[k] = 0;
        if (*p == '|') p++;
        osa_rect sr = rectf(r.x + i * seg_w + 2, r.y + 2, seg_w - 4, r.h - 4);
        bool sel = *value == i;
        if (sel) c_fill_round(u->c, sr, sr.h * 0.5f, u->t.accent);
        ui_label(u, sr, part, 11 * u->L.scale,
                 sel ? rgba(255, 255, 255, 255) : u->t.text_dim, 1);
        if (ui_hit(u, sr) && u->in.clicked && !u->consumed_click && !sel) {
            u->consumed_click = true;
            *value = i;
            changed = true;
            (void)id;
        }
    }
    return changed;
}

bool ui_stepper(ui_context *u, int id, osa_rect r, int *value, int vmin, int vmax) {
    bool changed = false;
    float bw = r.h;
    osa_rect minus = rectf(u->L.rtl ? r.x + r.w - bw : r.x, r.y, bw, r.h);
    osa_rect plus = rectf(u->L.rtl ? r.x : r.x + r.w - bw, r.y, bw, r.h);
    osa_rect mid = rectf(r.x + bw + 2, r.y, r.w - bw * 2 - 4, r.h);
    c_fill_round(u->c, mid, r.h * 0.3f, u->t.panel_alt);
    c_stroke_round(u->c, r, r.h * 0.3f, 1, u->t.border);
    char num[32];
    snprintf(num, sizeof num, "%d", *value);
    ui_label(u, mid, num, 12 * u->L.scale, u->t.text, 1);

    for (int k = 0; k < 2; k++) {
        osa_rect br = k == 0 ? minus : plus;
        bool hover = ui_hit(u, br);
        if (hover) c_fill_round(u->c, br, r.h * 0.3f, col_alpha(u->t.text, 0.06f));
        const char *g = k == 0 ? "\xE2\x88\x92" : "+";  /* U+2212 MINUS SIGN */
        ui_label(u, br, g, 15 * u->L.scale, hover ? u->t.text : u->t.text_dim, 1);
        if (hover && u->in.clicked && !u->consumed_click) {
            u->consumed_click = true;
            int nv = *value + (k == 0 ? -1 : 1);
            if (nv < vmin) nv = vmin;
            if (nv > vmax) nv = vmax;
            if (nv != *value) { *value = nv; changed = true; }
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
    osa_rect fill = rectf(tr.x, tr.y, tr.w * t, tr.h);
    c_fill_round(u->c, fill, 3 * u->L.scale, u->t.accent);
    float kx = truncf(tr.x + tr.w * t);
    c_circle(u->c, kx, tr.y + tr.h * 0.5f, 8 * u->L.scale, rgba(255, 255, 255, 255));
    c_circle(u->c, kx, tr.y + tr.h * 0.5f, 7 * u->L.scale, u->t.accent);

    if (ui_hit(u, r) && u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        u->active_id = id;
    }
    if (u->active_id == id) {
        if (u->in.down) {
            float nt = (u->in.mx - tr.x) / tr.w;
            if (nt < 0) nt = 0;
            if (nt > 1) nt = 1;
            float nv = vmin + nt * (vmax - vmin);
            if (nv != *value) { *value = nv; changed = true; }
        } else {
            u->active_id = 0;
        }
    }
    return changed;
}

bool ui_toggle_switch(ui_context *u, int id, osa_rect r, bool *value) {
    bool hover = ui_hit(u, r);
    if (hover) u->hot_id = id;
    bool on = *value;
    osa_color track = on ? u->t.accent : col_alpha(u->t.text_dim, 0.30f);
    c_fill_round(u->c, r, r.h * 0.5f, track);
    float kr = r.h * 0.5f - 3 * u->L.scale;
    float kx = on ? r.x + r.w - kr - 3 * u->L.scale : r.x + kr + 3 * u->L.scale;
    c_circle(u->c, kx, r.y + r.h * 0.5f, kr, rgba(255, 255, 255, 255));
    bool clicked = hover && u->in.clicked && !u->consumed_click;
    if (clicked) {
        u->consumed_click = true;
        *value = !on;
    }
    return clicked;
}

bool ui_text_field(ui_context *u, int id, osa_rect r, char *buf, size_t cap, int max_chars) {
    (void)max_chars;
    bool hover = ui_hit(u, r);
    c_fill_round(u->c, r, u->L.radius * 0.6f, u->t.panel_alt);
    c_stroke_round(u->c, r, u->L.radius * 0.6f, 1,
                   u->active_id == id ? u->t.accent : u->t.border);
    if (hover && u->in.clicked && !u->consumed_click) {
        u->consumed_click = true;
        u->active_id = id;
    }
    osa_rect tr = rect_inset(r, 10 * u->L.scale);
    float caret = 0;
    if (u->active_id == id) {
        size_t n = strlen(buf);
        for (int i = 0; i < u->in.n_chars; i++) {
            if (n + 1 >= cap) break;
            unsigned cp = u->in.chars[i];
            if (cp == 8) {
                if (n > 0) {
                    n--;
                    while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;
                    buf[n] = 0;
                }
            } else if (cp >= 32) {
                char tmp[4];
                int k = 0;
                if (cp < 0x80) tmp[k++] = (char)cp;
                else if (cp < 0x800) { tmp[k++] = (char)(0xC0 | (cp >> 6)); tmp[k++] = (char)(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { tmp[k++] = (char)(0xE0 | (cp >> 12)); tmp[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[k++] = (char)(0x80 | (cp & 0x3F)); }
                else { tmp[k++] = (char)(0xF0 | (cp >> 18)); tmp[k++] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[k++] = (char)(0x80 | (cp & 0x3F)); }
                if (n + (size_t)k + 1 >= cap) break;
                memcpy(buf + n, tmp, (size_t)k);
                n += (size_t)k;
                buf[n] = 0;
            }
        }
        caret = 1;
    }
    bool rtl = text_is_rtl(buf);
    float base = tr.y + (tr.h + text_ascent(u->reg, 13 * u->L.scale)) * 0.5f;
    float w = text_measure(u->reg, 13 * u->L.scale, buf);
    if (rtl) {
        float x = tr.x + MAXF(0.0f, tr.w - w);
        text_draw(u->c, u->reg, 13 * u->L.scale, u->t.text, x, base, buf);
    } else {
        text_draw(u->c, u->reg, 13 * u->L.scale, u->t.text, tr.x, base, buf);
    }
    if (caret) {
        float cx = rtl ? tr.x + tr.w - MINF(w, tr.w) : tr.x + MINF(w, tr.w);
        c_vline(u->c, cx + 1, tr.y + 3, tr.y + tr.h - 3, 1.5f, u->t.accent);
    }
    return u->in.n_chars > 0 && u->active_id == id;
}

bool ui_text_area(ui_context *u, int id, osa_rect r, char *buf, size_t cap, int max_chars) {
    return ui_text_field(u, id, r, buf, cap, max_chars);
}

bool ui_has_focus(ui_context *u) { return u->active_id != 0; }
int  ui_focus_id(ui_context *u) { return u->active_id; }
