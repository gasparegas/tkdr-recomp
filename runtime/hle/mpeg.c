// sceMpeg (PSMF movie playback). The game fills a ring of 2048-byte MPEG-PS
// packets through its own callback; packets are demuxed as they arrive into a
// video elementary stream (H.264, decoded with FFmpeg in avc.c) and ATRAC3+
// audio frames. Ring space is given back as the video is consumed.
#include <stdlib.h>

#include "hle.h"

#define MPEG_ERR_NO_DATA 0x80618001u
#define PACKET 2048u

void hle_call_guest(Ctx *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2);

// SceMpegRingbuffer field offsets
enum { RB_PACKETS = 0x00, RB_READ = 0x04, RB_WRITTEN = 0x08, RB_FREE = 0x0C, RB_PSIZE = 0x10,
       RB_DATA = 0x14, RB_CB = 0x18, RB_CBARG = 0x1C, RB_UPPER = 0x20, RB_MPEG = 0x28 };

static uint32_t g_ring;  // the game uses a single movie at a time
static uint64_t g_pts;
static int g_pixel_mode = 3;

void avc_reset(void);
int avc_next_au(const uint8_t *data, int len, int *got);
int avc_decode(uint32_t dst, int stride, int mode);

typedef struct AtracDecoder AtracDecoder;
AtracDecoder *atrac_dec_new(int plus, int channels, int block_align, const uint8_t *extra, int extra_size);
void atrac_dec_free(AtracDecoder *d);
int atrac_dec_frame(AtracDecoder *d, const uint8_t *buf, int size, float *left, float *right);

// growable byte FIFO
typedef struct {
    uint8_t *d;
    size_t r, w, cap;
} Fifo;

static void fifo_push(Fifo *f, const uint8_t *p, size_t n) {
    if (f->r && f->r == f->w) f->r = f->w = 0;
    if (f->w + n > f->cap) {
        if (f->r) {  // compact first
            memmove(f->d, f->d + f->r, f->w - f->r);
            f->w -= f->r;
            f->r = 0;
        }
        if (f->w + n > f->cap) {
            f->cap = (f->w + n) * 2;
            f->d = realloc(f->d, f->cap);
        }
    }
    memcpy(f->d + f->w, p, n);
    f->w += n;
}

static Fifo g_video, g_audio;
static uint64_t g_video_in, g_video_out;  // total video bytes pushed / consumed
// for each packet still in the ring: total video bytes once it is demuxed
static uint64_t g_pkt_end[4096];
static uint32_t g_pkt_head, g_pkt_tail;
static AtracDecoder *g_at;
static int g_at_bpf;
static uint8_t g_at_frame[4096];
static int g_at_pending;

static void mpeg_reset_streams(void) {
    g_video.r = g_video.w = g_audio.r = g_audio.w = 0;
    g_video_in = g_video_out = 0;
    g_pkt_head = g_pkt_tail = 0;
    g_at_pending = 0;
    avc_reset();
}

// Split one 2048-byte MPEG-PS packet into the video and audio FIFOs.
static void demux_packet(const uint8_t *p) {
    uint32_t i = 0;
    while (i + 4 <= PACKET) {
        if (p[i] || p[i + 1] || p[i + 2] != 1) { i++; continue; }
        uint8_t sid = p[i + 3];
        if (sid == 0xBA) { i += 14 + (p[i + 13] & 7); continue; }
        if (sid == 0xB9) break;
        uint32_t len = (uint32_t)p[i + 4] << 8 | p[i + 5];
        uint32_t end = i + 6 + len > PACKET ? PACKET : i + 6 + len;
        if (sid == 0xE0 || sid == 0xBD) {
            uint32_t data = i + 9 + p[i + 8];
            if (sid == 0xBD) data += 4;  // private stream prefix
            if (data < end) fifo_push(sid == 0xE0 ? &g_video : &g_audio, p + data, end - data);
            if (sid == 0xE0 && data < end) g_video_in += end - data;
        }
        i = end;
    }
}

// Give ring packets back once the video they carried has been consumed.
static void release_packets(void) {
    uint32_t n = 0;
    while (g_pkt_tail != g_pkt_head && g_pkt_end[g_pkt_tail % 4096] <= g_video_out) {
        g_pkt_tail++;
        n++;
    }
    if (n && g_ring) {
        mem_w32(g_ring + RB_FREE, mem_r32(g_ring + RB_FREE) + n);
        mem_w32(g_ring + RB_READ, mem_r32(g_ring + RB_READ) + n);
    }
}

