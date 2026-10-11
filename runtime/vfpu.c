// VFPU (PSP vector unit) interpreter, called from recompiled code.
//
// Register file: 128 floats, index = matrix*4 + column + row*32. Vector and
// matrix operands are decoded into index lists. Source prefixes (vpfxs/vpfxt)
// swizzle/negate/abs/constant-fill the inputs, the destination prefix (vpfxd)
// saturates and masks the outputs; all prefixes reset after each operation.
#include <math.h>
#include <stdio.h>

#include "recomp.h"

enum { CTRL_SPFX = 0, CTRL_TPFX = 1, CTRL_DPFX = 2, CTRL_CC = 3, CTRL_RCX0 = 8 };
#define PFX_NONE_ST 0xE4u

#define VD(op) ((op) & 0x7F)
#define VS(op) (((op) >> 8) & 0x7F)
#define VT(op) (((op) >> 16) & 0x7F)

static inline int vsize(uint32_t op) { return (int)(((op >> 7) & 1) | (((op >> 15) & 1) << 1)) + 1; }

static void vec_regs(int out[4], int n, int reg) {
    int mtx = (reg >> 2) & 7, col = reg & 3, row, tr = (reg >> 5) & 1;
    switch (n) {
    case 1: tr = 0; row = (reg >> 5) & 3; break;
    case 3: row = (reg >> 6) & 1; break;
    default: row = (reg >> 5) & 2; break;
    }
    for (int i = 0; i < n; i++)
        out[i] = mtx * 4 + (tr ? ((row + i) & 3) + col * 32 : col + ((row + i) & 3) * 32);
}

// m[j*4 + i]: column j, row i of the (possibly transposed) matrix operand
static void mtx_regs(int out[16], int n, int reg) {
    int mtx = (reg >> 2) & 7, col = reg & 3, row, tr = (reg >> 5) & 1;
    switch (n) {
    case 1: tr = 0; row = (reg >> 5) & 3; break;
    case 3: row = (reg >> 6) & 1; break;
    default: row = (reg >> 5) & 2; break;
    }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            out[j * 4 + i] = mtx * 4 + (tr ? ((row + i) & 3) + ((col + j) & 3) * 32
                                           : ((col + j) & 3) + ((row + i) & 3) * 32);
}

static void read_vec(Ctx *c, float *d, int n, int reg) {
    int r[4];
    vec_regs(r, n, reg);
    for (int i = 0; i < n; i++) d[i] = c->v[r[i]].f;
}

static void write_vec_raw(Ctx *c, const float *d, int n, int reg, uint32_t mask) {
    int r[4];
    vec_regs(r, n, reg);
    for (int i = 0; i < n; i++)
        if (!(mask & (1u << i))) c->v[r[i]].f = d[i];
}

static void read_mtx(Ctx *c, float *m, int n, int reg) {
    int r[16];
    mtx_regs(r, n, reg);
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) m[j * 4 + i] = c->v[r[j * 4 + i]].f;
}

static void write_mtx(Ctx *c, const float *m, int n, int reg) {
    int r[16];
    mtx_regs(r, n, reg);
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) c->v[r[j * 4 + i]].f = m[j * 4 + i];
}

static const float k_pfx_const[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f};

static void apply_st(float *v, int n, uint32_t pfx) {
    if (pfx == PFX_NONE_ST) return;
    float orig[4] = {v[0], v[1], v[2], v[3]};
    for (int i = 0; i < n; i++) {
        int sel = (pfx >> (i * 2)) & 3, abs_ = (pfx >> (8 + i)) & 1;
        int cst = (pfx >> (12 + i)) & 1, neg = (pfx >> (16 + i)) & 1;
        float x = cst ? k_pfx_const[sel + (abs_ << 2)] : (abs_ ? fabsf(orig[sel]) : orig[sel]);
        v[i] = neg ? -x : x;
    }
}

static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

