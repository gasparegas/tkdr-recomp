// sceMpeg (PSMF movie playback), without decoding for now: the ring buffer is
// filled through the game's own callback and drained as if frames were decoded,
// so movies "play" (as black frames + silence) and end on their own.
#include "hle.h"

#define MPEG_ERR_NO_DATA 0x80618001u
#define PACKET 2048u

void hle_call_guest(Ctx *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2);

// SceMpegRingbuffer field offsets
enum { RB_PACKETS = 0x00, RB_READ = 0x04, RB_WRITTEN = 0x08, RB_FREE = 0x0C, RB_PSIZE = 0x10,
       RB_DATA = 0x14, RB_CB = 0x18, RB_CBARG = 0x1C, RB_UPPER = 0x20, RB_MPEG = 0x28 };

static uint32_t g_ring;  // the game uses a single movie at a time
static uint64_t g_pts;

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
void sceMpegAvcDecodeMode(Ctx *c) { RET(0); }

void sceMpegFlushAllStream(Ctx *c) {
    if (g_ring) {
        mem_w32(g_ring + RB_FREE, mem_r32(g_ring + RB_PACKETS));
        mem_w32(g_ring + RB_READ, 0);
        mem_w32(g_ring + RB_WRITTEN, 0);
    }
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

// Each video access unit consumes a few packets of the stream.
void sceMpegGetAvcAu(Ctx *c) {
    uint32_t rb = g_ring, total = mem_r32(rb + RB_PACKETS), free_ = mem_r32(rb + RB_FREE);
    uint32_t avail = total - free_;
    if (!avail) { RET(MPEG_ERR_NO_DATA); return; }
    uint32_t use = avail < 4 ? avail : 4;
    mem_w32(rb + RB_FREE, free_ + use);
    mem_w32(rb + RB_READ, mem_r32(rb + RB_READ) + use);
    g_pts += 3003;  // 29.97 fps in 90 kHz units
    write_au(ARG(2));
    if (ARG(3)) mem_w32(ARG(3), 0);
    RET(0);
}

void sceMpegGetAtracAu(Ctx *c) {
    uint32_t avail = mem_r32(g_ring + RB_PACKETS) - mem_r32(g_ring + RB_FREE);
    if (!avail) { RET(MPEG_ERR_NO_DATA); return; }
    write_au(ARG(2));
    if (ARG(3)) mem_w32(ARG(3), 0);
    RET(0);
}

// (mpeg, au, frameWidth, buffer**, init*)
void sceMpegAvcDecode(Ctx *c) {
    if (ARG(4)) mem_w32(ARG(4), 1);  // a frame is ready
    RET(0);
}
void sceMpegAvcDecodeStop(Ctx *c) {
    if (ARG(3)) mem_w32(ARG(3), 0);
    RET(0);
}
// (mpeg, au, buffer, init)
void sceMpegAtracDecode(Ctx *c) {
    memset(host(ARG(2)), 0, 8192);
    RET(0);
}
