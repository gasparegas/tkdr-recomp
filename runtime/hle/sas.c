// sceSasCore: Sony's software mixer. 32 voices play VAG (PSP ADPCM) samples or
// noise through an ADSR envelope, resampled by pitch, into a stereo grain.
#include <stdlib.h>

#include "hle.h"

#define NVOICES 32
#define PITCH_BASE 0x1000
#define ENV_MAX 0x40000000LL

enum { CURVE_LIN_INC, CURVE_LIN_DEC, CURVE_LIN_BENT, CURVE_EXP_DEC, CURVE_EXP_INC, CURVE_DIRECT };
enum { ST_OFF, ST_KEYON, ST_KEYON_STEP, ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE };

typedef struct {
    int64_t height;
    int state;
    int rate[4], type[4];  // attack, decay, sustain, release
    int64_t sustain_level;
} Envelope;

typedef struct {
    // VAG stream
    uint32_t vag_addr, vag_size, read;
    int loop_enabled, loop_next, loop_block, block, nblocks, ended;
    int16_t samples[28];
    int cur, s1, s2;
    // voice
    int noise, noise_freq;
    uint32_t noise_state;
    int on, playing, paused;
    int pitch, vol_l, vol_r, eff_l, eff_r;
    uint32_t frac;
    int16_t prev, next;
    Envelope env;
} Voice;

static Voice g_v[NVOICES];
static int g_grain = 256, g_out_mode, g_dry = 1, g_wet;
static int32_t g_mix[2048 * 2], g_send[2048 * 2];

// PSP VAG prediction filters (the PS1 set extended to 16 entries)
static const int k_filter[16][2] = {
    {0, 0}, {60, 0}, {115, 52}, {98, 55}, {122, 60}, {0, 0}, {0, 0}, {52, 0},
    {55, 2}, {60, 125}, {0, 0}, {0, 91}, {0, 0}, {2, 216}, {125, 6}, {0, 151},
};

static int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

static void vag_start(Voice *v) {
    v->read = v->vag_addr;
    v->nblocks = (int)(v->vag_size / 16);
    v->block = -1;
    v->loop_next = 0;
    v->loop_block = -1;
    v->ended = 0;
    v->cur = 28;
    v->s1 = v->s2 = 0;
}

static void vag_decode_block(Voice *v) {
    if (v->block == v->nblocks - 1) { v->ended = 1; return; }
    uint32_t p = v->read;
    int hdr = mem_r8(p), flags = mem_r8(p + 1);
    int shift = hdr & 15, pred = hdr >> 4;
    if (flags == 7) { v->ended = 1; return; }
    if (flags == 6) v->loop_block = v->block;
    else if (flags == 3 && v->loop_enabled) v->loop_next = 1;
    int c1 = k_filter[pred][0], c2 = -k_filter[pred][1], s1 = v->s1, s2 = v->s2;
    for (int i = 0; i < 28; i += 2) {
        int d = mem_r8(p + 2 + (uint32_t)i / 2);
        int a = (int16_t)((d & 0xF) << 12) >> shift, b = (int16_t)((d & 0xF0) << 8) >> shift;
        s2 = clamp16(a + ((s1 * c1 + s2 * c2) >> 6));
        s1 = clamp16(b + ((s2 * c1 + s1 * c2) >> 6));
        v->samples[i] = (int16_t)s2;
        v->samples[i + 1] = (int16_t)s1;
    }
    v->s1 = s1;
    v->s2 = s2;
    v->cur = 0;
    v->block++;
    v->read = p + 16;
}

static int16_t next_sample(Voice *v) {
    if (v->noise) {
        // LFSR noise; the frequency setting only changes how often it advances
        v->noise_state = v->noise_state * 1103515245u + 12345u;
        return (int16_t)(v->noise_state >> 16);
    }
    if (v->ended) return 0;
    if (v->cur == 28) {
        if (v->loop_next && v->loop_block >= 0) {
            v->read = v->vag_addr + 16 * (uint32_t)v->loop_block + 16;
            v->block = v->loop_block;
            v->loop_next = 0;
        }
        vag_decode_block(v);
        if (v->ended) return 0;
    }
    return v->samples[v->cur++];
}

static void walk(Envelope *e, int type, int rate) {
    int64_t d;
    switch (type) {
    case CURVE_LIN_INC: e->height += rate; break;
    case CURVE_LIN_DEC: e->height -= rate; break;
    case CURVE_LIN_BENT: e->height += e->height <= ENV_MAX * 3 / 4 ? rate : rate / 4; break;
    case CURVE_EXP_DEC:
        d = e->height - ENV_MAX;
        d += (-d * (int64_t)(uint32_t)rate) >> 32;
        e->height = d + ENV_MAX - ((int64_t)(uint32_t)rate + 3) / 4;
        break;
    case CURVE_EXP_INC:
        d = e->height - ENV_MAX;
        d += (-d * (int64_t)(uint32_t)rate) >> 32;
        e->height = d + 0x4000 + ENV_MAX;
        break;
    default: e->height = rate; break;
    }
}

