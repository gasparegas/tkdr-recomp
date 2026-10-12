// Software rasterizer for the PSP GE.
//
// Pipeline: decode vertices -> (skin, morph) -> transform or "through" mode ->
// near-plane clip -> rasterize (triangles, rectangles, lines, points) ->
// texture + texture function -> alpha test -> depth test -> blend -> masks.
// Not yet: lighting, fog, stencil, logic ops, dithering, splines.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "gpu.h"

#define R(reg_) (g_ge.cmd[reg_])

static uint32_t g_prims;
uint32_t g_flush_reason[5];  // stats: list end, texture, transfer, targets full, state wrap
uint32_t gpu_stat_prims(void) {
    uint32_t n = g_prims;
    g_prims = 0;
    return n;
}

// ---------------------------------------------------------------- vertices

typedef struct {
    float x, y, z, w;  // clip space (transform) or screen (through)
    float u, v;
    float c[4];  // r g b a, 0..255
} Vtx;

static const uint8_t k_tcsize[4] = {0, 2, 4, 8}, k_tcalign[4] = {0, 1, 2, 4};
static const uint8_t k_colsize[8] = {0, 0, 0, 0, 2, 2, 2, 4}, k_colalign[8] = {0, 0, 0, 0, 2, 2, 2, 4};
static const uint8_t k_nrmsize[4] = {0, 3, 6, 12}, k_nrmalign[4] = {0, 1, 2, 4};
static const uint8_t k_possize[4] = {3, 3, 6, 12}, k_posalign[4] = {1, 1, 2, 4};
static const uint8_t k_wtsize[4] = {0, 1, 2, 4}, k_wtalign[4] = {0, 1, 2, 4};

typedef struct {
    uint32_t vt;
    int tc, col, nrm, pos, wt, nw, nmorph, idx, through;
    int off_wt, off_tc, off_col, off_nrm, off_pos, size;
} VFormat;

static uint32_t align_up(uint32_t v, uint32_t a) { return a > 1 ? (v + a - 1) & ~(a - 1) : v; }

static void vformat(VFormat *f, uint32_t vt) {
    f->vt = vt;
    f->tc = vt & 3;
    f->col = (vt >> 2) & 7;
    f->nrm = (vt >> 5) & 3;
    f->pos = (vt >> 7) & 3;
    f->wt = (vt >> 9) & 3;
    f->idx = (vt >> 11) & 3;
    f->nw = ((vt >> 14) & 7) + 1;
    f->nmorph = ((vt >> 18) & 7) + 1;
    f->through = (vt >> 23) & 1;
    uint32_t s = 0, big = 1;
    if (f->wt) {
        f->off_wt = (int)s;
        s += k_wtsize[f->wt] * (uint32_t)f->nw;
        if (k_wtalign[f->wt] > big) big = k_wtalign[f->wt];
    }
    if (f->tc) {
        s = align_up(s, k_tcalign[f->tc]);
        f->off_tc = (int)s;
        s += k_tcsize[f->tc];
        if (k_tcalign[f->tc] > big) big = k_tcalign[f->tc];
    }
    if (f->col) {
        s = align_up(s, k_colalign[f->col]);
        f->off_col = (int)s;
        s += k_colsize[f->col];
        if (k_colalign[f->col] > big) big = k_colalign[f->col];
    }
    if (f->nrm) {
        s = align_up(s, k_nrmalign[f->nrm]);
        f->off_nrm = (int)s;
        s += k_nrmsize[f->nrm];
        if (k_nrmalign[f->nrm] > big) big = k_nrmalign[f->nrm];
    }
    s = align_up(s, k_posalign[f->pos]);
    f->off_pos = (int)s;
    s += k_possize[f->pos];
    if (k_posalign[f->pos] > big) big = k_posalign[f->pos];
    f->size = (int)align_up(s, big);
}

