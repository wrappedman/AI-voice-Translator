/* canvas.h — software raster canvas. One renderer for Windows and for
 * headless PNG snapshots, so the shipped UI is exactly what CI verifies. */
#ifndef OSAI_CANVAS_H
#define OSAI_CANVAS_H

#include "osai_common.h"
#include "../core/osai_common.h"

typedef struct osa_color { uint8_t r, g, b, a; } osa_color;
typedef struct osa_rect  { float x, y, w, h; } osa_rect;
typedef struct osa_point { float x, y; } osa_point;

static inline osa_color rgba(int r, int g, int b, int a) {
    osa_color c = { (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a };
    return c;
}
static inline osa_color hexc(uint32_t v) {
    return rgba((v >> 16) & 255, (v >> 8) & 255, v & 255, (v >> 24) ? (v >> 24) : 255);
}
static inline osa_color col_alpha(osa_color c, float a) {
    c.a = (uint8_t)(c.a * osa_clampf(a, 0, 1));
    return c;
}
static inline osa_color col_mix(osa_color a, osa_color b, float t) {
    t = osa_clampf(t, 0, 1);
    osa_color c = {
        (uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
        (uint8_t)(a.b + (b.b - a.b) * t), (uint8_t)(a.a + (b.a - a.a) * t) };
    return c;
}
static inline osa_color col_darken(osa_color c, float f) { return col_mix(c, rgba(0,0,0,255), f); }
static inline osa_color col_lighten(osa_color c, float f) { return col_mix(c, rgba(255,255,255,255), f); }
static inline osa_rect  rectf(float x, float y, float w, float h) {
    osa_rect r = { x, y, w, h }; return r;
}
static inline osa_rect rect_inset(osa_rect r, float d) {
    osa_rect o = { r.x + d, r.y + d, r.w - d * 2, r.h - d * 2 }; return o;
}
static inline bool rect_has(osa_rect r, float x, float y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
static inline osa_rect rect_split_left(osa_rect *r, float w) {
    osa_rect o = { r->x, r->y, w, r->h };
    r->x += w; r->w -= w;
    return o;
}
static inline osa_rect rect_split_right(osa_rect *r, float w) { 
    osa_rect o = { r->x + r->w - w, r->y, w, r->h };
    r->w -= w;
    return o;
}
static inline osa_rect rect_split_top(osa_rect *r, float h) {
    osa_rect o = { r->x, r->y, r->w, h };
    r->y += h; r->h -= h;
    return o;
}

/* ------------------------------ canvas ------------------------------ */
#define OSA_CANVAS_CLIP_MAX 16

typedef struct osa_clip_rect { int x0, y0, x1, y1; } osa_clip_rect;

typedef struct osa_canvas {
    int w, h, stride;
    uint32_t *px;             /* 0xAARRGGBB, premultiplied */
    int clip_x0, clip_y0, clip_x1, clip_y1;
    osa_clip_rect clip_stack[OSA_CANVAS_CLIP_MAX];
    int clip_depth;
    int dc_x0, dc_y0, dc_x1, dc_y1;   /* dirty region */
    float scale;
    /* Seconds since the previous frame. The shell animates from this instead of
     * the frame counter, so a 60 Hz repaint and a 20 Hz repaint look the same
     * and a still (frame_time == 0) is reproducible. */
    float frame_time;
    bool  rtl;                /* drawing order: true while the UI is RTL */
} osa_canvas;

bool   canvas_init(osa_canvas *c, int w, int h);
void   canvas_free(osa_canvas *c);
void   canvas_resize(osa_canvas *c, int w, int h);
void   canvas_clear(osa_canvas *c, osa_color col);
void   canvas_clip_push(osa_canvas *c, osa_rect r);
void   canvas_clip_pop(osa_canvas *c);
void   canvas_clip_reset(osa_canvas *c);
osa_rect canvas_clip_get(osa_canvas *c);
void   canvas_reset_dirty(osa_canvas *c);
void   canvas_mark_all_dirty(osa_canvas *c);

void   c_fill(osa_canvas *c, osa_rect r, osa_color col);
void   c_fill_round(osa_canvas *c, osa_rect r, float rad, osa_color col);
void   c_fill_round_corners(osa_canvas *c, osa_rect r, float tl, float tr, float br, float bl, osa_color col);
void   c_stroke_round(osa_canvas *c, osa_rect r, float rad, float width, osa_color col);
void   c_hline(osa_canvas *c, float x0, float x1, float y, float width, osa_color col);
void   c_vline(osa_canvas *c, float x, float y0, float y1, float width, osa_color col);
void   c_line(osa_canvas *c, float x0, float y0, float x1, float y1, float width, osa_color col);
void   c_circle(osa_canvas *c, float cx, float cy, float r, osa_color col);
void   c_circle_stroke(osa_canvas *c, float cx, float cy, float r, float width, osa_color col);
void   c_arc(osa_canvas *c, float cx, float cy, float r, float width, float a0, float a1, osa_color col);
void   c_gradient_v(osa_canvas *c, osa_rect r, osa_color top, osa_color bottom);
void   c_gradient_h(osa_canvas *c, osa_rect r, osa_color left, osa_color right);
void   c_shadow(osa_canvas *c, osa_rect r, float rad, float blur, osa_color col);
/* Approximate a Gaussian drop shadow below a rounded rect (blur in px). The
 * falloff is computed per pixel, not by stacking offset copies. */
void   c_soft_shadow(osa_canvas *c, osa_rect r, float rad, float spread,
                     float dy, osa_color col);
void   c_blend_px(osa_canvas *c, int x, int y, osa_color col);
void   c_blend_sprite(osa_canvas *c, int x, int y, int w, int h,
                      const uint8_t *alpha, int stride, osa_color col);

/* PNG output (stb_image_write) */
bool   canvas_save_png(const osa_canvas *c, const char *path);
const uint8_t *canvas_pixels_bgra(const osa_canvas *c);  /* for BitBlt */

/* geometry helpers */
void osa_round_rect_path(osa_point *pts, int *n, osa_rect r, float rad, int seg_per_corner);

#endif
