// Shared definitions between generated code and the runtime.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef union {
    float f;
    uint32_t u;
    int32_t s;
} FReg;

typedef struct Ctx {
    uint32_t r[32];
    uint32_t hi, lo;
    FReg f[32];
    uint32_t fcr31;
    uint32_t fcc;  // FPU condition bit
    // VFPU: 128 single-precision registers + control registers
    FReg v[128];
    uint32_t vfpu_ctrl[16];  // [0..2] prefixes s/t/d, [3] cc, ...
    uint32_t ic;  // interrupt-enable flag (mfic/mtic)
} Ctx;

typedef void (*RecompFunc)(Ctx *);

// Guest memory: one flat buffer covering 0x00000000-0x0A000000. Masking the
// top bits folds the cached/uncached/kernel mirrors onto the same bytes.
extern uint8_t *g_mem;
#define GUEST_MEM_SIZE 0x0A000000u

static inline uint8_t *host(uint32_t a) { return g_mem + (a & 0x1FFFFFFFu); }

static inline uint32_t mem_r32(uint32_t a) { uint32_t v; memcpy(&v, host(a), 4); return v; }
static inline uint16_t mem_r16(uint32_t a) { uint16_t v; memcpy(&v, host(a), 2); return v; }
static inline uint8_t mem_r8(uint32_t a) { return *host(a); }
static inline void mem_w32(uint32_t a, uint32_t v) { memcpy(host(a), &v, 4); }
static inline void mem_w16(uint32_t a, uint16_t v) { memcpy(host(a), &v, 2); }
static inline void mem_w8(uint32_t a, uint8_t v) { *host(a) = v; }

// Unaligned word access (lwl/lwr/swl/swr), little-endian semantics.
static inline uint32_t do_lwl(uint32_t rt, uint32_t a) {
    uint32_t sh = (a & 3) * 8, w = mem_r32(a & ~3u);
    return (rt & (0x00FFFFFFu >> sh)) | (w << (24 - sh));
}
static inline uint32_t do_lwr(uint32_t rt, uint32_t a) {
    uint32_t sh = (a & 3) * 8, w = mem_r32(a & ~3u);
    return (rt & ~(0xFFFFFFFFu >> sh)) | (w >> sh);
}
static inline void do_swl(uint32_t rt, uint32_t a) {
    uint32_t sh = (a & 3) * 8, w = mem_r32(a & ~3u);
    mem_w32(a & ~3u, (w & ~(0xFFFFFFFFu >> (24 - sh))) | (rt >> (24 - sh)));
}
static inline void do_swr(uint32_t rt, uint32_t a) {
    uint32_t sh = (a & 3) * 8, w = mem_r32(a & ~3u);
    mem_w32(a & ~3u, (w & ~(0xFFFFFFFFu << sh)) | (rt << sh));
}

static inline void do_div(Ctx *c, int32_t a, int32_t b) {
    if (b == 0) { c->lo = a < 0 ? 1 : 0xFFFFFFFFu; c->hi = (uint32_t)a; }
    else if (a == INT32_MIN && b == -1) { c->lo = (uint32_t)a; c->hi = 0; }
    else { c->lo = (uint32_t)(a / b); c->hi = (uint32_t)(a % b); }
}
static inline void do_divu(Ctx *c, uint32_t a, uint32_t b) {
    if (b == 0) { c->lo = a <= 0xFFFFu ? 0xFFFFu : 0xFFFFFFFFu; c->hi = a; }
    else { c->lo = a / b; c->hi = a % b; }
}
static inline void acc_add(Ctx *c, int64_t v) {
    int64_t acc = (int64_t)(((uint64_t)c->hi << 32) | c->lo) + v;
    c->hi = (uint32_t)((uint64_t)acc >> 32); c->lo = (uint32_t)acc;
}

static inline uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
static inline uint32_t bitrev32(uint32_t x) {
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4);
    return __builtin_bswap32(x);
}

// float -> int32 with MIPS saturation; mode 0 nearest, 1 trunc, 2 ceil, 3 floor
static inline int32_t f2i(float f, int mode) {
    if (isnan(f)) return 0x7FFFFFFF;
    double d = mode == 0 ? nearbyint(f) : mode == 1 ? trunc(f) : mode == 2 ? ceil(f) : floor(f);
    if (d >= 2147483647.0) return 0x7FFFFFFF;
    if (d <= -2147483648.0) return (int32_t)0x80000000;
    return (int32_t)d;
}
static inline int fcmp(float a, float b, int cond) {
    int un = isnan(a) || isnan(b);
    return ((cond & 1) && un) || ((cond & 2) && !un && a == b) || ((cond & 4) && !un && a < b);
}

// Provided by the runtime.
void call_indirect(Ctx *c, uint32_t addr);
void hle_call(Ctx *c, uint32_t import_index);
void vfpu_exec(Ctx *c, uint32_t pc, uint32_t insn);
void recomp_trap(Ctx *c, uint32_t pc, const char *why);
