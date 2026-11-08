#include "canvas.h"
#include <math.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO 1
#include <stb/stb_image_write.h>
#include "../core/osai_platform.h"

bool canvas_init(osa_canvas *c, int w, int h) {
    memset(c, 0, sizeof *c);
    c->w = w; c->h = h; c->stride = w * 4; c->scale = 1.0f;
    c->px = (uint32_t *)calloc((size_t)w * (size_t)h, 4);
    if (!c->px) return false;
    c->clip_x0 = 0; c->clip_y0 = 0; c->clip_x1 = w; c->clip_y1 = h;
    /* A scissor rectangle in *this* canvas, used to keep widgets from drawing
     * outside their card (see canvas_clip_push). */
    c->rtl = false;
    c->frame_time = 0.0f;
    canvas_mark_all_dirty(c);
    return true;
}

void canvas_free(osa_canvas *c) { free(c->px); memset(c, 0, sizeof *c); }

void canvas_resize(osa_canvas *c, int w, int h) {
    if (w == c->w && h == c->h) return;
    free(c->px);
    c->w = w; c->h = h; c->stride = w * 4;
    c->px = (uint32_t *)calloc((size_t)w * (size_t)h, 4);
    c->clip_x0 = 0; c->clip_y0 = 0; c->clip_x1 = w; c->clip_y1 = h;
    canvas_mark_all_dirty(c);
}

void canvas_mark_all_dirty(osa_canvas *c) {
    c->dc_x0 = 0; c->dc_y0 = 0; c->dc_x1 = c->w; c->dc_y1 = c->h;
}
void canvas_reset_dirty(osa_canvas *c) {
    c->dc_x0 = c->w; c->dc_y0 = c->h; c->dc_x1 = 0; c->dc_y1 = 0;
}
static void mark_dirty(osa_canvas *c, int x0, int y0, int x1, int y1) {
    if (x0 < c->dc_x0) c->dc_x0 = x0;
    if (y0 < c->dc_y0) c->dc_y0 = y0;
    if (x1 > c->dc_x1) c->dc_x1 = x1;
    if (y1 > c->dc_y1) c->dc_y1 = y1;
}

/* The clip stack belongs to the canvas, not to the process: two canvases (the
 * Windows surface and, say, an offscreen preview) used to share a global stack
 * whose depth was never reset, so a single unbalanced push left every later
 * frame drawn through a stale rectangle — in the RTL layout that reads as the
 * whole content area going blank. */
void canvas_clip_push(osa_canvas *c, osa_rect r) {
    if (c->clip_depth >= OSA_CANVAS_CLIP_MAX) return;
    c->clip_stack[c->clip_depth].x0 = c->clip_x0;
    c->clip_stack[c->clip_depth].y0 = c->clip_y0;
    c->clip_stack[c->clip_depth].x1 = c->clip_x1;
    c->clip_stack[c->clip_depth].y1 = c->clip_y1;
    c->clip_depth++;
    int x0 = (int)floorf(r.x), y0 = (int)floorf(r.y);
    int x1 = (int)ceilf(r.x + r.w), y1 = (int)ceilf(r.y + r.h);
    if (x0 > c->clip_x0) c->clip_x0 = x0;
    if (y0 > c->clip_y0) c->clip_y0 = y0;
    if (x1 < c->clip_x1) c->clip_x1 = x1;
    if (y1 < c->clip_y1) c->clip_y1 = y1;
    if (c->clip_x0 < 0) c->clip_x0 = 0;
    if (c->clip_y0 < 0) c->clip_y0 = 0;
    if (c->clip_x1 > c->w) c->clip_x1 = c->w;
    if (c->clip_y1 > c->h) c->clip_y1 = c->h;
    if (c->clip_x1 < c->clip_x0) c->clip_x1 = c->clip_x0;
    if (c->clip_y1 < c->clip_y0) c->clip_y1 = c->clip_y0;
}

