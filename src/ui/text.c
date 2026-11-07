#include "text.h"
#include <math.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include <hb.h>
#include <hb-ft.h>
#include "bidi.h"

/* ---------------------------- state ---------------------------- */
static FT_Library g_ft;
static bool g_text_ready;
static hb_font_t *g_hb_dummy;

struct osa_font {
    char path[OSAI_MAX_PATH];
    int face_index;
    osa_font_weight weight;
    FT_Face ft_face;
    hb_font_t *hb_font;
    bool synth_bold;
};

/* glyph cache -------------------------------------------------------- */
typedef struct gcache_ent {
    uint32_t key;        /* glyph<<8 | px bucket */
    int w, h, bx, by;
    uint8_t *bm;
    float adv;
    struct gcache_ent *next;
} gcache_ent;

#define GCACHE_BUCKETS 1024
#define GCACHE_MAX 3000
static gcache_ent *g_gcache[GCACHE_BUCKETS];
static int g_gcache_count;
static osa_font *g_cache_font;

static void gcache_flush(void) {
    for (int i = 0; i < GCACHE_BUCKETS; i++) {
        gcache_ent *e = g_gcache[i];
        while (e) {
            gcache_ent *n = e->next;
            free(e->bm);
            free(e);
            e = n;
        }
        g_gcache[i] = NULL;
    }
    g_gcache_count = 0;
    g_cache_font = NULL;
}

/* a small font stack so weight fallback works even if a face is missing */
static osa_font *g_fonts[4];
static char g_font_dir[OSAI_MAX_PATH];

bool text_init(void) {
    if (g_text_ready) return true;
    if (FT_Init_FreeType(&g_ft) != 0) return false;
    g_hb_dummy = hb_font_get_empty();
    /* fonts are loaded from <exe_dir>/assets/fonts, then <assets>, then cwd */
    osa_strlcpy(g_font_dir, osa_exe_dir(), sizeof g_font_dir);
    g_text_ready = true;
    return true;
}

void text_shutdown(void) {
    if (!g_text_ready) return;
    gcache_flush();
    for (int i = 0; i < 4; i++) {
        if (g_fonts[i]) {
            if (g_fonts[i]->hb_font) hb_font_destroy(g_fonts[i]->hb_font);
            if (g_fonts[i]->ft_face) FT_Done_Face(g_fonts[i]->ft_face);
            free(g_fonts[i]);
            g_fonts[i] = NULL;
        }
    }
    if (g_hb_dummy) { hb_font_destroy(g_hb_dummy); g_hb_dummy = NULL; }
    FT_Done_FreeType(g_ft);
    g_ft = NULL;
    g_text_ready = false;
}

osa_font *text_font_load(const char *path, osa_font_weight w) {
    if (!text_init()) return NULL;
    osa_font *f = (osa_font *)calloc(1, sizeof *f);
    if (!f) return NULL;
    osa_strlcpy(f->path, path, sizeof f->path);
    f->weight = w;
    if (FT_New_Face(g_ft, path, 0, &f->ft_face) != 0) { free(f); return NULL; }
    f->hb_font = hb_ft_font_create_referenced(f->ft_face);
    hb_ft_font_set_load_flags(f->hb_font, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING);
    return f;
}

void text_font_release(osa_font *f) {
    if (!f) return;
    if (f->hb_font) hb_font_destroy(f->hb_font);
    if (f->ft_face) FT_Done_Face(f->ft_face);
    free(f);
}

static const char *weight_file(osa_font_weight w) {
    switch (w) {
        case OSA_FW_BOLD:   return "Vazirmatn-Bold.ttf";
        case OSA_FW_MEDIUM: return "Vazirmatn-Medium.ttf";
        case OSA_FW_MONO:   return "Vazirmatn-Medium.ttf";
        default:            return "Vazirmatn-Regular.ttf";
    }
}