static float rd_comp(uint32_t a, int kind, int i, int through, int is_signed) {
    switch (kind) {
    case 1: {
        uint8_t b = mem_r8(a + (uint32_t)i);
        if (through) return is_signed ? (float)(int8_t)b : (float)b;
        return is_signed ? (float)(int8_t)b / 128.f : (float)b / 128.f;
    }
    case 2: {
        uint16_t h = mem_r16(a + (uint32_t)i * 2);
        if (through) return is_signed ? (float)(int16_t)h : (float)h;
        return is_signed ? (float)(int16_t)h / 32768.f : (float)h / 32768.f;
    }
    case 3: {
        uint32_t u = mem_r32(a + (uint32_t)i * 4);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    }
    return 0.f;
}

static void decode_color(uint32_t a, int fmt, float *c) {
    if (fmt == 7) {
        uint32_t v = mem_r32(a);
        c[0] = (float)(v & 0xFF); c[1] = (float)(v >> 8 & 0xFF); c[2] = (float)(v >> 16 & 0xFF); c[3] = (float)(v >> 24);
        return;
    }
    uint32_t v = mem_r16(a);
    if (fmt == 4) {  // 565
        c[0] = (float)((v & 31) * 255 / 31); c[1] = (float)((v >> 5 & 63) * 255 / 63);
        c[2] = (float)((v >> 11 & 31) * 255 / 31); c[3] = 255.f;
    } else if (fmt == 5) {  // 5551
        c[0] = (float)((v & 31) * 255 / 31); c[1] = (float)((v >> 5 & 31) * 255 / 31);
        c[2] = (float)((v >> 10 & 31) * 255 / 31); c[3] = (v >> 15) ? 255.f : 0.f;
    } else {  // 4444
        c[0] = (float)((v & 15) * 17); c[1] = (float)((v >> 4 & 15) * 17);
        c[2] = (float)((v >> 8 & 15) * 17); c[3] = (float)((v >> 12 & 15) * 17);
    }
}

// 4x3 matrix (column-major, 4 columns of xyz) times point
static void mul43(const float *m, const float *p, float *o) {
    for (int i = 0; i < 3; i++) o[i] = m[i] * p[0] + m[3 + i] * p[1] + m[6 + i] * p[2] + m[9 + i];
}

static void material_color(float *c) {
    uint32_t amb = R(GE_MATERIALAMBIENT);
    c[0] = (float)(amb & 0xFF); c[1] = (float)(amb >> 8 & 0xFF); c[2] = (float)(amb >> 16 & 0xFF);
    c[3] = (float)(R(GE_MATERIALALPHA) & 0xFF);
}

// ---------------------------------------------------------------- lighting

static void rgb24(uint32_t v, float *c) {
    c[0] = (float)(v & 0xFF); c[1] = (float)(v >> 8 & 0xFF); c[2] = (float)(v >> 16 & 0xFF);
}

// GE vertex lighting, in world space. col: vertex color (in/out, 0..255).
static void light_vertex(const float *wp, float *n, float *col, int has_color) {
    float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0.f) for (int i = 0; i < 3; i++) n[i] /= len;
    if (R(GE_REVERSENORMAL) & 1) for (int i = 0; i < 3; i++) n[i] = -n[i];
    uint32_t mu = has_color ? R(GE_MATERIALUPDATE) & 7 : 0;
    float ma[4], md[3], ms[3], me[3], ga[4];
    if (mu & 1) memcpy(ma, col, sizeof ma);
    else { rgb24(R(GE_MATERIALAMBIENT), ma); ma[3] = (float)(R(GE_MATERIALALPHA) & 0xFF); }
    if (mu & 2) memcpy(md, col, sizeof md); else rgb24(R(GE_MATERIALDIFFUSE), md);
    if (mu & 4) memcpy(ms, col, sizeof ms); else rgb24(R(GE_MATERIALSPECULAR), ms);
    rgb24(R(GE_MATERIALEMISSIVE), me);
    rgb24(R(GE_AMBIENTCOLOR), ga);
    ga[3] = (float)(R(GE_AMBIENTALPHA) & 0xFF);
    float spec_exp = ge_float(R(GE_MATERIALSPECULARCOEF));
    float out[3], spec[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) out[i] = me[i] + ma[i] * ga[i] / 255.f;
    float alpha = ma[3] * ga[3] / 255.f;
    for (int l = 0; l < 4; l++) {
        if (!(R(GE_LIGHTENABLE0 + l) & 1)) continue;
        uint32_t type = R(GE_LIGHTTYPE0 + l), kind = type >> 8 & 3, comp = type & 3;
        float L[3] = {ge_float(R(GE_LX0 + 3 * l)), ge_float(R(GE_LX0 + 3 * l + 1)), ge_float(R(GE_LX0 + 3 * l + 2))};
        float att = 1.f, spot = 1.f;
        if (kind != 0) {  // point/spot: vector from the vertex to the light
            for (int i = 0; i < 3; i++) L[i] -= wp[i];
            float d = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            if (d > 0.f) for (int i = 0; i < 3; i++) L[i] /= d;
            float ka = ge_float(R(GE_LKA0 + 3 * l)), kb = ge_float(R(GE_LKA0 + 3 * l + 1)), kc = ge_float(R(GE_LKA0 + 3 * l + 2));
            float den = ka + kb * d + kc * d * d;
            att = den > 0.f ? fminf(1.f / den, 1.f) : 0.f;
            if (kind == 2) {
                float D[3] = {ge_float(R(GE_LDX0 + 3 * l)), ge_float(R(GE_LDX0 + 3 * l + 1)), ge_float(R(GE_LDX0 + 3 * l + 2))};
                float dl = sqrtf(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
                float sd = dl > 0.f ? -(D[0] * L[0] + D[1] * L[1] + D[2] * L[2]) / dl : 0.f;
                float cutoff = ge_float(R(GE_LKO0 + l)), ex = ge_float(R(GE_LKS0 + l));
                spot = sd >= cutoff ? powf(fmaxf(sd, 0.f), ex) : 0.f;
            }
        } else {
            float d = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            if (d > 0.f) for (int i = 0; i < 3; i++) L[i] /= d;
        }
        float ndotl = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
        float diff = ndotl > 0.f ? ndotl : 0.f;
        if (comp == 2) diff = powf(diff, spec_exp);  // powered diffuse
        float la[3], ld[3], ls[3];
        rgb24(R(GE_LAC0 + 3 * l), la);
        rgb24(R(GE_LAC0 + 3 * l + 1), ld);
        rgb24(R(GE_LAC0 + 3 * l + 2), ls);
        float k = att * spot;
        for (int i = 0; i < 3; i++) out[i] += k * (la[i] * ma[i] + ld[i] * md[i] * diff) / 255.f;
        if (comp == 1 && ndotl > 0.f) {  // specular, viewer along +z
            float H[3] = {L[0], L[1], L[2] + 1.f};
            float hl = sqrtf(H[0] * H[0] + H[1] * H[1] + H[2] * H[2]);
            float ndoth = hl > 0.f ? (n[0] * H[0] + n[1] * H[1] + n[2] * H[2]) / hl : 0.f;
            float sp = ndoth > 0.f ? powf(ndoth, spec_exp) : 0.f;
            for (int i = 0; i < 3; i++) spec[i] += k * ls[i] * ms[i] * sp / 255.f;
        }
    }
    for (int i = 0; i < 3; i++) {
        float v = out[i] + spec[i];
        col[i] = v > 255.f ? 255.f : v;
    }
    col[3] = alpha > 255.f ? 255.f : alpha;
}

static void decode_vertex(const VFormat *f, uint32_t base, Vtx *out) {
    float pos[3] = {0}, tc[2] = {0}, col[4] = {0}, w[8] = {0}, nrm[3] = {0};
    int have_col = f->col != 0;
    // morphing: blend every component of the morph targets
    for (int m = 0; m < f->nmorph; m++) {
        uint32_t a = base + (uint32_t)(m * f->size);
        float k = f->nmorph > 1 ? g_ge.morph[m] : 1.f;
        for (int i = 0; i < 3; i++)
            pos[i] += k * rd_comp(a + (uint32_t)f->off_pos, f->pos, i, f->through, f->pos != 1 || !f->through);
        if (f->tc)
            for (int i = 0; i < 2; i++) tc[i] += k * rd_comp(a + (uint32_t)f->off_tc, f->tc, i, f->through, 0);
        if (have_col) {
            float c[4];
            decode_color(a + (uint32_t)f->off_col, f->col, c);
            for (int i = 0; i < 4; i++) col[i] += k * c[i];
        }
        if (f->wt)
            for (int i = 0; i < f->nw; i++) w[i] += k * rd_comp(a + (uint32_t)f->off_wt, f->wt, i, 0, 0);
        if (f->nrm)
            for (int i = 0; i < 3; i++) nrm[i] += k * rd_comp(a + (uint32_t)f->off_nrm, f->nrm, i, 0, 1);
    }
    if (f->through && f->pos == 2) pos[2] = (float)(uint16_t)(int16_t)pos[2];  // z is unsigned
    if (!have_col) material_color(col);

    if (f->through) {
        out->x = pos[0]; out->y = pos[1]; out->z = pos[2]; out->w = 1.f;
        out->u = tc[0]; out->v = tc[1];
    } else {
        float wp[3], wn[3] = {0, 0, 0}, mn[3] = {nrm[0], nrm[1], nrm[2]};
        int lit = (R(GE_LIGHTINGENABLE) & 1) != 0;
        if (f->wt) {  // skinning: weighted sum of bone transforms
            wp[0] = wp[1] = wp[2] = 0.f;
            float sn[3] = {0, 0, 0};
            for (int i = 0; i < f->nw; i++) {
                float t[3];
                mul43(g_ge.bone[i], pos, t);
                for (int j = 0; j < 3; j++) wp[j] += w[i] * t[j];
                if (lit) {
                    const float *b = g_ge.bone[i];
                    for (int j = 0; j < 3; j++) sn[j] += w[i] * (b[j] * nrm[0] + b[3 + j] * nrm[1] + b[6 + j] * nrm[2]);
                }
            }
            float t[3] = {wp[0], wp[1], wp[2]};
            mul43(g_ge.world, t, wp);
            memcpy(mn, sn, sizeof mn);
        } else {
            mul43(g_ge.world, pos, wp);
        }
        if (lit) {
            const float *m = g_ge.world;
            for (int j = 0; j < 3; j++) wn[j] = m[j] * mn[0] + m[3 + j] * mn[1] + m[6 + j] * mn[2];
            light_vertex(wp, wn, col, have_col);
        }
        float vp[3];
        mul43(g_ge.view, wp, vp);
        const float *p = g_ge.proj;
        out->x = p[0] * vp[0] + p[4] * vp[1] + p[8] * vp[2] + p[12];
        out->y = p[1] * vp[0] + p[5] * vp[1] + p[9] * vp[2] + p[13];
        out->z = p[2] * vp[0] + p[6] * vp[1] + p[10] * vp[2] + p[14];
        out->w = p[3] * vp[0] + p[7] * vp[1] + p[11] * vp[2] + p[15];
        // texture coordinates: scale/offset (texmapmode 0); others not yet
        out->u = tc[0] * ge_float(R(GE_TEXSCALEU)) + ge_float(R(GE_TEXOFFSETU));
        out->v = tc[1] * ge_float(R(GE_TEXSCALEV)) + ge_float(R(GE_TEXOFFSETV));
    }
    memcpy(out->c, col, sizeof col);
}

// ---------------------------------------------------------------- screen mapping

typedef struct {
    float x, y, z;  // screen pixels, depth 0..65535
    float iw;  // 1/w for perspective correction
    float u, v;  // divided by w (transform mode)
    float c[4];  // divided by w
} SVtx;

static void to_screen(const Vtx *v, SVtx *s, int through) {
    if (through) {
        s->x = v->x; s->y = v->y; s->z = v->z; s->iw = 1.f;
        s->u = v->u; s->v = v->v;
        memcpy(s->c, v->c, sizeof s->c);
        return;
    }
    float iw = 1.f / v->w;
    float ox = (float)(R(GE_OFFSETX) & 0xFFFF) / 16.f, oy = (float)(R(GE_OFFSETY) & 0xFFFF) / 16.f;
    s->x = v->x * iw * ge_float(R(GE_VIEWPORTXSCALE)) + ge_float(R(GE_VIEWPORTXCENTER)) - ox;
    s->y = v->y * iw * ge_float(R(GE_VIEWPORTYSCALE)) + ge_float(R(GE_VIEWPORTYCENTER)) - oy;
    s->z = v->z * iw * ge_float(R(GE_VIEWPORTZSCALE)) + ge_float(R(GE_VIEWPORTZCENTER));
    s->iw = iw;
    s->u = v->u * iw;
    s->v = v->v * iw;
    for (int i = 0; i < 4; i++) s->c[i] = v->c[i] * iw;
}

// ---------------------------------------------------------------- framebuffer

typedef struct {
    uint32_t fb, fb_stride, fb_fmt, zb, zb_stride;
    int sx1, sy1, sx2, sy2;  // scissor, inclusive
} Target;

static void target(Target *t) {
    t->fb = 0x04000000u | (R(GE_FRAMEBUFPTR) & 0xFFFFF0u);
    t->fb_stride = R(GE_FRAMEBUFWIDTH) & 0x7FC;
    t->fb_fmt = R(GE_FRAMEBUFPIXFORMAT) & 3;
    t->zb = 0x04000000u | (R(GE_ZBUFPTR) & 0xFFFFF0u);
    t->zb_stride = R(GE_ZBUFWIDTH) & 0x7FC;
    t->sx1 = (int)(R(GE_SCISSOR1) & 0x3FF);
    t->sy1 = (int)(R(GE_SCISSOR1) >> 10 & 0x3FF);
    t->sx2 = (int)(R(GE_SCISSOR2) & 0x3FF);
    t->sy2 = (int)(R(GE_SCISSOR2) >> 10 & 0x3FF);
}

static uint32_t fb_read(const Target *t, int x, int y) {
    uint32_t a = t->fb + (uint32_t)(y * (int)t->fb_stride + x) * (t->fb_fmt == 3 ? 4 : 2);
    if (t->fb_fmt == 3) return mem_r32(a);
    uint32_t v = mem_r16(a), r, g, b, al;
    switch (t->fb_fmt) {
    case 0: r = (v & 31) << 3 | (v & 31) >> 2; g = (v >> 5 & 63) << 2 | (v >> 5 & 63) >> 4;
            b = (v >> 11 & 31) << 3 | (v >> 11 & 31) >> 2; al = 0; break;
    case 1: r = (v & 31) << 3 | (v & 31) >> 2; g = (v >> 5 & 31) << 3 | (v >> 5 & 31) >> 2;
            b = (v >> 10 & 31) << 3 | (v >> 10 & 31) >> 2; al = (v >> 15) ? 255 : 0; break;
    default: r = (v & 15) * 17; g = (v >> 4 & 15) * 17; b = (v >> 8 & 15) * 17; al = (v >> 12) * 17; break;
    }
    return r | g << 8 | b << 16 | al << 24;
}

static void fb_write(const Target *t, int x, int y, uint32_t c, uint32_t mask) {
    uint32_t a = t->fb + (uint32_t)(y * (int)t->fb_stride + x) * (t->fb_fmt == 3 ? 4 : 2);
    if (t->fb_fmt == 3) {
        if (mask) c = (c & ~mask) | (mem_r32(a) & mask);
        mem_w32(a, c);
        return;
    }
    uint32_t r = c & 0xFF, g = c >> 8 & 0xFF, b = c >> 16 & 0xFF, al = c >> 24, v;
    switch (t->fb_fmt) {
    case 0: v = r >> 3 | (g >> 2) << 5 | (b >> 3) << 11; break;
    case 1: v = r >> 3 | (g >> 3) << 5 | (b >> 3) << 10 | (al >= 0x80 ? 0x8000u : 0u); break;
    default: v = r >> 4 | (g >> 4) << 4 | (b >> 4) << 8 | (al >> 4) << 12; break;
    }
    if (mask) {
        uint32_t old = mem_r16(a), m16;
        switch (t->fb_fmt) {
        case 0: m16 = (mask & 0xF8 ? 0x1F : 0) | (mask & 0xFC00 ? 0x7E0 : 0) | (mask & 0xF80000 ? 0xF800 : 0); break;
        case 1: m16 = (mask & 0xF8 ? 0x1F : 0) | (mask & 0xF800 ? 0x3E0 : 0) | (mask & 0xF80000 ? 0x7C00 : 0) |
                      (mask & 0x80000000u ? 0x8000 : 0); break;
        default: m16 = (mask & 0xF0 ? 0xF : 0) | (mask & 0xF000 ? 0xF0 : 0) | (mask & 0xF00000 ? 0xF00 : 0) |
                       (mask & 0xF0000000u ? 0xF000 : 0); break;
        }
        v = (v & ~m16) | (old & m16);
    }
    mem_w16(a, (uint16_t)v);
}

// ---------------------------------------------------------------- textures

typedef struct {
    const uint32_t *pix;  // decoded RGBA8888 texels (texture cache)
    int enabled, fmt, swizzle, w, h, stride, clamp_u, clamp_v, linear, func, rgba, dbl;
    uint32_t addr;
    float env[3];
    int clut_fmt, clut_shift, clut_mask, clut_off;
} TexState;

static void tex_state(TexState *t) {
    t->enabled = R(GE_TEXTUREMAPENABLE) & 1;
    if (!t->enabled) return;
    t->fmt = (int)(R(GE_TEXFORMAT) & 0xF);
    t->swizzle = R(GE_TEXMODE) & 1;
    t->w = 1 << (R(GE_TEXSIZE0) & 0xF);
    t->h = 1 << (R(GE_TEXSIZE0) >> 8 & 0xF);
    t->stride = (int)(R(GE_TEXBUFWIDTH0) & 0x7FF);
    t->addr = (R(GE_TEXADDR0) & 0xFFFFF0u) | ((R(GE_TEXBUFWIDTH0) << 8) & 0x0F000000u);
    t->clamp_u = R(GE_TEXWRAP) & 1;
    t->clamp_v = R(GE_TEXWRAP) >> 8 & 1;
    t->linear = (R(GE_TEXFILTER) >> 8) & 1;
    t->func = (int)(R(GE_TEXFUNC) & 7);
    t->rgba = R(GE_TEXFUNC) >> 8 & 1;
    t->dbl = R(GE_TEXFUNC) >> 16 & 1;
    uint32_t env = R(GE_TEXENVCOLOR);
    t->env[0] = (float)(env & 0xFF); t->env[1] = (float)(env >> 8 & 0xFF); t->env[2] = (float)(env >> 16 & 0xFF);
    uint32_t cf = R(GE_CLUTFORMAT);
    t->clut_fmt = (int)(cf & 3);
    t->clut_shift = (int)(cf >> 2 & 0x1F);
    t->clut_mask = (int)(cf >> 8 & 0xFF);
    t->clut_off = (int)((cf >> 16 & 0x1F) << 4);
}

static uint32_t c16_to_32(uint32_t v, int fmt) {
    uint32_t r, g, b, a;
    switch (fmt) {
    case 0: r = (v & 31) << 3 | (v & 31) >> 2; g = (v >> 5 & 63) << 2 | (v >> 5 & 63) >> 4;
            b = (v >> 11 & 31) << 3 | (v >> 11 & 31) >> 2; a = 255; break;
    case 1: r = (v & 31) << 3 | (v & 31) >> 2; g = (v >> 5 & 31) << 3 | (v >> 5 & 31) >> 2;
            b = (v >> 10 & 31) << 3 | (v >> 10 & 31) >> 2; a = (v >> 15) ? 255 : 0; break;
    default: r = (v & 15) * 17; g = (v >> 4 & 15) * 17; b = (v >> 8 & 15) * 17; a = (v >> 12) * 17; break;
    }
    return r | g << 8 | b << 16 | a << 24;
}

static uint32_t clut_lookup(const TexState *t, uint32_t idx) {
    idx = ((idx >> t->clut_shift) & (uint32_t)t->clut_mask) | (uint32_t)t->clut_off;
    if (t->clut_fmt == 3) return g_ge.clut[idx & 255];
    const uint16_t *c16 = (const uint16_t *)g_ge.clut;
    return c16_to_32(c16[idx & 511], t->clut_fmt);
}

// Byte address of texel (x, y) for a texture with `bpp_bits` bits per texel.
static uint32_t texel_addr(const TexState *t, int x, int y, int bpp_bits) {
    uint32_t row_bytes = (uint32_t)t->stride * (uint32_t)bpp_bits / 8;
    uint32_t xb = (uint32_t)x * (uint32_t)bpp_bits / 8;
    if (!t->swizzle) return t->addr + (uint32_t)y * row_bytes + xb;
    // swizzled: 16-byte x 8-row blocks laid out linearly
    uint32_t blocks_per_row = row_bytes / 16;
    uint32_t block = ((uint32_t)y / 8) * blocks_per_row + xb / 16;
    return t->addr + block * 128 + ((uint32_t)y % 8) * 16 + xb % 16;
}

static uint32_t dxt_color(uint32_t a, int x, int y, int alpha_mode);

static uint32_t fetch(const TexState *t, int x, int y) {
    switch (t->fmt) {
    case 0: case 1: case 2: return c16_to_32(mem_r16(texel_addr(t, x, y, 16)), t->fmt);
    case 3: return mem_r32(texel_addr(t, x, y, 32));
    case 4: {
        uint8_t b = mem_r8(texel_addr(t, x, y, 4));
        return clut_lookup(t, (x & 1) ? b >> 4 : b & 15);
    }
    case 5: return clut_lookup(t, mem_r8(texel_addr(t, x, y, 8)));
    case 6: return clut_lookup(t, mem_r16(texel_addr(t, x, y, 16)));
    case 7: return clut_lookup(t, mem_r32(texel_addr(t, x, y, 32)));
    default: {  // DXT1/3/5: 4x4 blocks
        int bsize = t->fmt == 8 ? 8 : 16;
        uint32_t blocks_per_row = (uint32_t)(t->stride / 4);
        uint32_t a = t->addr + (((uint32_t)y / 4) * blocks_per_row + (uint32_t)x / 4) * (uint32_t)bsize;
        return dxt_color(a, x & 3, y & 3, t->fmt);
    }
    }
}

static uint32_t rgb565(uint16_t v) { return c16_to_32(v, 0) & 0xFFFFFFu; }

static uint32_t dxt_color(uint32_t a, int x, int y, int fmt) {
    uint32_t cblk = fmt == 8 ? a : a + 8;  // DXT3/5: alpha first
    uint16_t c0 = mem_r16(cblk), c1 = mem_r16(cblk + 2);
    uint32_t bits = mem_r32(cblk + 4), code = (bits >> ((y * 4 + x) * 2)) & 3;
    uint32_t p0 = rgb565(c0), p1 = rgb565(c1), col;
#define MIX(a_, b_, n, d) ((((a_ & 0xFF) * n + (b_ & 0xFF) * (d - n)) / d) | ((((a_ >> 8 & 0xFF) * n + (b_ >> 8 & 0xFF) * (d - n)) / d) << 8) | ((((a_ >> 16 & 0xFF) * n + (b_ >> 16 & 0xFF) * (d - n)) / d) << 16))
    if (code == 0) col = p0;
    else if (code == 1) col = p1;
    else if (c0 > c1 || fmt != 8) col = code == 2 ? MIX(p0, p1, 2, 3) : MIX(p0, p1, 1, 3);
    else col = code == 2 ? MIX(p0, p1, 1, 2) : 0;
#undef MIX
    uint32_t alpha = 255;
    if (fmt == 8) alpha = (c0 <= c1 && code == 3) ? 0 : 255;
    else if (fmt == 9) alpha = ((mem_r16(a + (uint32_t)y * 2) >> (x * 4)) & 15) * 17;
    else {
        uint32_t a0 = mem_r8(a), a1 = mem_r8(a + 1);
        uint64_t ab = 0;
        for (int i = 0; i < 6; i++) ab |= (uint64_t)mem_r8(a + 2 + (uint32_t)i) << (8 * i);
        uint32_t ac = (uint32_t)(ab >> ((y * 4 + x) * 3)) & 7;
        if (ac == 0) alpha = a0;
        else if (ac == 1) alpha = a1;
        else if (a0 > a1) alpha = ((8 - ac) * a0 + (ac - 1) * a1) / 7;
        else alpha = ac == 6 ? 0 : ac == 7 ? 255 : ((6 - ac) * a0 + (ac - 1) * a1) / 5;
    }
    return col | alpha << 24;
}

static int wrap(int c, int size, int clamp) {
    if (clamp) return c < 0 ? 0 : c >= size ? size - 1 : c;
    return c & (size - 1);
}

// ---------------------------------------------------------------- texture cache
// Decoded textures are kept across frames. When something may have written
// texture memory (TEXFLUSH, block transfers, a new frame) the generation
// changes and entries are re-validated by hashing their source bytes.

#define TEX_CACHE 512
typedef struct {
    uint32_t addr, key_fmt, key_size, key_clut, clut_hash, gen;
    uint64_t data_hash;
    uint32_t *pix;
    int w, h;
} TexEntry;
static TexEntry g_tex_cache[TEX_CACHE];
static uint32_t g_tex_gen = 1;

void gpu_tex_invalidate(void) { g_tex_gen++; }

static uint64_t hash_bytes(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static uint32_t tex_bytes(const TexState *t) {
    static const int bpp[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};
    uint32_t rows = (uint32_t)t->h, stride = (uint32_t)(t->stride < t->w ? t->w : t->stride);
    if (t->fmt >= 8) rows = (rows + 3) / 4 * 4;
    return rows * stride * (uint32_t)bpp[t->fmt] / 8;
}

void raster_flush(void);

// Framebuffer ranges that queued (not yet drawn) primitives write to. A
// texture only needs the queue drained if it reads from one of them.
#define MAX_PENDING_TARGETS 8
static struct { uint32_t lo, hi; } g_pending_targets[MAX_PENDING_TARGETS];
static int g_n_pending_targets;

static void note_target(uint32_t lo, uint32_t hi) {
    lo &= 0x1FFFFFFF;
    hi &= 0x1FFFFFFF;
    for (int k = 0; k < g_n_pending_targets; k++)
        if (g_pending_targets[k].lo == lo && g_pending_targets[k].hi == hi) return;
    if (g_n_pending_targets == MAX_PENDING_TARGETS) {
        g_flush_reason[3]++;
        raster_flush();
    }
    g_pending_targets[g_n_pending_targets].lo = lo;
    g_pending_targets[g_n_pending_targets].hi = hi;
    g_n_pending_targets++;
}

static void flush_if_rendering_to(uint32_t lo, uint32_t hi) {
    lo &= 0x1FFFFFFF;
    hi &= 0x1FFFFFFF;
    for (int k = 0; k < g_n_pending_targets; k++)
        if (lo < g_pending_targets[k].hi && g_pending_targets[k].lo < hi) {
            g_flush_reason[1]++;
            raster_flush();
            return;
        }
}

static void tex_cache_lookup(TexState *t) {
    uint32_t key_fmt = (uint32_t)t->fmt | (uint32_t)t->swizzle << 8 | (uint32_t)t->stride << 16;
    uint32_t key_size = (uint32_t)t->w | (uint32_t)t->h << 16;
    uint32_t key_clut = (uint32_t)t->clut_fmt | (uint32_t)t->clut_shift << 4 | (uint32_t)t->clut_mask << 12 |
                        (uint32_t)t->clut_off << 20;
    uint32_t clut_hash = 0;
    if (t->fmt >= 4 && t->fmt <= 7) clut_hash = (uint32_t)hash_bytes((const uint8_t *)g_ge.clut, sizeof g_ge.clut);
    uint32_t slot = (t->addr * 2654435761u ^ key_fmt ^ key_size * 31u) % TEX_CACHE;
    TexEntry *e = &g_tex_cache[slot];
    int same_key = e->pix && e->addr == t->addr && e->key_fmt == key_fmt && e->key_size == key_size &&
                   e->key_clut == key_clut && e->clut_hash == clut_hash;
    if (same_key && e->gen == g_tex_gen) {
        t->pix = e->pix;
        return;
    }
    uint32_t n = tex_bytes(t);
    flush_if_rendering_to(t->addr, t->addr + n);  // the source may still be being drawn
    uint64_t h = (t->addr & 0x1FFFFFFF) + n <= GUEST_MEM_SIZE ? hash_bytes(host(t->addr), n) : 0;
    if (same_key && e->data_hash == h) {
        e->gen = g_tex_gen;
        t->pix = e->pix;
        return;
    }
    if (!e->pix || e->w * e->h < t->w * t->h) {
        free(e->pix);
        e->pix = malloc((size_t)t->w * (size_t)t->h * 4);
    }
    t->pix = NULL;  // fetch() decodes from guest memory
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) e->pix[y * t->w + x] = fetch(t, x, y);
    e->addr = t->addr; e->key_fmt = key_fmt; e->key_size = key_size; e->key_clut = key_clut;
    e->clut_hash = clut_hash; e->gen = g_tex_gen; e->data_hash = h; e->w = t->w; e->h = t->h;
    t->pix = e->pix;
}

// u, v in texels
static void sample(const TexState *t, float u, float v, float *out) {
    const uint32_t *p = t->pix;
    if (!t->linear) {
        int x = wrap((int)floorf(u), t->w, t->clamp_u), y = wrap((int)floorf(v), t->h, t->clamp_v);
        uint32_t c = p[y * t->w + x];
        out[0] = (float)(c & 0xFF); out[1] = (float)(c >> 8 & 0xFF);
        out[2] = (float)(c >> 16 & 0xFF); out[3] = (float)(c >> 24);
        return;
    }
    u -= 0.5f;
    v -= 0.5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - (float)x0, fy = v - (float)y0;
    int xa = wrap(x0, t->w, t->clamp_u), xb = wrap(x0 + 1, t->w, t->clamp_u);
    int ya = wrap(y0, t->h, t->clamp_v) * t->w, yb = wrap(y0 + 1, t->h, t->clamp_v) * t->w;
    uint32_t c[4] = {p[ya + xa], p[ya + xb], p[yb + xa], p[yb + xb]};
    for (int i = 0; i < 4; i++) {
        float a = (float)(c[0] >> (8 * i) & 0xFF), b = (float)(c[1] >> (8 * i) & 0xFF);
        float cc = (float)(c[2] >> (8 * i) & 0xFF), d = (float)(c[3] >> (8 * i) & 0xFF);
        out[i] = (a * (1 - fx) + b * fx) * (1 - fy) + (cc * (1 - fx) + d * fx) * fy;
    }
}

// ---------------------------------------------------------------- per-pixel

typedef struct {
    Target tg;
    TexState tex;
    int clear, clear_color, clear_alpha, clear_depth;
    int alpha_test, at_func, at_ref, at_mask;
    int z_test, z_func, z_write;
    int blend, bl_src, bl_dst, bl_eq;
    uint32_t fix_a, fix_b, write_mask;
    int through;
} PixState;

static void pix_state(PixState *p, int through) {
    target(&p->tg);
    tex_state(&p->tex);
    if (p->tex.enabled) tex_cache_lookup(&p->tex);
    p->through = through;
    uint32_t cm = R(GE_CLEARMODE);
    p->clear = cm & 1;
    p->clear_color = cm >> 8 & 1;
    p->clear_alpha = cm >> 9 & 1;
    p->clear_depth = cm >> 10 & 1;
    p->alpha_test = R(GE_ALPHATESTENABLE) & 1;
    p->at_func = (int)(R(GE_ALPHATEST) & 7);
    p->at_ref = (int)(R(GE_ALPHATEST) >> 8 & 0xFF);
    p->at_mask = (int)(R(GE_ALPHATEST) >> 16 & 0xFF);
    p->z_test = R(GE_ZTESTENABLE) & 1;
    p->z_func = (int)(R(GE_ZTEST) & 7);
    p->z_write = !(R(GE_ZWRITEDISABLE) & 1);
    p->blend = R(GE_ALPHABLENDENABLE) & 1;
    p->bl_src = (int)(R(GE_BLENDMODE) & 0xF);
    p->bl_dst = (int)(R(GE_BLENDMODE) >> 4 & 0xF);
    p->bl_eq = (int)(R(GE_BLENDMODE) >> 8 & 7);
    p->fix_a = R(GE_BLENDFIXEDA);
    p->fix_b = R(GE_BLENDFIXEDB);
    p->write_mask = (R(GE_MASKRGB) & 0xFFFFFF) | (R(GE_MASKALPHA) & 0xFF) << 24;
}

static int cmp(int func, int a, int b) {
    switch (func) {
    case 0: return 0;
    case 1: return 1;
    case 2: return a == b;
    case 3: return a != b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return a > b;
    default: return a >= b;
    }
}

static float blend_factor(int f, int is_src, const float *s, const float *d, uint32_t fix, int ch) {
    float sa = s[3] / 255.f, da = d[3] / 255.f;
    switch (f) {
    case 0: return is_src ? d[ch] / 255.f : s[ch] / 255.f;
    case 1: return is_src ? 1.f - d[ch] / 255.f : 1.f - s[ch] / 255.f;
    case 2: return sa;
    case 3: return 1.f - sa;
    case 4: return da;
    case 5: return 1.f - da;
    case 6: return 2.f * sa;
    case 7: return 1.f - 2.f * sa;
    case 8: return 2.f * da;
    case 9: return 1.f - 2.f * da;
    default: return (float)(fix >> (8 * ch) & 0xFF) / 255.f;
    }
}

static inline float clamp255(float v) { return v < 0.f ? 0.f : v > 255.f ? 255.f : v; }

// ---------------------------------------------------------------- parallel bands
// With RASTER_THREADS > 0, primitives are queued and every worker rasterizes
// all of them, but only on its own rows (8-row bands, interleaved).
static int g_nbands = 1;
static __thread int tl_band;
// `band` is read from tl_band once per primitive: thread-locals are slow on some platforms
static inline int owns_row(int y, int band) { return g_nbands == 1 || ((y >> 3) % g_nbands) == band; }

// Shade one pixel. col: interpolated vertex color; uv in texels.
static void shade(const PixState *p, int x, int y, float z, const float *col, float u, float v) {
    const Target *t = &p->tg;
    if (x < t->sx1 || x > t->sx2 || y < t->sy1 || y > t->sy2 || x < 0 || y < 0 || x >= 1024 || y >= 1024) return;
    uint32_t za = t->zb + (uint32_t)(y * (int)t->zb_stride + x) * 2;
    int zi = (int)(z < 0.f ? 0.f : z > 65535.f ? 65535.f : z);

    if (p->clear) {
        uint32_t c = (uint32_t)clamp255(col[0]) | (uint32_t)clamp255(col[1]) << 8 |
                     (uint32_t)clamp255(col[2]) << 16 | (uint32_t)clamp255(col[3]) << 24;
        uint32_t keep = (p->clear_color ? 0 : 0xFFFFFFu) | (p->clear_alpha ? 0 : 0xFF000000u);
        if (keep != 0xFFFFFFFFu) fb_write(t, x, y, c, keep);
        if (p->clear_depth) mem_w16(za, (uint16_t)zi);
        return;
    }

    // early depth test: skip texturing hidden pixels (alpha test must come first otherwise)
    int z_done = 0;
    if (p->z_test && !p->alpha_test) {
        if (!cmp(p->z_func, zi, mem_r16(za))) return;
        z_done = 1;
    }

    float c[4] = {col[0], col[1], col[2], col[3]};
    if (p->tex.enabled) {
        float tc[4];
        sample(&p->tex, u, v, tc);
        const TexState *tx = &p->tex;
        switch (tx->func) {
        case 0:  // modulate
            for (int i = 0; i < 3; i++) c[i] = c[i] * tc[i] / 255.f;
            c[3] = tx->rgba ? c[3] * tc[3] / 255.f : c[3];
            break;
        case 1:  // decal
            if (tx->rgba) for (int i = 0; i < 3; i++) c[i] = (tc[i] * tc[3] + c[i] * (255.f - tc[3])) / 255.f;
            else for (int i = 0; i < 3; i++) c[i] = tc[i];
            break;
        case 2:  // blend with env color
            for (int i = 0; i < 3; i++) c[i] = (c[i] * (255.f - tc[i]) + tx->env[i] * tc[i]) / 255.f;
            c[3] = tx->rgba ? c[3] * tc[3] / 255.f : c[3];
            break;
        case 3:  // replace
            for (int i = 0; i < 3; i++) c[i] = tc[i];
            if (tx->rgba) c[3] = tc[3];
            break;
        default:  // add
            for (int i = 0; i < 3; i++) c[i] = clamp255(c[i] + tc[i]);
            c[3] = tx->rgba ? c[3] * tc[3] / 255.f : c[3];
            break;
        }
        if (tx->dbl) for (int i = 0; i < 3; i++) c[i] = clamp255(c[i] * 2.f);
    }

    if (p->alpha_test) {
        int a = (int)clamp255(c[3]) & p->at_mask, ref = p->at_ref & p->at_mask;
        if (!cmp(p->at_func, a, ref)) return;
    }
    if (p->z_test && !z_done) {
        if (!cmp(p->z_func, zi, mem_r16(za))) return;
    }
    if (p->blend) {
        uint32_t dv = fb_read(t, x, y);
        float d[4] = {(float)(dv & 0xFF), (float)(dv >> 8 & 0xFF), (float)(dv >> 16 & 0xFF), (float)(dv >> 24)};
        float out[3];
        for (int i = 0; i < 3; i++) {
            float fa = blend_factor(p->bl_src, 1, c, d, p->fix_a, i);
            float fb = blend_factor(p->bl_dst, 0, c, d, p->fix_b, i);
            switch (p->bl_eq) {
            case 0: out[i] = c[i] * fa + d[i] * fb; break;
            case 1: out[i] = c[i] * fa - d[i] * fb; break;
            case 2: out[i] = d[i] * fb - c[i] * fa; break;
            case 3: out[i] = c[i] < d[i] ? c[i] : d[i]; break;
            case 4: out[i] = c[i] > d[i] ? c[i] : d[i]; break;
            default: out[i] = fabsf(c[i] - d[i]); break;
            }
        }
        c[0] = out[0]; c[1] = out[1]; c[2] = out[2];
        c[3] = d[3];  // alpha holds stencil; blending never changes it
    }
    uint32_t px = (uint32_t)clamp255(c[0]) | (uint32_t)clamp255(c[1]) << 8 | (uint32_t)clamp255(c[2]) << 16 |
                  (uint32_t)clamp255(c[3]) << 24;
    uint32_t mask = p->write_mask | (p->blend ? 0xFF000000u : 0);
    fb_write(t, x, y, px, mask);
    if (p->z_write && p->z_test) mem_w16(za, (uint16_t)zi);
}

// ---------------------------------------------------------------- primitives

static inline float edge(const SVtx *a, const SVtx *b, float x, float y) {
    return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
}

static void draw_triangle(const PixState *p, const SVtx *v0, const SVtx *v1, const SVtx *v2, int flat) {
    float area = edge(v0, v1, v2->x, v2->y);
    if (area == 0.f || !isfinite(area)) return;
    const Target *t = &p->tg;
    float minx = fminf(v0->x, fminf(v1->x, v2->x)), maxx = fmaxf(v0->x, fmaxf(v1->x, v2->x));
    float miny = fminf(v0->y, fminf(v1->y, v2->y)), maxy = fmaxf(v0->y, fmaxf(v1->y, v2->y));
    int x0 = (int)fmaxf(floorf(minx), (float)t->sx1), x1 = (int)fminf(ceilf(maxx), (float)t->sx2);
    int y0 = (int)fmaxf(floorf(miny), (float)t->sy1), y1 = (int)fminf(ceilf(maxy), (float)t->sy2);
    if (x1 < x0 || y1 < y0 || x1 - x0 > 2048 || y1 - y0 > 2048) return;
    float inv = 1.f / area;
    // barycentric weights are affine in screen space: w(x,y) = w0 + dx*x + dy*y
    float e0x = -(v2->y - v1->y) * inv, e0y = (v2->x - v1->x) * inv;
    float e1x = -(v0->y - v2->y) * inv, e1y = (v0->x - v2->x) * inv;
    float e2x = -(v1->y - v0->y) * inv, e2y = (v1->x - v0->x) * inv;
    float px0 = (float)x0 + 0.5f, py0 = (float)y0 + 0.5f;
    float r0 = edge(v1, v2, px0, py0) * inv, r1 = edge(v2, v0, px0, py0) * inv, r2 = edge(v0, v1, px0, py0) * inv;
    float tw = p->through ? 1.f : (float)p->tex.w, th = p->through ? 1.f : (float)p->tex.h;
    int persp = !p->through;
    float flat_col[4];
    for (int i = 0; i < 4; i++) flat_col[i] = v2->c[i] / v2->iw;
    int band = tl_band;
    for (int y = y0; y <= y1; y++, r0 += e0y, r1 += e1y, r2 += e2y) {
        if (!owns_row(y, band)) continue;
        // span of this row where all three weights are >= 0 (w = r + e*(x - x0))
        float lo = 0.f, hi = (float)(x1 - x0);
        const float rr[3] = {r0, r1, r2}, ee[3] = {e0x, e1x, e2x};
        for (int i = 0; i < 3; i++) {
            if (ee[i] > 0.f) lo = fmaxf(lo, -rr[i] / ee[i]);
            else if (ee[i] < 0.f) hi = fminf(hi, rr[i] / -ee[i]);
            else if (rr[i] < 0.f) hi = -1.f;
        }
        if (hi < lo) continue;
        int xs = x0 + (int)ceilf(lo - 0.001f), xe = x0 + (int)floorf(hi + 0.001f);
        if (xs < x0) xs = x0;
        if (xe > x1) xe = x1;
        float dx = (float)(xs - x0);
        float w0 = r0 + e0x * dx, w1 = r1 + e1x * dx, w2 = r2 + e2x * dx;
        for (int x = xs; x <= xe; x++, w0 += e0x, w1 += e1x, w2 += e2x) {
            if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
            float k = persp ? 1.f / (w0 * v0->iw + w1 * v1->iw + w2 * v2->iw) : 1.f;
            float z = w0 * v0->z + w1 * v1->z + w2 * v2->z;
            float col[4];
            if (flat) memcpy(col, flat_col, sizeof col);
            else
                for (int i = 0; i < 4; i++) col[i] = (w0 * v0->c[i] + w1 * v1->c[i] + w2 * v2->c[i]) * k;
            float u = (w0 * v0->u + w1 * v1->u + w2 * v2->u) * k * tw;
            float v = (w0 * v0->v + w1 * v1->v + w2 * v2->v) * k * th;
            shade(p, x, y, z, col, u, v);
        }
    }
}

// RECTANGLES: two corner vertices; color/z from the second one.
static void draw_rect(const PixState *p, const SVtx *a, const SVtx *b) {
    float x0 = fminf(a->x, b->x), x1 = fmaxf(a->x, b->x), y0 = fminf(a->y, b->y), y1 = fmaxf(a->y, b->y);
    int ix0 = (int)ceilf(x0 - 0.5f), ix1 = (int)ceilf(x1 - 0.5f), iy0 = (int)ceilf(y0 - 0.5f), iy1 = (int)ceilf(y1 - 0.5f);
    float col[4];
    for (int i = 0; i < 4; i++) col[i] = b->c[i] / b->iw;
    float du = (b->u - a->u) / (b->x - a->x ? b->x - a->x : 1.f);
    float dv = (b->v - a->v) / (b->y - a->y ? b->y - a->y : 1.f);
    float tw = (float)p->tex.w, th = (float)p->tex.h;
    int band = tl_band;
    for (int y = iy0; y < iy1; y++) {
        if (!owns_row(y, band)) continue;
        for (int x = ix0; x < ix1; x++) {
            float u = a->u + ((float)x + 0.5f - a->x) * du, v = a->v + ((float)y + 0.5f - a->y) * dv;
            if (!p->through) {
                u *= tw;
                v *= th;
            }
            shade(p, x, y, b->z, col, u, v);
        }
    }
}

static void draw_line(const PixState *p, const SVtx *a, const SVtx *b) {
    float dx = b->x - a->x, dy = b->y - a->y;
    int n = (int)fmaxf(fabsf(dx), fabsf(dy));
    for (int i = 0; i <= n; i++) {
        float t = n ? (float)i / (float)n : 0.f, col[4];
        for (int k = 0; k < 4; k++) col[k] = (a->c[k] / a->iw) * (1 - t) + (b->c[k] / b->iw) * t;
        int py = (int)(a->y + dy * t);
        if (owns_row(py, tl_band)) shade(p, (int)(a->x + dx * t), py, a->z + (b->z - a->z) * t, col, a->u, a->v);
    }
}

// ---------------------------------------------------------------- job queue

enum { JOB_TRI, JOB_RECT, JOB_LINE, JOB_POINT };
typedef struct {
    uint8_t kind, flat;
    uint16_t state;
    SVtx v[3];
} Job;

#define NJOBS 16384
#define NSTATES 8192
#define MAX_WORKERS 16
static Job g_jobs[NJOBS];
static PixState g_states[NSTATES];
static int g_state_next;
static uint64_t g_job_w, g_job_r[MAX_WORKERS];
static uint64_t g_job_pending, g_job_r_min_cache;  // producer side (GPU thread only)
static int g_nworkers;
static pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_work = PTHREAD_COND_INITIALIZER, g_q_done = PTHREAD_COND_INITIALIZER;
static int g_cur_state = -1;
static const PixState *g_cur_ps;

static void run_job(const Job *j) {
    const PixState *p = &g_states[j->state];
    switch (j->kind) {
    case JOB_TRI: draw_triangle(p, &j->v[0], &j->v[1], &j->v[2], j->flat); break;
    case JOB_RECT: draw_rect(p, &j->v[0], &j->v[1]); break;
    case JOB_LINE: draw_line(p, &j->v[0], &j->v[1]); break;
    default: {
        float col[4];
        for (int k = 0; k < 4; k++) col[k] = j->v[0].c[k] / j->v[0].iw;
        if (owns_row((int)j->v[0].y, tl_band))
            shade(p, (int)j->v[0].x, (int)j->v[0].y, j->v[0].z, col, j->v[0].u, j->v[0].v);
    }
    }
}

static void *raster_worker(void *arg) {
    int me = (int)(intptr_t)arg;
    tl_band = me;
    pthread_mutex_lock(&g_q_lock);
    for (;;) {
        while (g_job_r[me] == g_job_w) pthread_cond_wait(&g_q_work, &g_q_lock);
        uint64_t end = g_job_w;
        pthread_mutex_unlock(&g_q_lock);
        for (uint64_t i = g_job_r[me]; i < end; i++) run_job(&g_jobs[i % NJOBS]);
        pthread_mutex_lock(&g_q_lock);
        g_job_r[me] = end;
        pthread_cond_broadcast(&g_q_done);
    }
    return NULL;
}

static uint64_t min_read(void) {
    uint64_t m = g_job_w;
    for (int k = 0; k < g_nworkers; k++)
        if (g_job_r[k] < m) m = g_job_r[k];
    return m;
}

static void kick(void);

// Wait until every queued primitive has been drawn.
uint64_t g_flush_wait_us, g_prim_us;  // stats
uint64_t hle_time_us(void);

void raster_flush(void) {
    if (!g_nworkers) return;
    kick();
    uint64_t t0 = hle_time_us();
    pthread_mutex_lock(&g_q_lock);
    while (min_read() != g_job_w) pthread_cond_wait(&g_q_done, &g_q_lock);
    pthread_mutex_unlock(&g_q_lock);
    g_n_pending_targets = 0;
    g_flush_wait_us += hle_time_us() - t0;
}

static void raster_init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *e = getenv("RASTER_THREADS");
    long n = e ? atol(e) : sysconf(_SC_NPROCESSORS_ONLN) - 2;
    if (n < 0) n = 0;
    if (n > MAX_WORKERS) n = MAX_WORKERS;
    if (n == 1) n = 0;  // one worker buys nothing over drawing inline
    g_nworkers = (int)n;
    g_nbands = n ? (int)n : 1;
    for (int k = 0; k < g_nworkers; k++) {
        pthread_t t;
        pthread_create(&t, NULL, raster_worker, (void *)(intptr_t)k);
        pthread_detach(t);
    }
    fprintf(stderr, "raster: %d worker threads\n", g_nworkers);
}