void canvas_clip_pop(osa_canvas *c) {
    if (c->clip_depth <= 0) return;
    c->clip_depth--;
    c->clip_x0 = c->clip_stack[c->clip_depth].x0;
    c->clip_y0 = c->clip_stack[c->clip_depth].y0;
    c->clip_x1 = c->clip_stack[c->clip_depth].x1;
    c->clip_y1 = c->clip_stack[c->clip_depth].y1;
}

void canvas_clip_reset(osa_canvas *c) {
    c->clip_depth = 0;
    c->clip_x0 = 0; c->clip_y0 = 0;
    c->clip_x1 = c->w; c->clip_y1 = c->h;
}

osa_rect canvas_clip_get(osa_canvas *c) {
    return rectf((float)c->clip_x0, (float)c->clip_y0,
                 (float)(c->clip_x1 - c->clip_x0), (float)(c->clip_y1 - c->clip_y0));
}

void canvas_clear(osa_canvas *c, osa_color col) {
    uint32_t v = ((uint32_t)col.a << 24) | ((uint32_t)(col.r * col.a / 255) << 16) |
                 ((uint32_t)(col.g * col.a / 255) << 8) | (uint32_t)(col.b * col.a / 255);
    for (int i = 0; i < c->w * c->h; i++) c->px[i] = v;
    mark_dirty(c, 0, 0, c->w, c->h);
}

void c_blend_px(osa_canvas *c, int x, int y, osa_color col) {
    if (x < c->clip_x0 || y < c->clip_y0 || x >= c->clip_x1 || y >= c->clip_y1) return;
    if (col.a == 0) return;
    if (col.a == 255) {
        c->px[y * c->w + x] = 0xFF000000u | ((uint32_t)col.r << 16) | ((uint32_t)col.g << 8) | col.b;
        mark_dirty(c, x, y, x + 1, y + 1);
        return;
    }
    uint32_t dst = c->px[y * c->w + x];
    int da = (int)(dst >> 24), dr = (int)((dst >> 16) & 255);
    int dg = (int)((dst >> 8) & 255), db = (int)(dst & 255);
    int sa = col.a, ia = 255 - sa;
    int oa = sa + da * ia / 255;
    int orr = col.r * sa / 255 + dr * ia / 255;
    int og  = col.g * sa / 255 + dg * ia / 255;
    int ob  = col.b * sa / 255 + db * ia / 255;
    c->px[y * c->w + x] = ((uint32_t)oa << 24) | ((uint32_t)(orr > 255 ? 255 : orr) << 16) |
                          ((uint32_t)(og > 255 ? 255 : og) << 8) | (uint32_t)(ob > 255 ? 255 : ob);
    mark_dirty(c, x, y, x + 1, y + 1);
}

void c_fill(osa_canvas *c, osa_rect r, osa_color col) {
    if (col.a == 0 || r.w <= 0 || r.h <= 0) return;
    int x0 = (int)floorf(r.x), y0 = (int)floorf(r.y);
    int x1 = (int)ceilf(r.x + r.w), y1 = (int)ceilf(r.y + r.h);
    if (x0 < c->clip_x0) x0 = c->clip_x0;
    if (y0 < c->clip_y0) y0 = c->clip_y0;
    if (x1 > c->clip_x1) x1 = c->clip_x1;
    if (y1 > c->clip_y1) y1 = c->clip_y1;
    if (x1 <= x0 || y1 <= y0) return;
    if (col.a == 255) {
        uint32_t v = 0xFF000000u | ((uint32_t)col.r << 16) | ((uint32_t)col.g << 8) | col.b;
        for (int y = y0; y < y1; y++) {
            uint32_t *row = c->px + (size_t)y * c->w;
            for (int x = x0; x < x1; x++) row[x] = v;
        }
        mark_dirty(c, x0, y0, x1, y1);
        return;
    }
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) c_blend_px(c, x, y, col);
}

/* ------------------------- polygon rasterization ------------------------- */
#define POLY_MAX 1024
typedef struct { float xs[POLY_MAX]; int n; } xs_list;

/* even-odd fill of up to two polygons (second acts as a hole) with 16x vertical
 * supersampling and analytic horizontal coverage => smooth AA edges. */