// Saturate per the destination prefix, then write honouring its mask.
static void write_vec_d(Ctx *c, float *d, int n, int reg) {
    uint32_t p = c->vfpu_ctrl[CTRL_DPFX];
    for (int i = 0; i < n; i++) {
        int sat = (p >> (i * 2)) & 3;
        if (sat == 1) d[i] = clampf(d[i], 0.f, 1.f);
        else if (sat == 3) d[i] = clampf(d[i], -1.f, 1.f);
    }
    write_vec_raw(c, d, n, reg, (p >> 8) & 0xF);
}

// Integer results: only the write mask applies.
static void write_ivec_d(Ctx *c, const uint32_t *d, int n, int reg) {
    int r[4];
    uint32_t mask = (c->vfpu_ctrl[CTRL_DPFX] >> 8) & 0xF;
    vec_regs(r, n, reg);
    for (int i = 0; i < n; i++)
        if (!(mask & (1u << i))) c->v[r[i]].u = d[i];
}

static void read_s(Ctx *c, float *s, int n, int reg) {
    s[0] = s[1] = s[2] = s[3] = 0.f;
    read_vec(c, s, n, reg);
    apply_st(s, n, c->vfpu_ctrl[CTRL_SPFX]);
}
static void read_t(Ctx *c, float *t, int n, int reg) {
    t[0] = t[1] = t[2] = t[3] = 0.f;
    read_vec(c, t, n, reg);
    apply_st(t, n, c->vfpu_ctrl[CTRL_TPFX]);
}

static void eat_prefixes(Ctx *c) {
    c->vfpu_ctrl[CTRL_SPFX] = PFX_NONE_ST;
    c->vfpu_ctrl[CTRL_TPFX] = PFX_NONE_ST;
    c->vfpu_ctrl[CTRL_DPFX] = 0;
}

// Angles are in quarter turns: vsin(x) = sin(x * pi/2). Reduce first for precision.
static float qsin(float x) {
    double r = fmod((double)x, 4.0);
    return (float)sin(r * M_PI_2);
}
static float qcos(float x) {
    double r = fmod((double)x, 4.0);
    return (float)cos(r * M_PI_2);
}

