// sceAudio output channels and the sceSasCore mixer (silent for now).
// Blocking output paces the calling thread at 44.1 kHz; samples go to a ring
// buffer that the frontend's audio callback drains.
#include <pthread.h>
#include <stdlib.h>

#include "hle.h"

#define NCH 8
#define RING 32768  // stereo frames

typedef struct {
    int reserved;
    uint32_t samples, format;  // format 0x00 stereo, 0x10 mono
    uint32_t lvol, rvol;
    uint64_t busy_until;  // when the hardware will have played what we queued
} Channel;

static Channel g_ch[NCH];
static int16_t g_ring[RING][2];
static uint32_t g_ring_w, g_ring_r;
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;

// Called by the frontend audio callback: interleaved stereo s16.
void audio_pull(int16_t *out, int frames) {
    pthread_mutex_lock(&g_ring_lock);
    for (int i = 0; i < frames; i++) {
        if (g_ring_r != g_ring_w) {
            out[i * 2] = g_ring[g_ring_r % RING][0];
            out[i * 2 + 1] = g_ring[g_ring_r % RING][1];
            g_ring_r++;
        } else {
            out[i * 2] = out[i * 2 + 1] = 0;
        }
    }
    pthread_mutex_unlock(&g_ring_lock);
}

static void mix_into_ring(const Channel *ch, uint32_t buf, uint32_t lvol, uint32_t rvol) {
    pthread_mutex_lock(&g_ring_lock);
    // drop old audio if the consumer fell behind (or there is none)
    if (g_ring_w - g_ring_r > RING - ch->samples) g_ring_r = g_ring_w - (RING - ch->samples);
    int mono = ch->format == 0x10;
    for (uint32_t i = 0; i < ch->samples; i++) {
        int32_t l, r;
        if (mono) l = r = (int16_t)mem_r16(buf + i * 2);
        else {
            l = (int16_t)mem_r16(buf + i * 4);
            r = (int16_t)mem_r16(buf + i * 4 + 2);
        }
        l = l * (int32_t)lvol / 0x8000;
        r = r * (int32_t)rvol / 0x8000;
        int16_t *slot = g_ring[(g_ring_w + i) % RING];
        slot[0] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        slot[1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    g_ring_w += ch->samples;  // TODO: mix concurrent channels instead of queueing them
    pthread_mutex_unlock(&g_ring_lock);
}

// Queue a buffer, then block until the previous one has "played".
static void output(Ctx *c, int ch_id, uint32_t lvol, uint32_t rvol, uint32_t buf, int blocking) {
    if (ch_id < 0 || ch_id >= NCH || !g_ch[ch_id].reserved) { RET(0x80260002u); return; }
    Channel *ch = &g_ch[ch_id];
    uint64_t now = hle_time_us();
    if (!blocking && ch->busy_until > now) { RET(0x80260009u); return; }  // busy
    if (buf) mix_into_ring(ch, buf, lvol, rvol);
    uint64_t dur = (uint64_t)ch->samples * 1000000u / 44100u;
    uint64_t start = ch->busy_until > now ? ch->busy_until : now;
    ch->busy_until = start + dur;
    RET(ch->samples);
    // the hardware keeps one buffer queued: wait until there is room again
    if (blocking && start > now) {
        uint32_t ret = c->r[2];
        thread_delay(c, start - now, 0);
        RET(ret);
    }
}

void sceAudioChReserve(Ctx *c) {
    int ch = (int)ARG(0);
    if (ch < 0)
        for (int k = NCH - 1; k >= 0; k--)
            if (!g_ch[k].reserved) { ch = k; break; }
    if (ch < 0 || ch >= NCH || g_ch[ch].reserved) { RET(0x80260002u); return; }
    g_ch[ch] = (Channel){.reserved = 1, .samples = ARG(1), .format = ARG(2)};
    RET(ch);
}
void sceAudioChRelease(Ctx *c) {
    if (ARG(0) < NCH) g_ch[ARG(0)].reserved = 0;
    RET(0);
}
void sceAudioSetChannelDataLen(Ctx *c) {
    if (ARG(0) < NCH) g_ch[ARG(0)].samples = ARG(1);
    RET(0);
}
void sceAudioChangeChannelConfig(Ctx *c) {
    if (ARG(0) < NCH) g_ch[ARG(0)].format = ARG(1);
    RET(0);
}
void sceAudioChangeChannelVolume(Ctx *c) {
    if (ARG(0) < NCH) { g_ch[ARG(0)].lvol = ARG(1); g_ch[ARG(0)].rvol = ARG(2); }
    RET(0);
}
void sceAudioGetChannelRestLength(Ctx *c) {
    if (ARG(0) >= NCH) { RET(0x80260002u); return; }
    Channel *ch = &g_ch[ARG(0)];
    uint64_t now = hle_time_us();
    RET(ch->busy_until > now ? (uint32_t)((ch->busy_until - now) * 44100u / 1000000u) : 0);
}
void sceAudioOutputBlocking(Ctx *c) { output(c, (int)ARG(0), ARG(1), ARG(1), ARG(2), 1); }
void sceAudioOutputPannedBlocking(Ctx *c) { output(c, (int)ARG(0), ARG(1), ARG(2), ARG(3), 1); }
void sceAudioOutputPanned(Ctx *c) { output(c, (int)ARG(0), ARG(1), ARG(2), ARG(3), 0); }