static void fill_poly2(osa_canvas *c, const osa_point *pts, int n,
                       const osa_point *hole, int hn, osa_color col) {
    if (col.a == 0 || n < 3) return;
    float miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; i++) {
        if (pts[i].y < miny) miny = pts[i].y;
        if (pts[i].y > maxy) maxy = pts[i].y;
    }
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (y0 < c->clip_y0) y0 = c->clip_y0;
    if (y1 > c->clip_y1) y1 = c->clip_y1;
    if (y1 <= y0) return;
    enum { SUB = 4 };
    static float acc[8192];
    int acc_cap = (int)(sizeof acc / sizeof acc[0]);

    for (int y = y0; y < y1; y++) {
        int row_x0 = c->clip_x0, row_x1 = c->clip_x1;
        if (row_x1 - row_x0 > acc_cap) row_x1 = row_x0 + acc_cap;
        memset(acc, 0, (size_t)(row_x1 - row_x0) * sizeof(float));
        bool any = false;
        for (int s = 0; s < SUB; s++) {
            float sy = (float)y + ((float)s + 0.5f) / (float)SUB;
            float xs[POLY_MAX * 2];
            int cnt = 0;
            for (int pass = 0; pass < 2; pass++) {
                const osa_point *P = pass ? hole : pts;
                int N = pass ? hn : n;
                if (pass && hn < 3) continue;
                for (int i = 0; i < N && cnt < POLY_MAX * 2; i++) {
                    const osa_point *a = &P[i];
                    const osa_point *b = &P[(i + 1) % N];
                    if ((a->y <= sy && b->y > sy) || (b->y <= sy && a->y > sy)) {
                        float t = (sy - a->y) / (b->y - a->y);
                        xs[cnt++] = a->x + t * (b->x - a->x);
                    }
                }
            }
            for (int i = 1; i < cnt; i++) {
                float v = xs[i]; int j = i - 1;
                while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
                xs[j + 1] = v;
            }
            for (int i = 0; i + 1 < cnt; i += 2) {
                float xa = xs[i], xb = xs[i + 1];
                int ix0 = (int)floorf(xa), ix1 = (int)ceilf(xb);
                if (ix0 < row_x0) ix0 = row_x0;
                if (ix1 > row_x1) ix1 = row_x1;
                for (int x = ix0; x < ix1; x++) {
                    float l = xa > (float)x ? xa : (float)x;
                    float r = xb < (float)x + 1.0f ? xb : (float)x + 1.0f;
                    if (r > l) { acc[x - row_x0] += (r - l) / (float)SUB; any = true; }
                }
            }
        }
        if (!any) continue;
        for (int x = row_x0; x < row_x1; x++) {
            float a = acc[x - row_x0];
            if (a <= 0.002f) continue;
            if (a > 1) a = 1;
            osa_color cc = col;
            cc.a = (uint8_t)((float)col.a * a);
            c_blend_px(c, x, y, cc);
        }
    }
}

static void fill_poly(osa_canvas *c, const osa_point *pts, int n, osa_color col) {
    fill_poly2(c, pts, n, NULL, 0, col);
}

void osa_round_rect_path(osa_point *pts, int *n, osa_rect r, float rad, int seg) {
    float maxr = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (rad > maxr) rad = maxr;
    if (rad < 0.5f) rad = 0;
    if (rad <= 0.0f) {
        pts[0] = (osa_point){ r.x, r.y };
        pts[1] = (osa_point){ r.x + r.w, r.y };
        pts[2] = (osa_point){ r.x + r.w, r.y + r.h };
        pts[3] = (osa_point){ r.x, r.y + r.h };
        *n = 4;
        return;
    }
    int k = 0;
    const float cx[4] = { r.x + r.w - rad, r.x + r.w - rad, r.x + rad, r.x + rad };
    const float cy[4] = { r.y + rad, r.y + r.h - rad, r.y + r.h - rad, r.y + rad };
    const float a0[4] = { -1.5707963f, 0.0f, 1.5707963f, 3.14159265f };
    for (int ci = 0; ci < 4; ci++)
        for (int i = 0; i <= seg; i++) {
            float a = a0[ci] + 1.5707963f * (float)i / (float)seg;
            pts[k++] = (osa_point){ cx[ci] + cosf(a) * rad, cy[ci] + sinf(a) * rad };
        }
    *n = k;
}