static float half_to_float(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 0x3FF, u;
    if (e == 31) u = s | 0x7F800000u | (m << 13);
    else if (e == 0) {
        if (!m) u = s;
        else {
            float f = ldexpf((float)m, -24);
            memcpy(&u, &f, 4);
            u |= s;
        }
    } else u = s | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static const float k_cst[32] = {
    0.f, 3.402823466e38f, 1.41421356f, 0.70710678f, 1.12837917f, 0.63661977f, 0.31830989f,
    0.78539816f, 1.57079633f, 3.14159265f, 2.71828183f, 1.44269504f, 0.43429448f, 0.69314718f,
    2.30258509f, 6.28318531f, 0.52359878f, 0.30103000f, 3.32192809f, 0.86602540f,
};

static uint32_t rnd_next(Ctx *c) {
    uint32_t x = c->vfpu_ctrl[CTRL_RCX0] ? c->vfpu_ctrl[CTRL_RCX0] : 0x3F800001u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    c->vfpu_ctrl[CTRL_RCX0] = x;
    return x;
}

static void unimplemented(Ctx *c, uint32_t pc, uint32_t op) {
    char buf[64];
    snprintf(buf, sizeof buf, "VFPU op %08X not implemented", op);
    recomp_trap(c, pc, buf);
}

static void op_vfpu0_1_3(Ctx *c, uint32_t pc, uint32_t op) {
    int n = vsize(op), group = (int)(op >> 26), sub = (int)((op >> 23) & 7);
    float s[4], t[4], d[4] = {0};
    if (group == 0x19 && sub == 2) {  // vscl: t is a single scalar
        read_s(c, s, n, VS(op));
        float k = c->v[VT(op) & 0x7F].f;
        int tr[4];
        vec_regs(tr, 1, VT(op));
        k = c->v[tr[0]].f;
        for (int i = 0; i < n; i++) d[i] = s[i] * k;
        write_vec_d(c, d, n, VD(op));
        return;
    }
    read_s(c, s, n, VS(op));
    read_t(c, t, n, VT(op));
    if (group == 0x18) {
        for (int i = 0; i < n; i++)
            d[i] = sub == 0 ? s[i] + t[i] : sub == 1 ? s[i] - t[i] : s[i] / t[i];
        if (sub == 2) return unimplemented(c, pc, op);  // vsbn
    } else if (group == 0x19) {
        switch (sub) {
        case 0: for (int i = 0; i < n; i++) d[i] = s[i] * t[i]; break;  // vmul
        case 1: {  // vdot
            float sum = 0.f;
            for (int i = 0; i < n; i++) sum += s[i] * t[i];
            d[0] = sum;
            write_vec_d(c, d, 1, VD(op));
            return;
        }
        case 4: {  // vhdp: dot with the last s lane forced to 1
            float sum = 0.f;
            for (int i = 0; i < n; i++) sum += (i == n - 1 ? 1.f : s[i]) * t[i];
            d[0] = sum;
            write_vec_d(c, d, 1, VD(op));
            return;
        }
        default: return unimplemented(c, pc, op);
        }
    } else {  // 0x1B
        if (sub == 0) {  // vcmp
            int cond = (int)(op & 15), cc = 0, any = 0, all = 1;
            uint32_t affected = 0x30;
            for (int i = 0; i < n; i++) {
                float a = s[i], b = t[i];
                int r;
                switch (cond) {
                case 0: r = 0; break;
                case 1: r = a == b; break;
                case 2: r = a < b; break;
                case 3: r = a <= b; break;
                case 4: r = 1; break;
                case 5: r = a != b; break;
                case 6: r = a >= b; break;
                case 7: r = a > b; break;
                case 8: r = a == 0.f; break;
                case 9: r = isnan(a); break;
                case 10: r = isinf(a); break;
                case 11: r = isnan(a) || isinf(a); break;
                case 12: r = a != 0.f; break;
                case 13: r = !isnan(a); break;
                case 14: r = !isinf(a); break;
                default: r = !(isnan(a) || isinf(a)); break;
                }
                cc |= r << i;
                any |= r;
                all &= r;
                affected |= 1u << i;
            }
            uint32_t v = (uint32_t)cc | ((uint32_t)any << 4) | ((uint32_t)all << 5);
            c->vfpu_ctrl[CTRL_CC] = (c->vfpu_ctrl[CTRL_CC] & ~affected) | (v & affected);
            return;
        }
        for (int i = 0; i < n; i++) {
            switch (sub) {
            case 2: d[i] = fminf(s[i], t[i]); break;  // vmin
            case 3: d[i] = fmaxf(s[i], t[i]); break;  // vmax
            case 5: d[i] = s[i] > t[i] ? 1.f : s[i] < t[i] ? -1.f : 0.f; break;  // vscmp
            case 6: d[i] = s[i] >= t[i] ? 1.f : 0.f; break;  // vsge
            case 7: d[i] = s[i] < t[i] ? 1.f : 0.f; break;  // vslt
            default: return unimplemented(c, pc, op);
            }
        }
    }
    write_vec_d(c, d, n, VD(op));
}

static void op_vfpu4(Ctx *c, uint32_t pc, uint32_t op) {
    int n = vsize(op), fn = (int)((op >> 16) & 31);
    float s[4], d[4] = {0};
    switch (fn) {
    case 3: {  // vidt
        int lane = VD(op) & (n >= 3 ? 3 : 1);
        for (int i = 0; i < n; i++) d[i] = i == lane ? 1.f : 0.f;
        break;
    }
    case 6: case 7:  // vzero / vone
        for (int i = 0; i < n; i++) d[i] = fn == 7 ? 1.f : 0.f;
        break;
    default:
        read_s(c, s, n, VS(op));
        for (int i = 0; i < n; i++) {
            float x = s[i];
            switch (fn) {
            case 0: d[i] = x; break;  // vmov
            case 1: d[i] = fabsf(x); break;
            case 2: d[i] = -x; break;
            case 4: d[i] = clampf(x, 0.f, 1.f); break;  // vsat0
            case 5: d[i] = clampf(x, -1.f, 1.f); break;  // vsat1
            case 16: d[i] = 1.f / x; break;  // vrcp
            case 17: d[i] = 1.f / sqrtf(x); break;  // vrsq
            case 18: d[i] = qsin(x); break;
            case 19: d[i] = qcos(x); break;
            case 20: d[i] = exp2f(x); break;
            case 21: d[i] = log2f(x); break;
            case 22: d[i] = sqrtf(fabsf(x)); break;
            case 23: d[i] = asinf(x) / (float)M_PI_2; break;
            case 24: d[i] = -1.f / x; break;  // vnrcp
            case 26: d[i] = -qsin(x); break;  // vnsin
            case 28: d[i] = 1.f / exp2f(x); break;  // vrexp2
            default: return unimplemented(c, pc, op);
            }
        }
    }
    write_vec_d(c, d, n, VD(op));
}

static void op_vfpu7(Ctx *c, uint32_t pc, uint32_t op) {
    int n = vsize(op), fn = (int)((op >> 16) & 31);
    uint32_t s[4] = {0}, d[4] = {0};
    if (fn >= 1 && fn <= 3) {  // vrndi / vrndf1 / vrndf2
        for (int i = 0; i < n; i++) {
            uint32_t r = rnd_next(c);
            d[i] = fn == 1 ? r : (fn == 2 ? 0x3F800000u : 0x40000000u) | (r & 0x007FFFFFu);
        }
        write_ivec_d(c, d, n, VD(op));
        return;
    }
    if (fn == 0) {  // vrnds: seed
        c->vfpu_ctrl[CTRL_RCX0] = c->v[VS(op) & 0x7F].u | 1;
        return;
    }
    {
        float f[4];
        read_s(c, f, n, VS(op));
        memcpy(s, f, sizeof s);
    }
    int on = n;
    switch (fn) {
    case 24: {  // vuc2ifs
        uint32_t v = s[0];
        for (int i = 0; i < 4; i++, v >>= 8) d[i] = ((v & 0xFF) * 0x01010101u) >> 1;
        on = 4;
        break;
    }
    case 25:  // vc2i
        d[0] = (s[0] & 0xFF) << 24;
        d[1] = (s[0] & 0xFF00) << 16;
        d[2] = (s[0] & 0xFF0000) << 8;
        d[3] = s[0] & 0xFF000000u;
        on = 4;
        break;
    case 26: case 27: {  // vus2i / vs2i
        int in = n >= 2 ? 2 : 1;
        for (int i = 0; i < in; i++) {
            if (fn == 26) {
                d[i * 2] = (s[i] & 0xFFFF) << 15;
                d[i * 2 + 1] = (s[i] & 0xFFFF0000u) >> 1;
            } else {
                d[i * 2] = (s[i] & 0xFFFF) << 16;
                d[i * 2 + 1] = s[i] & 0xFFFF0000u;
            }
        }
        on = in * 2;
        break;
    }
    case 28: case 29:  // vi2uc / vi2c
        for (int i = 0; i < 4; i++) {
            int32_t v = (int32_t)s[i];
            uint32_t b = fn == 28 ? (uint32_t)((v < 0 ? 0 : v) >> 23) & 0xFF : (s[i] >> 24);
            d[0] |= b << (i * 8);
        }
        on = 1;
        break;
    case 30: case 31: {  // vi2us / vi2s
        int pairs = (n + 1) / 2;
        for (int i = 0; i < pairs; i++) {
            if (fn == 30) {
                int32_t lo = (int32_t)s[i * 2], hi = (int32_t)s[i * 2 + 1];
                lo = (lo < 0 ? 0 : lo) >> 15;
                hi = (hi < 0 ? 0 : hi) >> 15;
                d[i] = (uint32_t)lo | ((uint32_t)hi << 16);
            } else {
                d[i] = (s[i * 2] >> 16) | ((s[i * 2 + 1] >> 16) << 16);
            }
        }
        on = pairs;
        break;
    }
    case 19: {  // vh2f
        float f[4];
        on = n == 1 ? 2 : 4;
        for (int i = 0; i < on / 2; i++) {
            f[i * 2] = half_to_float((uint16_t)s[i]);
            f[i * 2 + 1] = half_to_float((uint16_t)(s[i] >> 16));
        }
        write_vec_d(c, f, on, VD(op));
        return;
    }
    default:
        return unimplemented(c, pc, op);
    }
    write_ivec_d(c, d, on, VD(op));
}

static void op_vfpu9(Ctx *c, uint32_t pc, uint32_t op) {
    int n = vsize(op), fn = (int)((op >> 16) & 31);
    float s[4], d[4] = {0};
    if (fn == 16) {  // vmfvc
        c->v[VD(op)].u = c->vfpu_ctrl[VS(op) & 15];
        return;
    }
    if (fn == 17) {  // vmtvc
        c->vfpu_ctrl[op & 15] = c->v[VS(op)].u;
        return;
    }
    read_s(c, s, n, VS(op));
    switch (fn) {
    case 2:  // vbfy1
        d[0] = s[0] + s[1]; d[1] = s[0] - s[1]; d[2] = s[2] + s[3]; d[3] = s[2] - s[3];
        break;
    case 3:  // vbfy2
        d[0] = s[0] + s[2]; d[1] = s[1] + s[3]; d[2] = s[0] - s[2]; d[3] = s[1] - s[3];
        break;
    case 4:  // vocp
        for (int i = 0; i < n; i++) d[i] = 1.f - s[i];
        break;
    case 6: case 7: {  // vfad / vavg
        float sum = 0.f;
        for (int i = 0; i < n; i++) sum += s[i];
        d[0] = fn == 6 ? sum : sum / (float)n;
        write_vec_d(c, d, 1, VD(op));
        return;
    }
    case 10:  // vsgn
        for (int i = 0; i < n; i++) d[i] = s[i] > 0.f ? 1.f : s[i] < 0.f ? -1.f : 0.f;
        break;
    default:
        return unimplemented(c, pc, op);
    }
    write_vec_d(c, d, n, VD(op));
}

static void op_vfpu4jump(Ctx *c, uint32_t pc, uint32_t op) {
    int sub = (int)((op >> 21) & 31), n = vsize(op);
    float s[4], d[4];
    switch (sub) {
    case 0: op_vfpu4(c, pc, op); return;
    case 1: op_vfpu7(c, pc, op); return;
    case 2: op_vfpu9(c, pc, op); return;
    case 3: {  // vcst
        float k = k_cst[(op >> 16) & 31];
        for (int i = 0; i < n; i++) d[i] = k;
        write_vec_d(c, d, n, VD(op));
        return;
    }
    case 16: case 17: case 18: case 19: {  // vf2in/iz/iu/id
        uint32_t r[4];
        double scale = ldexp(1.0, (int)((op >> 16) & 31));
        read_s(c, s, n, VS(op));
        for (int i = 0; i < n; i++) {
            if (isnan(s[i])) { r[i] = 0x7FFFFFFFu; continue; }
            double v = (double)s[i] * scale, q;
            if (v > 2147483647.0) { r[i] = 0x7FFFFFFFu; continue; }
            if (v <= -2147483648.0) { r[i] = 0x80000000u; continue; }
            switch (sub) {
            case 16: q = nearbyint(v); break;
            case 17: q = trunc(v); break;
            case 18: q = ceil(v); break;
            default: q = floor(v); break;
            }
            r[i] = (uint32_t)(int32_t)q;
        }
        write_ivec_d(c, r, n, VD(op));
        return;
    }
    case 20: {  // vi2f
        int rr[4];
        float scale = ldexpf(1.f, -(int)((op >> 16) & 31));
        vec_regs(rr, n, VS(op));
        float tmp[4] = {0};
        for (int i = 0; i < n; i++) memcpy(&tmp[i], &c->v[rr[i]].u, 4);
        apply_st(tmp, n, c->vfpu_ctrl[CTRL_SPFX]);
        for (int i = 0; i < n; i++) {
            int32_t iv;
            memcpy(&iv, &tmp[i], 4);
            d[i] = (float)iv * scale;
        }
        write_vec_d(c, d, n, VD(op));
        return;
    }
    case 21: {  // vcmov
        int tf = (int)((op >> 19) & 1), imm3 = (int)((op >> 16) & 7);
        uint32_t cc = c->vfpu_ctrl[CTRL_CC];
        read_s(c, s, n, VS(op));
        read_t(c, d, n, VD(op));
        for (int i = 0; i < n; i++) {
            int bit = imm3 < 6 ? imm3 : imm3 == 6 ? i : -1;
            if (bit >= 0 && (int)((cc >> bit) & 1) == !tf) d[i] = s[i];
        }
        write_vec_d(c, d, n, VD(op));
        return;
    }
    default:
        unimplemented(c, pc, op);
    }
}

static void op_vfpu6(Ctx *c, uint32_t pc, uint32_t op) {
    int sub = (int)((op >> 21) & 31), n = vsize(op);
    float s[16] = {0}, t[16] = {0}, d[16] = {0};
    if (sub < 4) {  // vmmul
        read_mtx(c, s, n, VS(op));
        read_mtx(c, t, n, VT(op));
        for (int a = 0; a < n; a++)
            for (int b = 0; b < n; b++) {
                float sum = 0.f;
                for (int k = 0; k < n; k++) sum += s[b * 4 + k] * t[a * 4 + k];
                d[a * 4 + b] = sum;
            }
        write_mtx(c, d, n, VD(op));
        return;
    }
    if (sub < 16) {  // vtfm2/3/4 and vhtfm2/3/4
        int ins = (int)((op >> 23) & 3);  // matrix side - 1
        int m = ins + 1;
        read_mtx(c, s, m, VS(op));
        float v[4] = {0};
        read_vec(c, v, n, VT(op));
        if (n < m) v[n] = 1.f;  // homogeneous variant: missing lane is 1
        for (int i = 0; i < m; i++) {
            float sum = 0.f;
            for (int k = 0; k < m; k++) sum += s[i * 4 + k] * v[k];
            d[i] = sum;
        }
        write_vec_d(c, d, m, VD(op));
        return;
    }
    if (sub < 20) {  // vmscl
        float k = c->v[VT(op)].f;
        int tr[4];
        vec_regs(tr, 1, VT(op));
        k = c->v[tr[0]].f;
        read_mtx(c, s, n, VS(op));
        for (int i = 0; i < 16; i++) d[i] = s[i] * k;
        write_mtx(c, d, n, VD(op));
        return;
    }
    if (sub < 24) {  // vcrsp.t / vqmul.q
        float a[4], b[4];
        read_s(c, a, n, VS(op));
        read_vec(c, b, n, VT(op));
        if (n == 3) {
            d[0] = a[1] * b[2] - a[2] * b[1];
            d[1] = a[2] * b[0] - a[0] * b[2];
            d[2] = a[0] * b[1] - a[1] * b[0];
        } else if (n == 4) {
            d[0] = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
            d[1] = -a[0] * b[2] + a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
            d[2] = a[0] * b[1] - a[1] * b[0] + a[2] * b[3] + a[3] * b[2];
            d[3] = -a[0] * b[0] - a[1] * b[1] - a[2] * b[2] + a[3] * b[3];
        } else {
            return unimplemented(c, pc, op);
        }
        write_vec_d(c, d, n, VD(op));
        return;
    }
    if (sub == 28) {
        int fn = (int)((op >> 16) & 15);
        if (fn == 0) {  // vmmov
            read_mtx(c, s, n, VS(op));
            write_mtx(c, s, n, VD(op));
            return;
        }
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++)
                d[j * 4 + i] = fn == 3 ? (i == j ? 1.f : 0.f) : fn == 7 ? 1.f : 0.f;
        if (fn != 3 && fn != 6 && fn != 7) return unimplemented(c, pc, op);
        write_mtx(c, d, n, VD(op));
        return;
    }
    if (sub == 29) {  // vrot
        int imm = (int)((op >> 16) & 31), sin_lane = (imm >> 2) & 3, cos_lane = imm & 3;
        int sr[1];
        vec_regs(sr, 1, VS(op));
        float ang = c->v[sr[0]].f;
        float sn = qsin(ang), cs = qcos(ang);
        if (imm & 0x10) sn = -sn;
        float r[4] = {0};
        if (sin_lane == cos_lane)
            for (int i = 0; i < 4; i++) r[i] = sn;
        else
            r[sin_lane] = sn;
        r[cos_lane] = cs;
        write_vec_d(c, r, n, VD(op));
        return;
    }
    unimplemented(c, pc, op);
}