static void set_state(Envelope *e, int s) {
    if (e->height > ENV_MAX) e->height = ENV_MAX;
    e->state = s;
}

static void env_step(Envelope *e) {
    switch (e->state) {
    case ST_ATTACK:
        walk(e, e->type[0], e->rate[0]);
        if (e->height >= ENV_MAX || e->height < 0) set_state(e, ST_DECAY);
        break;
    case ST_DECAY:
        walk(e, e->type[1], e->rate[1]);
        if (e->height < e->sustain_level) set_state(e, ST_SUSTAIN);
        break;
    case ST_SUSTAIN:
        walk(e, e->type[2], e->rate[2]);
        if (e->height <= 0) { e->height = 0; set_state(e, ST_RELEASE); }
        break;
    case ST_RELEASE:
        walk(e, e->type[3], e->rate[3]);
        if (e->height <= 0) { e->height = 0; set_state(e, ST_OFF); }
        break;
    case ST_KEYON:
        e->height = 0;
        set_state(e, ST_KEYON_STEP);
        break;
    case ST_KEYON_STEP:
        if (++e->height >= 31) { e->height = 0; set_state(e, ST_ATTACK); }
        break;
    }
}

static void mix_voice(Voice *v) {
    for (int i = 0; i < g_grain; i++) {
        int s = v->prev + (((v->next - v->prev) * (int)(v->frac & 0xFFF)) >> 12);
        v->frac += (uint32_t)v->pitch;
        while (v->frac >= PITCH_BASE) {
            v->frac -= PITCH_BASE;
            v->prev = v->next;
            v->next = next_sample(v);
        }
        int64_t h = v->env.height > ENV_MAX ? ENV_MAX : v->env.height < 0 ? 0 : v->env.height;
        env_step(&v->env);
        int env = (int)((h + (1 << 14)) >> 15);
        s = (s * env + (1 << 14)) >> 15;
        g_mix[i * 2] += (s * v->vol_l) >> 12;
        g_mix[i * 2 + 1] += (s * v->vol_r) >> 12;
        g_send[i * 2] += (s * v->eff_l) >> 12;
        g_send[i * 2 + 1] += (s * v->eff_r) >> 12;
    }
    if (!v->noise && v->ended) set_state(&v->env, ST_OFF), v->env.height = 0;
    if (v->env.state == ST_OFF) v->playing = v->on = 0;
}

static Voice *voice(Ctx *c) {
    int n = (int)ARG(1);
    return n >= 0 && n < NVOICES ? &g_v[n] : NULL;
}
#define VOICE_OR_FAIL                                    \
    Voice *v = voice(c);                                 \
    if (!v) { RET(0x80420010u); return; }

// ---------------------------------------------------------------- HLE

void __sceSasInit(Ctx *c) {
    g_grain = (int)ARG(1);
    if (g_grain > 2048) g_grain = 2048;
    g_out_mode = (int)ARG(3);
    memset(g_v, 0, sizeof g_v);
    for (int i = 0; i < NVOICES; i++) {
        g_v[i].pitch = PITCH_BASE;
        g_v[i].vol_l = g_v[i].vol_r = g_v[i].eff_l = g_v[i].eff_r = 0x1000;
        g_v[i].noise_state = 0x1234 + (uint32_t)i;
        Envelope *e = &g_v[i].env;
        e->type[0] = CURVE_LIN_INC;
        e->type[1] = e->type[2] = e->type[3] = CURVE_LIN_DEC;
    }
    RET(0);
}

static void core_mix(uint32_t out, uint32_t in, int lvol, int rvol) {
    for (int i = 0; i < NVOICES; i++)
        if (g_v[i].playing && !g_v[i].paused) mix_voice(&g_v[i]);
    for (int i = 0; i < g_grain * 2; i += 2) {
        int l = 0, r = 0;
        if (in) {
            l = ((int16_t)mem_r16(in + (uint32_t)i * 2) * lvol) >> 12;
            r = ((int16_t)mem_r16(in + (uint32_t)i * 2 + 2) * rvol) >> 12;
        }
        if (g_dry) { l += g_mix[i]; r += g_mix[i + 1]; }
        if (g_wet) { l += g_send[i] / 2; r += g_send[i + 1] / 2; }  // no reverb yet: plain send
        mem_w16(out + (uint32_t)i * 2, (uint16_t)clamp16(l));
        mem_w16(out + (uint32_t)i * 2 + 2, (uint16_t)clamp16(r));
    }
    memset(g_mix, 0, sizeof(int32_t) * (size_t)g_grain * 2);
    memset(g_send, 0, sizeof(int32_t) * (size_t)g_grain * 2);
}

void __sceSasCore(Ctx *c) {
    core_mix(ARG(1), 0, 0, 0);
    RET(0);
}
void __sceSasCoreWithMix(Ctx *c) {
    core_mix(ARG(1), ARG(1), (int)ARG(2), (int)ARG(3));
    RET(0);
}

void __sceSasGetEndFlag(Ctx *c) {
    uint32_t f = 0;
    for (int i = 0; i < NVOICES; i++)
        if (!g_v[i].playing) f |= 1u << i;
    RET(f);
}