void c_fill_round(osa_canvas *c, osa_rect r, float rad, osa_color col) {
    if (col.a == 0 || r.w <= 0 || r.h <= 0) return;
    if (rad <= 0.5f) { c_fill(c, r, col); return; }
    osa_point pts[300];
    int n = 0;
    osa_round_rect_path(pts, &n, r, rad, 5);
    fill_poly(c, pts, n, col);
}

void c_fill_round_corners(osa_canvas *c, osa_rect r, float tl, float tr, float br, float bl, osa_color col) {
    if (col.a == 0 || r.w <= 0 || r.h <= 0) return;
    float m = (r.w < r.h ? r.w : r.h) * 0.5f;
    tl = osa_clampf(tl, 0, m); tr = osa_clampf(tr, 0, m);
    br = osa_clampf(br, 0, m); bl = osa_clampf(bl, 0, m);
    if (tl == tr && tr == br && br == bl) { c_fill_round(c, r, tl, col); return; }
    /* build a single path with per-corner radii */
    osa_point pts[300]; int k = 0;
    const float rad[4] = { tl, tr, br, bl };
    const float cx[4] = { r.x + tl, r.x + r.w - tr, r.x + r.w - br, r.x + bl };
    const float cy[4] = { r.y + tl, r.y + tr, r.y + r.h - br, r.y + r.h - bl };
    const float a0[4] = { 3.14159265f, 4.71238898f, 0.0f, 1.5707963f };
    for (int ci = 0; ci < 4; ci++) {
        if (rad[ci] <= 0.5f) {
            switch (ci) {
                case 0: pts[k++] = (osa_point){ r.x, r.y }; break;
                case 1: pts[k++] = (osa_point){ r.x + r.w, r.y }; break;
                case 2: pts[k++] = (osa_point){ r.x + r.w, r.y + r.h }; break;
                default: pts[k++] = (osa_point){ r.x, r.y + r.h }; break;
            }
            continue;
        }
        for (int i = 0; i <= 5; i++) {
            float a = a0[ci] + 1.5707963f * (float)i / 5.0f;
            pts[k++] = (osa_point){ cx[ci] + cosf(a) * rad[ci], cy[ci] + sinf(a) * rad[ci] };
        }
    }
    fill_poly(c, pts, k, col);
}

void c_stroke_round(osa_canvas *c, osa_rect r, float rad, float width, osa_color col) {
    if (width <= 0 || col.a == 0 || r.w <= 0 || r.h <= 0) return;
    osa_point outer[300]; int no = 0;
    osa_round_rect_path(outer, &no, r, rad, 5);
    osa_rect in = rect_inset(r, width);
    if (in.w <= 0 || in.h <= 0) { fill_poly(c, outer, no, col); return; }
    osa_point inner[300]; int ni = 0;
    float irad = rad - width; if (irad < 0) irad = 0;
    osa_round_rect_path(inner, &ni, in, irad, 5);
    fill_poly2(c, outer, no, inner, ni, col);
}

void c_hline(osa_canvas *c, float x0, float x1, float y, float width, osa_color col) {
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; }
    c_fill(c, rectf(x0, y - width * 0.5f, x1 - x0, width), col);
}
void c_vline(osa_canvas *c, float x, float y0, float y1, float width, osa_color col) {
    if (y1 < y0) { float t = y0; y0 = y1; y1 = t; }
    c_fill(c, rectf(x - width * 0.5f, y0, width, y1 - y0), col);
}