static uint32_t be32(uint32_t a) { return __builtin_bswap32(mem_r32(a)); }

void sceMpegInit(Ctx *c) { RET(0); }
void sceMpegFinish(Ctx *c) { RET(0); }
void sceMpegRingbufferQueryMemSize(Ctx *c) { RET(ARG(0) * PACKET); }
void sceMpegQueryMemSize(Ctx *c) { RET(0x10000); }

// (ringbuf, packets, data, size, callback, cbArg)
void sceMpegRingbufferConstruct(Ctx *c) {
    uint32_t rb = ARG(0);
    memset(host(rb), 0, 0x2C);
    mem_w32(rb + RB_PACKETS, ARG(1));
    mem_w32(rb + RB_FREE, ARG(1));
    mem_w32(rb + RB_PSIZE, PACKET);
    mem_w32(rb + RB_DATA, ARG(2));
    mem_w32(rb + RB_CB, ARG(4));
    mem_w32(rb + RB_CBARG, ARG(5));
    mem_w32(rb + RB_UPPER, ARG(2) + ARG(1) * PACKET);
    RET(0);
}
void sceMpegRingbufferDestruct(Ctx *c) { RET(0); }

// (mpeg*, data, size, ringbuf, frameWidth, mode, ddrtop)
void sceMpegCreate(Ctx *c) {
    mem_w32(ARG(0), ARG(1));
    g_ring = ARG(3);
    mem_w32(g_ring + RB_MPEG, ARG(0));
    g_pts = 0;
    mpeg_reset_streams();
    RET(0);
}
void sceMpegDelete(Ctx *c) { RET(0); }

// PSMF header: stream data offset at +8, size at +12 (big endian)
void sceMpegQueryStreamOffset(Ctx *c) {
    mem_w32(ARG(2), be32(ARG(1) + 8));
    RET(0);
}
void sceMpegQueryStreamSize(Ctx *c) {
    mem_w32(ARG(1), be32(ARG(0) + 12));
    RET(0);
}

static uint32_t g_next_stream = 1;
void sceMpegRegistStream(Ctx *c) { RET(g_next_stream++); }
void sceMpegUnRegistStream(Ctx *c) { RET(0); }
void sceMpegMallocAvcEsBuf(Ctx *c) { RET(1); }
void sceMpegFreeAvcEsBuf(Ctx *c) { RET(0); }
void sceMpegInitAu(Ctx *c) {
    memset(host(ARG(2)), 0, 24);
    mem_w32(ARG(2), 0xFFFFFFFFu);
    mem_w32(ARG(2) + 4, 0xFFFFFFFFu);
    RET(0);
}
void sceMpegQueryAtracEsSize(Ctx *c) {
    mem_w32(ARG(1), 2112);
    mem_w32(ARG(2), 8192);
    RET(0);
}
// (mpeg, mode*): {int unknown; int pixelMode}
void sceMpegAvcDecodeMode(Ctx *c) {
    if (ARG(1)) g_pixel_mode = (int)mem_r32(ARG(1) + 4);
    RET(0);
}

void sceMpegFlushAllStream(Ctx *c) {
    if (g_ring) {
        mem_w32(g_ring + RB_FREE, mem_r32(g_ring + RB_PACKETS));
        mem_w32(g_ring + RB_READ, 0);
        mem_w32(g_ring + RB_WRITTEN, 0);
    }
    mpeg_reset_streams();
    RET(0);
}

void sceMpegRingbufferAvailableSize(Ctx *c) { RET(mem_r32(ARG(0) + RB_FREE)); }