void __sceSasSetVoice(Ctx *c) {
    VOICE_OR_FAIL
    v->vag_addr = ARG(2);
    v->vag_size = ARG(3);
    v->loop_enabled = (int)ARG(4);
    v->noise = 0;
    RET(0);
}

void __sceSasSetNoise(Ctx *c) {
    VOICE_OR_FAIL
    v->noise = 1;
    v->noise_freq = (int)ARG(2);
    RET(0);
}

void __sceSasSetVolume(Ctx *c) {
    VOICE_OR_FAIL
    v->vol_l = (int)ARG(2);
    v->vol_r = (int)ARG(3);
    v->eff_l = (int)ARG(4);
    v->eff_r = (int)ARG(5);
    RET(0);
}

void __sceSasSetPitch(Ctx *c) {
    VOICE_OR_FAIL
    v->pitch = (int)ARG(2) & 0x7FFF;
    RET(0);
}

void __sceSasSetKeyOn(Ctx *c) {
    VOICE_OR_FAIL
    if (!v->noise) vag_start(v);
    v->frac = 0;
    v->prev = v->next = 0;
    v->on = v->playing = 1;
    v->paused = 0;
    set_state(&v->env, ST_KEYON);
    RET(0);
}

void __sceSasSetKeyOff(Ctx *c) {
    VOICE_OR_FAIL
    if (v->playing) set_state(&v->env, ST_RELEASE);
    RET(0);
}

void __sceSasSetPause(Ctx *c) {
    for (int i = 0; i < NVOICES; i++)
        if (ARG(1) & (1u << i)) g_v[i].paused = (int)ARG(2);
    RET(0);
}
void __sceSasGetPauseFlag(Ctx *c) {
    uint32_t f = 0;
    for (int i = 0; i < NVOICES; i++)
        if (g_v[i].paused) f |= 1u << i;
    RET(f);
}

// (core, voice, flag, a, d, s, r): rates
void __sceSasSetADSR(Ctx *c) {
    VOICE_OR_FAIL
    for (int k = 0; k < 4; k++)
        if (ARG(2) & (1u << k)) v->env.rate[k] = (int)ARG(3 + k);
    RET(0);
}
// (core, voice, flag, a, d, s, r): curve types
void __sceSasSetADSRmode(Ctx *c) {
    VOICE_OR_FAIL
    for (int k = 0; k < 4; k++)
        if (ARG(2) & (1u << k)) v->env.type[k] = (int)ARG(3 + k);
    RET(0);
}
void __sceSasSetSL(Ctx *c) {
    VOICE_OR_FAIL
    v->env.sustain_level = (int64_t)ARG(2);
    RET(0);
}
void __sceSasGetEnvelopeHeight(Ctx *c) {
    VOICE_OR_FAIL
    RET((uint32_t)(v->env.height > ENV_MAX ? ENV_MAX : v->env.height < 0 ? 0 : v->env.height));
}

static int simple_rate(int n, int shift) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    int r = ((7 - (n & 3)) << shift) >> (n >> 2);
    return r ? r : 1;
}

// The packed ADSR format used by most games.
void __sceSasSetSimpleADSR(Ctx *c) {
    VOICE_OR_FAIL
    uint32_t e1 = ARG(2), e2 = ARG(3);
    Envelope *e = &v->env;
    e->rate[0] = simple_rate((int)(e1 >> 8), 26);
    e->type[0] = (e1 & 0x8000) ? CURVE_LIN_BENT : CURVE_LIN_INC;
    int dn = (int)((e1 >> 4) & 15);
    e->rate[1] = dn ? (int)(0x80000000u >> dn) : 0x7FFFFFFF;
    e->type[1] = CURVE_EXP_DEC;
    int st = (int)((e2 >> 14) & 3);
    e->type[2] = st;
    e->rate[2] = st == CURVE_EXP_DEC ? simple_rate((int)(e2 >> 6), 24) : simple_rate((int)(e2 >> 6), 26);
    e->type[3] = (e2 & 0x20) ? CURVE_EXP_DEC : CURVE_LIN_DEC;
    int rn = (int)(e2 & 31);
    if (rn == 31) e->rate[3] = 0;
    else if (e->type[3] == CURVE_LIN_DEC) e->rate[3] = rn == 30 ? 0x40000000 : rn == 29 ? 1 : (0x10000000 >> rn);
    else e->rate[3] = rn ? (int)(0x80000000u >> rn) : 0x7FFFFFFF;
    e->sustain_level = (int64_t)(((e1 & 15) + 1) << 26);
    RET(0);
}

// Reverb is not emulated: wet output is the plain effect send.
void __sceSasRevType(Ctx *c) { RET(0); }
void __sceSasRevParam(Ctx *c) { RET(0); }
void __sceSasRevEVOL(Ctx *c) { RET(0); }
void __sceSasRevVON(Ctx *c) {
    g_dry = ARG(1) != 0;
    g_wet = ARG(2) != 0;
    RET(0);
}