void c_line(osa_canvas *c, float x0, float y0, float x1, float y1, float width, osa_color col) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) { c_circle(c, x0, y0, width * 0.5f, col); return; }
    float nx = -dy / len * width * 0.5f, ny = dx / len * width * 0.5f;
    osa_point p[4] = {
        { x0 + nx, y0 + ny }, { x1 + nx, y1 + ny },
        { x1 - nx, y1 - ny }, { x0 - nx, y0 - ny } };
    fill_poly(c, p, 4, col);
    c_circle(c, x0, y0, width * 0.5f, col);
    c_circle(c, x1, y1, width * 0.5f, col);
}

void c_circle(osa_canvas *c, float cx, float cy, float r, osa_color col) {
    if (r <= 0 || col.a == 0) return;
    int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    int x0 = (int)floorf(cx - r), x1 = (int)ceilf(cx + r);
    if (x0 < c->clip_x0) x0 = c->clip_x0;
    if (y0 < c->clip_y0) y0 = c->clip_y0;
    if (x1 > c->clip_x1) x1 = c->clip_x1;
    if (y1 > c->clip_y1) y1 = c->clip_y1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float cov = osa_clampf(r - sqrtf(dx * dx + dy * dy) + 0.5f, 0, 1);
            if (cov <= 0) continue;
            osa_color cc = col;
            cc.a = (uint8_t)((float)col.a * cov);
            c_blend_px(c, x, y, cc);
        }
}

void c_circle_stroke(osa_canvas *c, float cx, float cy, float r, float width, osa_color col) {
    float r1 = r + width * 0.5f;
    int y0 = (int)floorf(cy - r1), y1 = (int)ceilf(cy + r1);
    int x0 = (int)floorf(cx - r1), x1 = (int)ceilf(cx + r1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float cov = osa_clampf(width * 0.5f - fabsf(sqrtf(dx * dx + dy * dy) - r) + 0.5f, 0, 1);
            if (cov <= 0) continue;
            osa_color cc = col;
            cc.a = (uint8_t)((float)col.a * cov);
            c_blend_px(c, x, y, cc);
        }
}

void c_arc(osa_canvas *c, float cx, float cy, float r, float width, float a0, float a1, osa_color col) {
    int steps = (int)(fabsf(a1 - a0) * r * 0.5f) + 6;
    if (steps > 512) steps = 512;
    float prevx = cx + cosf(a0) * r, prevy = cy + sinf(a0) * r;
    for (int i = 1; i <= steps; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)steps;
        float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
        c_line(c, prevx, prevy, x, y, width, col);
        prevx = x; prevy = y;
    }
}

void c_gradient_v(osa_canvas *c, osa_rect r, osa_color top, osa_color bottom) {
    int y0 = (int)floorf(r.y), y1 = (int)ceilf(r.y + r.h);
    if (y0 < c->clip_y0) y0 = c->clip_y0;
    if (y1 > c->clip_y1) y1 = c->clip_y1;
    for (int y = y0; y < y1; y++) {
        float t = ((float)y - r.y) / (r.h > 0 ? r.h : 1);
        c_fill(c, rectf(r.x, (float)y, r.w, 1), col_mix(top, bottom, t));
    }
}

void c_gradient_h(osa_canvas *c, osa_rect r, osa_color left, osa_color right) {
    int x0 = (int)floorf(r.x), x1 = (int)ceilf(r.x + r.w);
    if (x0 < c->clip_x0) x0 = c->clip_x0;
    if (x1 > c->clip_x1) x1 = c->clip_x1;
    for (int x = x0; x < x1; x++) {
        float t = ((float)x - r.x) / (r.w > 0 ? r.w : 1);
        c_fill(c, rectf((float)x, r.y, 1, r.h), col_mix(left, right, t));
    }
}

void c_shadow(osa_canvas *c, osa_rect r, float rad, float blur, osa_color col) {
    for (int i = 6; i >= 1; i--) {
        float e = blur * (float)i / 6.0f;
        osa_color cc = col;
        cc.a = (uint8_t)((float)col.a / (float)(i * 2));
        c_fill_round(c, rectf(r.x - e, r.y - e * 0.5f, r.w + e * 2, r.h + e), rad + e, cc);
    }
}

/* Signed distance to a rounded rectangle, negative inside. Used for the soft
 * shadow below, so the falloff is smooth instead of a stack of hard rings. */