// Snapshot the pixel state for the primitives that follow.
static void begin_state(const PixState *ps) {
    g_cur_ps = ps;
    if (!g_nworkers) return;
    {
        const Target *t = &ps->tg;
        uint32_t bpp = t->fb_fmt == 3 ? 4 : 2, rows = (uint32_t)t->sy2 + 1;
        note_target(t->fb, t->fb + t->fb_stride * rows * bpp);
        if (ps->z_test || ps->clear_depth) note_target(t->zb, t->zb + t->zb_stride * rows * 2);
    }
    // consecutive primitives usually share their state: reuse the last snapshot
    if (g_cur_state >= 0 && !memcmp(&g_states[g_cur_state], ps, sizeof *ps)) return;
    if (g_state_next == NSTATES) {  // wrapping: states still referenced must be done
        g_flush_reason[4]++;
        raster_flush();
        g_state_next = 0;
    }
    g_states[g_state_next] = *ps;
    g_cur_state = g_state_next++;
}

static void submit(int kind, const SVtx *a, const SVtx *b, const SVtx *c, int flat) {
    if (!g_nworkers) {
        Job j = {.kind = (uint8_t)kind, .flat = (uint8_t)flat};
        j.v[0] = *a;
        if (b) j.v[1] = *b;
        if (c) j.v[2] = *c;
        switch (kind) {  // draw inline with the live state
        case JOB_TRI: draw_triangle(g_cur_ps, a, b, c, flat); break;
        case JOB_RECT: draw_rect(g_cur_ps, a, b); break;
        case JOB_LINE: draw_line(g_cur_ps, a, b); break;
        default: {
            float col[4];
            for (int k = 0; k < 4; k++) col[k] = a->c[k] / a->iw;
            shade(g_cur_ps, (int)a->x, (int)a->y, a->z, col, a->u, a->v);
        }
        }
        return;
    }
    // Jobs are written past the published end without the lock and made
    // visible in batches by kick(); only a full ring makes us wait.
    if (g_job_pending - g_job_r_min_cache >= NJOBS) {
        pthread_mutex_lock(&g_q_lock);
        g_job_w = g_job_pending;  // publish what we have so workers can drain
        pthread_cond_broadcast(&g_q_work);
        while (g_job_pending - min_read() >= NJOBS) pthread_cond_wait(&g_q_done, &g_q_lock);
        g_job_r_min_cache = min_read();
        pthread_mutex_unlock(&g_q_lock);
    }
    Job *j = &g_jobs[g_job_pending % NJOBS];
    j->kind = (uint8_t)kind;
    j->flat = (uint8_t)flat;
    j->state = (uint16_t)g_cur_state;
    j->v[0] = *a;
    if (b) j->v[1] = *b;
    if (c) j->v[2] = *c;
    g_job_pending++;
}