/* try a sequence of candidate directories for a bundled asset */
static bool find_asset(const char *name, char *out, size_t cap) {
    const char *bases[5];
    char b1[OSAI_MAX_PATH], b2[OSAI_MAX_PATH], b3[OSAI_MAX_PATH];
    snprintf(b1, sizeof b1, "%s/assets", osa_exe_dir());
    snprintf(b2, sizeof b2, "%s", osa_exe_dir());
    snprintf(b3, sizeof b3, "src/ui/assets");
    bases[0] = b1; bases[1] = b2; bases[2] = b3;
    bases[3] = "assets"; bases[4] = "../src/ui/assets";
    for (int i = 0; i < 5; i++) {
        osa_path_join(out, cap, bases[i], name);
        if (osa_file_exists(out)) return true;
    }
    return false;
}

osa_font *text_font_default(osa_font_weight w) {
    if (w > OSA_FW_MONO) w = OSA_FW_REGULAR;
    if (g_fonts[w]) return g_fonts[w];
    char path[OSAI_MAX_PATH] = {0};
    if (!find_asset(weight_file(w), path, sizeof path)) {
        /* last-ditch: any regular face we can find */
        if (!find_asset("Vazirmatn-Regular.ttf", path, sizeof path)) return NULL;
    }
    g_fonts[w] = text_font_load(path, w);
    return g_fonts[w];
}

text_opts text_default_opts(float size) {
    text_opts o;
    memset(&o, 0, sizeof o);
    o.line_height = size * 1.55f;
    o.align = 0;
    o.rtl = false;
    o.max_width = 0;
    return o;
}

/* ---------------------------- helpers ---------------------------- */
float text_ascent(const osa_font *f, float size) {
    if (!f) return size * 0.8f;
    FT_Set_Char_Size(f->ft_face, 0, (FT_F26Dot6)(size * 64.0f), 72, 72);
    return (float)f->ft_face->size->metrics.ascender / 64.0f;
}
float text_descent(const osa_font *f, float size) {
    if (!f) return size * 0.25f;
    FT_Set_Char_Size(f->ft_face, 0, (FT_F26Dot6)(size * 64.0f), 72, 72);
    return -(float)f->ft_face->size->metrics.descender / 64.0f;
}
float text_line_height(const osa_font *f, float size) { return size * 1.55f; }

bool text_is_rtl(const char *utf8) {
    if (!utf8) return false;
    size_t len = strlen(utf8);
    size_t i = 0;
    while (i < len) {
        osa_rune r = 0;
        int n = osa_utf8_decode(utf8 + i, len - i, &r);
        if (n <= 0) break;
        if (osa_rune_is_rtl(r)) return true;
        /* first strong LTR character decides LTR */
        if ((r >= 'A' && r <= 'Z') || (r >= 'a' && r <= 'z') ||
            (r >= 0x00C0 && r <= 0x024F)) return false;
        i += (size_t)n;
    }
    return false;
}