// (ringbuf, numPackets, available): pull data through the game's callback
void sceMpegRingbufferPut(Ctx *c) {
    uint32_t rb = ARG(0), want = ARG(1), total = mem_r32(rb + RB_PACKETS);
    uint32_t free_ = mem_r32(rb + RB_FREE);
    if (want > free_) want = free_;
    uint32_t added = 0;
    while (want > 0) {
        uint32_t wpos = mem_r32(rb + RB_WRITTEN) % total;
        uint32_t chunk = total - wpos < want ? total - wpos : want;
        hle_call_guest(c, mem_r32(rb + RB_CB), mem_r32(rb + RB_DATA) + wpos * PACKET, chunk,
                       mem_r32(rb + RB_CBARG));
        int32_t got = (int32_t)c->r[2];
        if (got <= 0) break;
        static FILE *dump;  // MPEG_DUMP=file: save the stream the game feeds us
        if (!dump && getenv("MPEG_DUMP")) dump = fopen(getenv("MPEG_DUMP"), "wb");
        if (dump) fwrite(host(mem_r32(rb + RB_DATA) + wpos * PACKET), PACKET, (size_t)got, dump);
        for (int32_t k = 0; k < got; k++) {
            demux_packet(host(mem_r32(rb + RB_DATA) + (wpos + (uint32_t)k) % total * PACKET));
            g_pkt_end[g_pkt_head++ % 4096] = g_video_in;
        }
        mem_w32(rb + RB_WRITTEN, mem_r32(rb + RB_WRITTEN) + (uint32_t)got);
        mem_w32(rb + RB_FREE, mem_r32(rb + RB_FREE) - (uint32_t)got);
        added += (uint32_t)got;
        want -= (uint32_t)got;
        if ((uint32_t)got < chunk) break;
    }
    RET(added);
}

static void write_au(uint32_t au) {
    mem_w32(au, (uint32_t)(g_pts >> 32));
    mem_w32(au + 4, (uint32_t)g_pts);
    mem_w32(au + 8, (uint32_t)(g_pts >> 32));
    mem_w32(au + 12, (uint32_t)g_pts);
}

void sceMpegGetAvcAu(Ctx *c) {
    int got = 0;
    while (!got && g_video.r < g_video.w) {
        int used = avc_next_au(g_video.d + g_video.r, (int)(g_video.w - g_video.r), &got);
        g_video.r += (size_t)used;
        g_video_out += (uint64_t)used;
    }
    release_packets();
    if (!got) { RET(MPEG_ERR_NO_DATA); return; }
    g_pts += 3003;  // 29.97 fps in 90 kHz units
    write_au(ARG(2));
    if (ARG(3)) mem_w32(ARG(3), 0);
    RET(0);
}

// One ATRAC3+ frame: 8-byte header (0F D0, size) + payload.
void sceMpegGetAtracAu(Ctx *c) {
    while (g_audio.w - g_audio.r >= 8) {
        const uint8_t *h = g_audio.d + g_audio.r;
        if (h[0] != 0x0F || h[1] != 0xD0) { g_audio.r++; continue; }
        int bpf = (((h[2] & 3) << 8) | h[3]) * 8 + 8;
        if (g_audio.w - g_audio.r < (size_t)bpf + 8) break;
        if (bpf > (int)sizeof g_at_frame) { g_audio.r += 2; continue; }
        if (!g_at || g_at_bpf != bpf) {
            atrac_dec_free(g_at);
            g_at = atrac_dec_new(1, 2, bpf, NULL, 0);
            g_at_bpf = bpf;
        }
        memcpy(g_at_frame, h + 8, (size_t)bpf);
        g_audio.r += (size_t)bpf + 8;
        g_at_pending = 1;
        write_au(ARG(2));
        if (ARG(3)) mem_w32(ARG(3), 0);
        RET(0);
        return;
    }
    RET(MPEG_ERR_NO_DATA);
}

// (mpeg, au, frameWidth, buffer**, init*)
void sceMpegAvcDecode(Ctx *c) {
    uint32_t dst = mem_r32(ARG(3));
    int ok = avc_decode(dst, (int)ARG(2), g_pixel_mode);
    if (ARG(4)) mem_w32(ARG(4), (uint32_t)ok);
    RET(0);
}
void sceMpegAvcDecodeStop(Ctx *c) {
    if (ARG(3)) mem_w32(ARG(3), 0);
    RET(0);
}

// (mpeg, au, buffer, init): 2048 stereo s16 samples
void sceMpegAtracDecode(Ctx *c) {
    static float l[2048], r[2048];
    uint32_t out = ARG(2);
    int n = 0;
    if (g_at_pending && g_at) n = atrac_dec_frame(g_at, g_at_frame, g_at_bpf, l, r);
    g_at_pending = 0;
    for (int i = 0; i < 2048; i++) {
        float fl = i < n ? l[i] : 0.f, fr = i < n ? r[i] : 0.f;
        mem_w16(out + (uint32_t)i * 4, (uint16_t)(int16_t)(fl > 1.f ? 32767 : fl < -1.f ? -32768 : fl * 32767.f));
        mem_w16(out + (uint32_t)i * 4 + 2, (uint16_t)(int16_t)(fr > 1.f ? 32767 : fr < -1.f ? -32768 : fr * 32767.f));
    }
    RET(0);
}