// Make queued work visible to the workers (end of a primitive batch).
static void kick(void) {
    if (!g_nworkers || g_job_pending == g_job_w) return;
    pthread_mutex_lock(&g_q_lock);
    g_job_w = g_job_pending;
    g_job_r_min_cache = min_read();
    pthread_cond_broadcast(&g_q_work);
    pthread_mutex_unlock(&g_q_lock);
}

// Clip a triangle against the near plane (z > -w) in clip space.
static int clip_near(const Vtx *in, int n, Vtx *out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const Vtx *a = &in[i], *b = &in[(i + 1) % n];
        float da = a->z + a->w, db = b->z + b->w;
        if (da >= 0.f) out[m++] = *a;
        if ((da >= 0.f) != (db >= 0.f)) {
            float t = da / (da - db);
            Vtx *o = &out[m++];
            o->x = a->x + (b->x - a->x) * t; o->y = a->y + (b->y - a->y) * t;
            o->z = a->z + (b->z - a->z) * t; o->w = a->w + (b->w - a->w) * t;
            o->u = a->u + (b->u - a->u) * t; o->v = a->v + (b->v - a->v) * t;
            for (int k = 0; k < 4; k++) o->c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
        }
    }
    return m;
}

static int cull_reject(const SVtx *a, const SVtx *b, const SVtx *c, int odd) {
    if (!(R(GE_CULLFACEENABLE) & 1)) return 0;
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    int ccw = area > 0.f;
    if (odd) ccw = !ccw;
    int cull_cw = R(GE_CULL) & 1;
    return cull_cw ? !ccw : ccw;
}