void text_localize_digits(const char *utf8, const char *lang, char *out, size_t cap) {
    if (!utf8) { if (cap) out[0] = 0; return; }
    bool fa = lang && (osa_starts(lang, "fa") || osa_starts(lang, "ar"));
    size_t o = 0;
    for (const char *p = utf8; *p && o + 3 < cap; p++) {
        if (fa && *p >= '0' && *p <= '9') {
            static const char *fa_digits[10] = { "۰","۱","۲","۳","۴","۵","۶","۷","۸","۹" };
            const char *d = fa_digits[*p - '0'];
            out[o++] = d[0]; out[o++] = d[1];
        } else if (fa && *p == '%') {
            out[o++] = (char)0xD9; out[o++] = (char)0xAA;   /* ٪ */
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

/* shape a whole string into glyphs with positions */
typedef struct shaped_glyph { uint32_t glyph; float x_adv; float x_off, y_off; } shaped_glyph;

static int shape_run(osa_font *f, float size, const char *utf8, size_t len,
                     shaped_glyph *out, int maxout, bool rtl) {
    if (!f || !f->hb_font || !g_ft) return 0;
    FT_Set_Char_Size(f->ft_face, 0, (FT_F26Dot6)(size * 64.0f), 72, 72);
    hb_buffer_t *buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, utf8, (int)len, 0, (int)len);
    hb_buffer_set_direction(buf, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    
    hb_buffer_guess_segment_properties(buf);
    hb_buffer_set_direction(buf, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_shape(f->hb_font, buf, NULL, 0);
    unsigned n = 0;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &n);
    int k = 0;
    for (unsigned i = 0; i < n && k < maxout; i++) {
        out[k].glyph = info[i].codepoint;
        out[k].x_adv = (float)pos[i].x_advance / 64.0f;
        out[k].x_off = (float)pos[i].x_offset / 64.0f;
        out[k].y_off = -(float)pos[i].y_offset / 64.0f;
        k++;
    }
    hb_buffer_destroy(buf);
    return k;
}

static gcache_ent *gcache_get(osa_font *f, uint32_t glyph, int px) {
    uint32_t key = (glyph << 8) ^ (uint32_t)px;
    int b = (int)(key % GCACHE_BUCKETS);
    if (g_cache_font != f && g_gcache_count > 0) { gcache_flush(); }
    g_cache_font = f;
    for (gcache_ent *e = g_gcache[b]; e; e = e->next) if (e->key == key) return e;
    if (g_gcache_count > GCACHE_MAX) gcache_flush();

    FT_Set_Char_Size(f->ft_face, 0, (FT_F26Dot6)((float)px * 64.0f), 72, 72);
    if (FT_Load_Glyph(f->ft_face, glyph, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING) != 0)
        return NULL;
    if (f->synth_bold) FT_GlyphSlot_Embolden(f->ft_face->glyph);
    if (FT_Render_Glyph(f->ft_face->glyph, FT_RENDER_MODE_NORMAL) != 0) return NULL;
    FT_GlyphSlot g = f->ft_face->glyph;
    gcache_ent *e = (gcache_ent *)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->key = key;
    e->w = (int)g->bitmap.width;
    e->h = (int)g->bitmap.rows;
    e->bx = g->bitmap_left;
    e->by = g->bitmap_top;
    e->adv = (float)g->advance.x / 64.0f;
    if (e->w > 0 && e->h > 0) {
        e->bm = (uint8_t *)malloc((size_t)e->w * (size_t)e->h);
        if (e->bm) {
            for (int y = 0; y < e->h; y++)
                memcpy(e->bm + (size_t)y * e->w, g->bitmap.buffer + (size_t)y * g->bitmap.pitch, (size_t)e->w);
        }
    }
    e->next = g_gcache[b];
    g_gcache[b] = e;
    g_gcache_count++;
    return e;
}

/* draws one shaped run, x = pen start (already aligned), returns advance */
static float draw_run(osa_canvas *c, osa_font *f, float size, osa_color col,
                      float x, float yb, const shaped_glyph *gl, int n, bool rtl) {
    /* HarfBuzz emits glyphs in visual order (left to right) for every direction,
       so the pen always advances to the right. */
    (void)rtl;
    float pen = x;
    int px = (int)(size + 0.5f);
    if (px < 6) px = 6;
    for (int i = 0; i < n; i++) {
        gcache_ent *e = gcache_get(f, gl[i].glyph, px);
        if (e && e->bm && c) {
            int gx = (int)floorf(pen + gl[i].x_off) + e->bx;
            int gy = (int)floorf(yb + gl[i].y_off) - e->by;
            c_blend_sprite(c, gx, gy, e->w, e->h, e->bm, e->w, col);
        }
        pen += gl[i].x_adv;
    }
    return pen - x;
}

static float run_advance(osa_font *f, float size, const char *utf8, size_t len, bool rtl) {
    shaped_glyph gl[1024];
    int n = shape_run(f, size, utf8, len, gl, 1024, rtl);
    float w = 0;
    for (int i = 0; i < n; i++) w += gl[i].x_adv;
    return w;
}

/* ---------------------------- layout ---------------------------------- */
/* Mirrors brackets/parens according to resolved bidi levels (rule L4). */
static size_t run_text(const char *utf8, size_t len, bool rtl, char *out, size_t cap) {
    size_t o = 0, i = 0;
    while (i < len && o + 5 < cap) {
        osa_rune r = 0;
        int n = osa_utf8_decode(utf8 + i, len - i, &r);
        if (n <= 0) n = 1;
        if (rtl) {
            switch (r) {
                case '(': r = ')'; break;
                case ')': r = '('; break;
                case '[': r = ']'; break;
                case ']': r = '['; break;
                case '{': r = '}'; break;
                case '}': r = '{'; break;
                case '<': r = '>'; break;
                case '>': r = '<'; break;
                case 0x00AB: r = 0x00BB; break;
                case 0x00BB: r = 0x00AB; break;
                default: break;
            }
        }
        o += (size_t)osa_utf8_encode(r, out + o);
        i += (size_t)n;
    }
    out[o] = 0;
    return o;
}

/* Returns the run slice (no character reversal: HarfBuzz handles shaping and
 * mirroring of RTL runs internally). */
static const char *prep_run(const char *utf8, const bidi_run *r, char *scratch, size_t cap,
                            size_t *out_len) {
    (void)scratch; (void)cap;
    *out_len = r->len;
    return utf8 + r->start;
}

float text_measure(const osa_font *f, float size, const char *utf8) {
    if (!utf8 || !*utf8 || !f) return 0;
    osa_font *ff = (osa_font *)f;
    bidi_run runs[BIDI_MAX_RUNS];
    bool prtl = false;
    int nr = bidi_paragraph(utf8, runs, BIDI_MAX_RUNS, &prtl);
    float total = 0;
    char scratch[4096];
    for (int i = 0; i < nr; i++) {
        size_t tl = 0;
        const char *txt = prep_run(utf8, &runs[i], scratch, sizeof scratch, &tl);
        total += run_advance(ff, size, txt, tl, runs[i].rtl);
    }
    return total;
}

/* Draws one paragraph line at `x` (left edge). Returns advance width. */
float text_draw(osa_canvas *c, const osa_font *f, float size, osa_color col,
                float x, float y_baseline, const char *utf8) {
    if (!f || !utf8 || !*utf8) return 0;
    osa_font *ff = (osa_font *)f;
    bidi_run runs[BIDI_MAX_RUNS];
    bool prtl = false;
    int nr = bidi_paragraph(utf8, runs, BIDI_MAX_RUNS, &prtl);
    char scratch[4096];
    shaped_glyph gl[1024];
    float pen = x;
    for (int i = 0; i < nr; i++) {
        size_t tl = 0;
        const char *txt = prep_run(utf8, &runs[i], scratch, sizeof scratch, &tl);
        int n = shape_run(ff, size, txt, tl, gl, 1024, runs[i].rtl);
        /* draw glyphs at the pen, then advance by the shaped width; trailing
           whitespace still advances (kerning/ink does not include it) */
        float adv = 0;
        for (int g = 0; g < n; g++) adv += gl[g].x_adv;
        draw_run(c, ff, size, col, pen, y_baseline, gl, n, runs[i].rtl);
        pen += adv;
    }
    return pen - x;
}

/* ---------------------------- wrapping ---------------------------- */
static size_t utf8_next(const char *s, size_t len, size_t i) {
    osa_rune r; int n = osa_utf8_decode(s + i, len - i, &r);
    if (n <= 0) n = 1;
    if (i + (size_t)n > len) n = (int)(len - i);
    return i + (size_t)n;
}

static size_t utf8_prev(const char *s, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

int text_wrap_lines(const osa_font *f, float size, const char *utf8, float max_w,
                    char *out, size_t cap, int max_lines) {
    if (!utf8 || !cap) return 0;
    size_t len = strlen(utf8);
    size_t pos = 0, o = 0;
    int line = 0;
    if (max_w <= 0) max_w = 1e9f;
    out[0] = 0;
    char tmp[8192];
    while (pos < len && (max_lines <= 0 || line < max_lines)) {
        while (pos < len && (utf8[pos] == ' ' || utf8[pos] == '\r')) pos++;
        if (pos >= len) break;
        size_t start = pos;
        size_t ln_end = pos;
        while (ln_end < len && utf8[ln_end] != '\n') ln_end++;
        size_t take_end = ln_end;
        size_t tl = ln_end - start;
        if (tl > sizeof tmp - 1) tl = sizeof tmp - 1;
        memcpy(tmp, utf8 + start, tl);
        tmp[tl] = 0;
        if (text_measure(f, size, tmp) > max_w) {
            size_t best = start, i = start, last_space = (size_t)-1;
            while (i < ln_end) {
                size_t ni = utf8_next(utf8, ln_end, i);
                size_t l = ni - start;
                if (l > sizeof tmp - 1) break;
                memcpy(tmp, utf8 + start, l);
                tmp[l] = 0;
                if (text_measure(f, size, tmp) > max_w) break;
                if (utf8[i] == ' ') last_space = i;
                best = ni;
                i = ni;
            }
            if (last_space != (size_t)-1 && last_space > start) {
                take_end = last_space;
                pos = last_space + 1;
            } else {
                if (best <= start) best = utf8_next(utf8, ln_end, start);
                take_end = best;
                pos = best;
            }
        } else {
            pos = (ln_end < len) ? ln_end + 1 : len;
        }
        size_t count = take_end - start;
        if (o + count + 2 > cap) break;
        memcpy(out + o, utf8 + start, count);
        o += count;
        out[o++] = 0;
        out[o] = 0;
        line++;
    }
    return line;
}

float text_draw_box(osa_canvas *c, const osa_font *f, float size, osa_color col,
                    osa_rect box, const char *utf8, text_opts o) {
    if (!f || !utf8 || !*utf8) return 0;
    static char lines[8192];
    float lh = o.line_height > 0 ? o.line_height : text_line_height(f, size);
    float lw = o.max_width > 0 ? o.max_width : box.w;
    int n = text_wrap_lines(f, size, utf8, lw, lines, sizeof lines, o.max_lines);
    bool rtl = o.rtl || text_is_rtl(utf8);
    float asc = text_ascent(f, size);
    const char *p = lines;
    float y = box.y + asc;
    for (int i = 0; i < n; i++) {
        const char *line = p;
        size_t len = strlen(p);
        p += len + 1;
        float w = text_measure(f, size, line);
        float x = box.x;
        if (o.align == 1) x = box.x + (box.w - w) * 0.5f;
        else if (o.align == 2 || (o.align == 0 && rtl)) x = box.x + box.w - w;
        if (x < box.x) x = box.x;
        text_draw(c, f, size, col, x, y, line);
        y += lh;
        if (y > box.y + box.h + lh) break;
    }
    return (float)n * lh;
}

float text_box_height(const osa_font *f, float size, osa_rect box, const char *utf8, text_opts o) {
    static char lines[8192];
    float lh = o.line_height > 0 ? o.line_height : text_line_height(f, size);
    float lw = o.max_width > 0 ? o.max_width : box.w;
    int n = text_wrap_lines(f, size, utf8, lw, lines, sizeof lines, o.max_lines);
    return (float)n * lh;
}

void text_ellipsize(const osa_font *f, float size, const char *utf8, float max_w,
                    char *out, size_t cap) {
    if (!utf8) { if (cap) out[0] = 0; return; }
    if (text_measure(f, size, utf8) <= max_w) {
        osa_strlcpy(out, utf8, cap);
        return;
    }
    const char *ell = "\xE2\x80\xA6";   /* … */
    size_t len = strlen(utf8);
    size_t best = 0;
    size_t i = 0;
    char tmp[4096];
    while (i < len) {
        i = utf8_next(utf8, len, i);
        size_t l = i;
        if (l + 4 >= sizeof tmp) break;
        memcpy(tmp, utf8, l);
        memcpy(tmp + l, ell, 3);
        tmp[l + 3] = 0;
        if (text_measure(f, size, tmp) > max_w) break;
        best = i;
    }
    if (best > cap - 5) best = cap - 5;
    memcpy(out, utf8, best);
    memcpy(out + best, ell, 3);
    out[best + 3] = 0;
}