static float sd_round_rect(float px, float py, osa_rect r, float rad) {
    float hx = r.w * 0.5f, hy = r.h * 0.5f;
    float qx = fabsf(px - (r.x + hx)) - (hx - rad);
    float qy = fabsf(py - (r.y + hy)) - (hy - rad);
    float ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
    float outside = sqrtf(ax * ax + ay * ay);
    float inside = (qx > qy ? qx : qy);
    if (inside > 0) inside = 0;
    return outside + inside - rad;
}

void c_soft_shadow(osa_canvas *c, osa_rect r, float rad, float spread, float dy,
                   osa_color col) {
    if (col.a == 0) return;
    float reach = spread * 1.5f;
    int x0 = (int)floorf(r.x - reach), x1 = (int)ceilf(r.x + r.w + reach);
    int y0 = (int)floorf(r.y - reach + dy), y1 = (int)ceilf(r.y + r.h + reach + dy);
    if (x0 < c->clip_x0) x0 = c->clip_x0;
    if (y0 < c->clip_y0) y0 = c->clip_y0;
    if (x1 > c->clip_x1) x1 = c->clip_x1;
    if (y1 > c->clip_y1) y1 = c->clip_y1;
    if (x1 <= x0 || y1 <= y0) return;
    osa_rect body = rectf(r.x, r.y + dy, r.w, r.h);
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float d = sd_round_rect((float)x + 0.5f, (float)y + 0.5f, body, rad);
            if (d > reach) continue;
            float t = 1.0f - (d < 0 ? 0.0f : d / reach);
            t = t * t * (3.0f - 2.0f * t);           /* smoothstep */
            osa_color cc = col;
            cc.a = (uint8_t)((float)col.a * t);
            if (cc.a) c_blend_px(c, x, y, cc);
        }
    }
}

void c_blend_sprite(osa_canvas *c, int x, int y, int w, int h,
                    const uint8_t *alpha, int stride, osa_color col) {
    for (int yy = 0; yy < h; yy++) {
        const uint8_t *src = alpha + (size_t)yy * stride;
        for (int xx = 0; xx < w; xx++) {
            if (!src[xx]) continue;
            osa_color cc = col;
            cc.a = (uint8_t)((uint32_t)col.a * src[xx] / 255);
            c_blend_px(c, x + xx, y + yy, cc);
        }
    }
}

/* ------------------------------ PNG ------------------------------ */
static void stbi_write_cb(void *ctx, void *data, int size) {
    osa_buf *b = (osa_buf *)ctx;
    osa_buf_putn(b, data, (size_t)size);
}

bool canvas_save_png(const osa_canvas *c, const char *path) {
    size_t n = (size_t)c->w * c->h * 4;
    uint8_t *rgba = (uint8_t *)malloc(n);
    if (!rgba) return false;
    for (int i = 0; i < c->w * c->h; i++) {
        uint32_t p = c->px[i];
        int a = (int)(p >> 24);
        if (a == 0) { memset(rgba + (size_t)i * 4, 0, 4); continue; }
        int r = (int)((p >> 16) & 255) * 255 / a;
        int g = (int)((p >> 8) & 255) * 255 / a;
        int b = (int)(p & 255) * 255 / a;
        rgba[i*4+0] = (uint8_t)(r > 255 ? 255 : r);
        rgba[i*4+1] = (uint8_t)(g > 255 ? 255 : g);
        rgba[i*4+2] = (uint8_t)(b > 255 ? 255 : b);
        rgba[i*4+3] = (uint8_t)a;
    }
    osa_buf png; osa_buf_init(&png);
    int ok = stbi_write_png_to_func(stbi_write_cb, &png, c->w, c->h, 4, rgba, c->w * 4);
    free(rgba);
    bool wrote = ok && osa_file_write(path, png.data, png.len);
    osa_buf_free(&png);
    return wrote;
}

const uint8_t *canvas_pixels_bgra(const osa_canvas *c) {
    return (const uint8_t *)c->px;   /* ARGB32 premultiplied == BGRA bytes on LE */
}