static void emit_triangle(const PixState *p, const Vtx *a, const Vtx *b, const Vtx *c, int odd, int flat) {
    Vtx in[3] = {*a, *b, *c}, clipped[8];
    int n = 3;
    const Vtx *poly = in;
    if (!p->through) {
        n = clip_near(in, 3, clipped);
        poly = clipped;
        if (n < 3) return;
    }
    SVtx s[8];
    for (int i = 0; i < n; i++) to_screen(&poly[i], &s[i], p->through);
    if (!p->through && cull_reject(&s[0], &s[1], &s[2], odd)) return;
    for (int i = 1; i + 1 < n; i++) submit(JOB_TRI, &s[0], &s[i], &s[i + 1], flat);
}

#define MAX_VERTS 65536
static Vtx g_verts[MAX_VERTS];

static void draw_prim(int prim, int count);
void gpu_draw_prim(int prim, int count) {
    uint64_t t0 = hle_time_us();
    draw_prim(prim, count);
    g_prim_us += hle_time_us() - t0;
}

static void draw_prim(int prim, int count) {
    g_prims++;
    if (count <= 0) return;
    if (count > MAX_VERTS) count = MAX_VERTS;
    VFormat f;
    vformat(&f, R(GE_VERTEXTYPE));
    raster_init();
    PixState ps;
    pix_state(&ps, f.through);
    begin_state(&ps);

    // gather vertices (indexed or sequential)
    uint32_t vbase = g_ge.vaddr;
    int max_index = 0;
    for (int i = 0; i < count; i++) {
        int idx = i;
        if (f.idx == 1) idx = mem_r8(g_ge.iaddr + (uint32_t)i);
        else if (f.idx == 2) idx = mem_r16(g_ge.iaddr + (uint32_t)i * 2);
        else if (f.idx == 3) idx = (int)(mem_r32(g_ge.iaddr + (uint32_t)i * 4) & 0xFFFF);
        if (idx > max_index) max_index = idx;
        decode_vertex(&f, vbase + (uint32_t)idx * (uint32_t)(f.size * f.nmorph), &g_verts[i]);
    }
    // the vertex/index pointers advance past what was consumed
    if (f.idx) g_ge.iaddr += (uint32_t)count * (f.idx == 1 ? 1u : f.idx == 2 ? 2u : 4u);
    else g_ge.vaddr += (uint32_t)count * (uint32_t)(f.size * f.nmorph);

    int flat = !(R(GE_SHADEMODE) & 1);
    Vtx *v = g_verts;
    switch (prim) {
    case 0:  // points
        for (int i = 0; i < count; i++) {
            SVtx s;
            to_screen(&v[i], &s, f.through);
            submit(JOB_POINT, &s, NULL, NULL, 0);
        }
        break;
    case 1: case 2:  // lines, line strip
        for (int i = 0; i + 1 < count; i += prim == 1 ? 2 : 1) {
            SVtx a, b;
            to_screen(&v[i], &a, f.through);
            to_screen(&v[i + 1], &b, f.through);
            submit(JOB_LINE, &a, &b, NULL, 0);
        }
        break;
    case 3:  // triangles
        for (int i = 0; i + 2 < count; i += 3) emit_triangle(&ps, &v[i], &v[i + 1], &v[i + 2], 0, flat);
        break;
    case 4:  // strip
        for (int i = 0; i + 2 < count; i++) emit_triangle(&ps, &v[i], &v[i + 1], &v[i + 2], i & 1, flat);
        break;
    case 5:  // fan
        for (int i = 1; i + 1 < count; i++) emit_triangle(&ps, &v[0], &v[i], &v[i + 1], 0, flat);
        break;
    case 6:  // rectangles
        for (int i = 0; i + 1 < count; i += 2) {
            SVtx a, b;
            to_screen(&v[i], &a, f.through);
            to_screen(&v[i + 1], &b, f.through);
            submit(JOB_RECT, &a, &b, NULL, 0);
        }
        break;
    }
    (void)max_index;
    kick();
}

