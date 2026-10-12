// sceAudio output channels. Blocking output paces the calling thread at
// 44.1 kHz; every channel has its own ring buffer and the consumer (the
// frontend's audio callback, or the dump clock in headless mode) mixes them.
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "hle.h"

#define NCH 8
#define RING 16384  // stereo frames per channel

typedef struct {
    int reserved;
    uint32_t samples, format;  // format 0x00 stereo, 0x10 mono
    uint32_t lvol, rvol;
    uint64_t busy_until;  // when the hardware will have played what we queued
    int16_t ring[RING][2];
    uint32_t w, r;
} Channel;

static Channel g_ch[NCH];
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;

static int16_t clamp_s16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

// Mix all channels into interleaved stereo s16.
void audio_pull(int16_t *out, int frames) {
    pthread_mutex_lock(&g_ring_lock);
    for (int i = 0; i < frames; i++) {
        int32_t l = 0, r = 0;
        for (int k = 0; k < NCH; k++) {
            Channel *ch = &g_ch[k];
            if (ch->r == ch->w) continue;
            l += ch->ring[ch->r % RING][0];
            r += ch->ring[ch->r % RING][1];
            ch->r++;
        }
        out[i * 2] = clamp_s16(l);
        out[i * 2 + 1] = clamp_s16(r);
    }
    pthread_mutex_unlock(&g_ring_lock);
}

// AUDIO_DUMP=file.raw (headless): consume audio at the guest clock's pace and
// write it out as raw s16 stereo 44.1 kHz.
static void *dump_clock(void *arg) {
    FILE *f = arg;
    uint64_t pulled = 0;
    int16_t buf[4096 * 2];
    for (;;) {
        usleep(5000);
        uint64_t target = hle_time_us() * 44100u / 1000000u;
        while (pulled < target) {
            int n = target - pulled > 4096 ? 4096 : (int)(target - pulled);
            audio_pull(buf, n);
            fwrite(buf, 4, (size_t)n, f);
            pulled += (uint64_t)n;
        }
        fflush(f);
    }
    return NULL;
}

void audio_start_dump(void) {
    const char *path = getenv("AUDIO_DUMP");
    FILE *f = path ? fopen(path, "wb") : NULL;
    if (!f) return;
    pthread_t t;
    pthread_create(&t, NULL, dump_clock, f);
    pthread_detach(t);
}

static void mix_into_ring(Channel *ch, uint32_t buf, uint32_t lvol, uint32_t rvol) {
    pthread_mutex_lock(&g_ring_lock);
    // drop old audio if the consumer fell behind (or there is none)
    if (ch->w - ch->r > RING - ch->samples) ch->r = ch->w - (RING - ch->samples);
    int mono = ch->format == 0x10;
    for (uint32_t i = 0; i < ch->samples; i++) {
        int32_t l, r;
        if (mono) l = r = (int16_t)mem_r16(buf + i * 2);
        else {
            l = (int16_t)mem_r16(buf + i * 4);
            r = (int16_t)mem_r16(buf + i * 4 + 2);
        }
        int16_t *slot = ch->ring[(ch->w + i) % RING];
        slot[0] = clamp_s16(l * (int32_t)lvol / 0x8000);
        slot[1] = clamp_s16(r * (int32_t)rvol / 0x8000);
    }
    ch->w += ch->samples;
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
    memset(&g_ch[ch], 0, sizeof g_ch[ch]);
    g_ch[ch].reserved = 1;
    g_ch[ch].samples = ARG(1);
    g_ch[ch].format = ARG(2);
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