static void op_loadstore(Ctx *c, uint32_t pc, uint32_t op) {
    uint32_t base = c->r[(op >> 21) & 31];
    int32_t off = (int16_t)(op & 0xFFFC);
    uint32_t addr = base + (uint32_t)off;
    switch (op >> 26) {
    case 0x32:  // lv.s
        c->v[((op >> 16) & 0x1F) | ((op & 3) << 5)].u = mem_r32(addr);
        return;
    case 0x3A:  // sv.s
        mem_w32(addr, c->v[((op >> 16) & 0x1F) | ((op & 3) << 5)].u);
        return;
    }
    int vt = (int)(((op >> 16) & 0x1F) | ((op & 1) << 5)), r[4];
    vec_regs(r, 4, vt);
    switch (op >> 26) {
    case 0x36:  // lv.q
        for (int i = 0; i < 4; i++) c->v[r[i]].u = mem_r32(addr + 4 * (uint32_t)i);
        return;
    case 0x3E:  // sv.q
        for (int i = 0; i < 4; i++) mem_w32(addr + 4 * (uint32_t)i, c->v[r[i]].u);
        return;
    case 0x35: {  // lvl.q / lvr.q
        int k = (int)((addr >> 2) & 3);
        if (!(op & 2))
            for (int i = 0; i <= k; i++) c->v[r[3 - i]].u = mem_r32(addr - 4 * (uint32_t)i);
        else
            for (int i = 0; i <= 3 - k; i++) c->v[r[i]].u = mem_r32(addr + 4 * (uint32_t)i);
        return;
    }
    case 0x3D: {  // svl.q / svr.q
        int k = (int)((addr >> 2) & 3);
        if (!(op & 2))
            for (int i = 0; i <= k; i++) mem_w32(addr - 4 * (uint32_t)i, c->v[r[3 - i]].u);
        else
            for (int i = 0; i <= 3 - k; i++) mem_w32(addr + 4 * (uint32_t)i, c->v[r[i]].u);
        return;
    }
    }
    unimplemented(c, pc, op);
}