// ---------------------------------------------------------------- transfers

// Copy a rectangle between two VRAM/RAM surfaces (GE_TRANSFERSTART).
void gpu_block_transfer(void) {
    g_flush_reason[2]++;
    raster_flush();
    const uint32_t *r = g_ge.cmd;
    uint32_t src = (r[GE_TRANSFERSRC] & 0xFFFFF0u) | ((r[GE_TRANSFERSRCW] & 0xFF0000u) << 8);
    uint32_t dst = (r[GE_TRANSFERDST] & 0xFFFFF0u) | ((r[GE_TRANSFERDSTW] & 0xFF0000u) << 8);
    uint32_t sstride = r[GE_TRANSFERSRCW] & 0x7F8u, dstride = r[GE_TRANSFERDSTW] & 0x7F8u;
    uint32_t sx = r[GE_TRANSFERSRCPOS] & 0x3FF, sy = (r[GE_TRANSFERSRCPOS] >> 10) & 0x3FF;
    uint32_t dx = r[GE_TRANSFERDSTPOS] & 0x3FF, dy = (r[GE_TRANSFERDSTPOS] >> 10) & 0x3FF;
    uint32_t w = (r[GE_TRANSFERSIZE] & 0x3FF) + 1, h = ((r[GE_TRANSFERSIZE] >> 10) & 0x3FF) + 1;
    uint32_t bpp = (r[GE_TRANSFERSTART] & 1) ? 4 : 2;
    for (uint32_t y = 0; y < h; y++)
        memmove(host(dst + ((dy + y) * dstride + dx) * bpp), host(src + ((sy + y) * sstride + sx) * bpp),
                w * bpp);
}

// LOADCLUT: copy n*32 bytes of palette into the GE's internal palette.
void gpu_load_clut(void) {
    uint32_t addr = (R(GE_CLUTADDR) & 0xFFFFF0u) | ((R(GE_CLUTADDRUPPER) << 8) & 0x0F000000u);
    uint32_t bytes = (R(GE_LOADCLUT) & 0x3F) * 32;
    if (bytes > sizeof g_ge.clut) bytes = sizeof g_ge.clut;
    memcpy(g_ge.clut, host(addr), bytes);
}