void vfpu_exec(Ctx *c, uint32_t pc, uint32_t op) {
    switch (op >> 26) {
    case 0x12: {  // mfv/mfvc/mtv/mtvc
        uint32_t imm = op & 0xFF, rt = (op >> 16) & 31, kind = (op >> 21) & 31;
        if (kind == 3) {
            uint32_t v = imm < 128 ? c->v[imm].u : c->vfpu_ctrl[(imm - 128) & 15];
            if (rt) c->r[rt] = v;
        } else if (kind == 7) {
            if (imm < 128) c->v[imm].u = c->r[rt];
            else c->vfpu_ctrl[(imm - 128) & 15] = c->r[rt];
        } else {
            unimplemented(c, pc, op);
        }
        return;  // transfers don't consume prefixes
    }
    case 0x32: case 0x35: case 0x36: case 0x3A: case 0x3D: case 0x3E:
        op_loadstore(c, pc, op);
        return;
    case 0x37: {
        int sub = (int)((op >> 23) & 7);
        if (sub < 6) {  // vpfxs/t/d
            int which = sub >> 1;
            c->vfpu_ctrl[which] = op & (which == CTRL_DPFX ? 0xFFFu : 0xFFFFFu);
            return;
        }
        float f = sub == 6 ? (float)(int16_t)(op & 0xFFFF) : half_to_float((uint16_t)op);
        int r[1];
        vec_regs(r, 1, VT(op));
        float d[1] = {f};
        write_vec_d(c, d, 1, VT(op));
        (void)r;
        break;
    }
    case 0x18: case 0x19: case 0x1B: op_vfpu0_1_3(c, pc, op); break;
    case 0x34: op_vfpu4jump(c, pc, op); break;
    case 0x3C: op_vfpu6(c, pc, op); break;
    case 0x3F: return;  // vnop / vsync / vflush
    default: unimplemented(c, pc, op);
    }
    eat_prefixes(c);
}

void vfpu_reset(Ctx *c) { eat_prefixes(c); }
