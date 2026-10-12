// sceAtrac3plus: ATRAC3/ATRAC3+ playback contexts (music, voices).
//
// The game hands over a buffer holding the start of a RIFF/WAVE file. If the
// whole file fits, it is decoded from memory; otherwise the game streams the
// rest into that buffer used as a ring, asking where to write next
// (GetStreamDataInfo) and reporting what it wrote (AddStreamData). We keep a
// host-side copy of the whole file and decode by file offset; the guest ring
// is only bookkeeping.
#include <stdlib.h>

#include "hle.h"

typedef struct AtracDecoder AtracDecoder;
AtracDecoder *atrac_dec_new(int plus, int channels, int block_align, const uint8_t *extra, int extra_size);
void atrac_dec_free(AtracDecoder *d);
void atrac_dec_flush(AtracDecoder *d);
int atrac_dec_frame(AtracDecoder *d, const uint8_t *buf, int size, float *left, float *right);

#define ERR_BAD_ID 0x80630005u
#define ERR_UNKNOWN_FORMAT 0x80630006u
#define ERR_NO_ID 0x80630003u
#define ERR_ADD_TOO_BIG 0x80630018u
#define ERR_NO_LOOP_INFO 0x80630021u
#define ERR_ALL_DECODED 0x80630024u
#define ALLDATA_ON_MEMORY (-1)
#define NONLOOP_STREAM_ON_MEMORY (-2)
#define LOOP_STREAM_ON_MEMORY (-3)

#define MAX_IDS 6

typedef struct {
    int used, plus, channels, bpf, spf;  // bytes / samples per frame
    uint32_t file_size, data_off, data_end;
    int end_sample, skip;  // last sample index; samples to drop at the start
    int loop_start, loop_end;  // in output samples, -1 if none
    int loop_num;
    uint8_t *file;  // host copy of the whole file
    uint32_t loaded;  // file bytes [0, loaded) are valid (stream order)
    uint32_t read_off;  // next file offset the game should read
    // guest ring
    uint32_t buf, buf_size, wpos, held;  // write position, bytes not yet consumed
    int streamed;
    int cur;  // next output sample
    AtracDecoder *dec;
    float pcm[2][4096];
} Atrac;

static Atrac g_at[MAX_IDS];

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static Atrac *get(uint32_t id) { return id < MAX_IDS && g_at[id].used ? &g_at[id] : NULL; }

static int parse_header(Atrac *a, const uint8_t *h, uint32_t avail) {
    if (avail < 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    a->file_size = le32(h + 4) + 8;
    a->loop_start = a->loop_end = -1;
    int sample_offset = 0, have_fmt = 0;
    const uint8_t *extra = NULL;
    int extra_size = 0;
    for (uint32_t o = 12; o + 8 <= avail;) {
        uint32_t size = le32(h + o + 4);
        const uint8_t *c = h + o + 8;
        if (!memcmp(h + o, "fmt ", 4)) {
            uint16_t tag = le16(c);
            a->plus = tag == 0xFFFE;
            if (tag != 0xFFFE && tag != 0x0270) return -1;
            a->channels = le16(c + 2);
            a->bpf = le16(c + 12);
            a->spf = a->plus ? 2048 : 1024;
            if (!a->plus && size > 18) {
                extra = c + 18;
                extra_size = (int)size - 18;
            }
            have_fmt = 1;
        } else if (!memcmp(h + o, "fact", 4)) {
            a->end_sample = (int)le32(c) - 1;
            if (size >= 8) sample_offset = (int)le32(c + 4);
        } else if (!memcmp(h + o, "smpl", 4)) {
            if (size >= 44 && le32(c + 28) > 0) {
                a->loop_start = (int)le32(c + 36 + 8);
                a->loop_end = (int)le32(c + 36 + 12);
            }
        } else if (!memcmp(h + o, "data", 4)) {
            a->data_off = o + 8;
            a->data_end = a->data_off + size;
            break;
        }
        o += 8 + size + (size & 1);
    }
    if (!have_fmt || !a->data_off || a->bpf <= 0) return -1;
    // decoder delay plus the encoder's own offset
    a->skip = sample_offset + (a->plus ? 368 : 69);
    if (a->loop_start >= 0) {
        a->loop_start -= a->skip;
        a->loop_end -= a->skip;
    }
    a->dec = atrac_dec_new(a->plus, a->channels, a->bpf, extra, extra_size);
    return a->dec ? 0 : -1;
}

static uint32_t frame_offset(const Atrac *a, int sample) {
    return a->data_off + (uint32_t)((sample + a->skip) / a->spf) * (uint32_t)a->bpf;
}

// Seek to an output sample: flush and pre-roll the frame before it.
static void seek(Atrac *a, int sample) {
    atrac_dec_flush(a->dec);
    a->cur = sample;
    uint32_t off = frame_offset(a, sample);
    if (off >= a->data_off + (uint32_t)a->bpf && off <= a->loaded)
        atrac_dec_frame(a->dec, a->file + off - a->bpf, a->bpf, a->pcm[0], a->pcm[1]);
}

void sceAtracSetDataAndGetID(Ctx *c) {
    uint32_t buf = ARG(0), size = ARG(1);
    int id = -1;
    for (int k = 0; k < MAX_IDS; k++)
        if (!g_at[k].used) { id = k; break; }
    if (id < 0) { RET(ERR_NO_ID); return; }
    Atrac *a = &g_at[id];
    memset(a, 0, sizeof *a);
    if (parse_header(a, host(buf), size)) {
        HLOG("  atrac: unsupported data at %08X\n", buf);
        RET(ERR_UNKNOWN_FORMAT);
        return;
    }
    a->used = 1;
    a->file = calloc(1, a->file_size);
    a->buf = buf;
    a->buf_size = size;
    a->loaded = size < a->file_size ? size : a->file_size;
    memcpy(a->file, host(buf), a->loaded);
    a->read_off = a->loaded;
    a->streamed = size < a->file_size;
    a->wpos = a->loaded % size;
    a->held = a->loaded;
    HLOG("  atrac %d: %s %dch bpf=%d samples=%d file=%u buf=%u loop=%d..%d\n", id, a->plus ? "AT3+" : "AT3",
         a->channels, a->bpf, a->end_sample + 1, a->file_size, size, a->loop_start, a->loop_end);
    seek(a, 0);
    RET(id);
}

void sceAtracReleaseAtracID(Ctx *c) {
    Atrac *a = get(ARG(0));
    if (!a) { RET(ERR_BAD_ID); return; }
    atrac_dec_free(a->dec);
    free(a->file);
    a->used = 0;
    RET(0);
}

static int remaining_frames(const Atrac *a) {
    if (!a->streamed) return ALLDATA_ON_MEMORY;
    if (a->read_off >= a->file_size && a->loop_num == 0) return NONLOOP_STREAM_ON_MEMORY;
    uint32_t pos = frame_offset(a, a->cur);
    return a->loaded > pos ? (int)((a->loaded - pos) / (uint32_t)a->bpf) : 0;
}

// Where the game should write the next piece of the file, and how much.
static void stream_info(const Atrac *a, uint32_t *write_ptr, uint32_t *writable, uint32_t *read_off) {
    if (!a->streamed || (a->read_off >= a->file_size && a->loop_num == 0)) {
        *write_ptr = a->buf;
        *writable = 0;
        *read_off = 0;
        return;
    }
    uint32_t free_ = a->buf_size > a->held ? a->buf_size - a->held : 0;
    uint32_t contiguous = a->buf_size - a->wpos;
    uint32_t n = free_ < contiguous ? free_ : contiguous;
    uint32_t rest = a->file_size - a->read_off;
    if (n > rest) n = rest;
    *write_ptr = a->buf + a->wpos;
    *writable = n;
    *read_off = a->read_off;
}

void sceAtracGetStreamDataInfo(Ctx *c) {
    Atrac *a = get(ARG(0));
    if (!a) { RET(ERR_BAD_ID); return; }
    uint32_t w, n, r;
    stream_info(a, &w, &n, &r);
    if (ARG(1)) mem_w32(ARG(1), w);
    if (ARG(2)) mem_w32(ARG(2), n);
    if (ARG(3)) mem_w32(ARG(3), r);
    RET(0);
}

void sceAtracAddStreamData(Ctx *c) {
    Atrac *a = get(ARG(0));
    if (!a) { RET(ERR_BAD_ID); return; }
    uint32_t w, n, r, add = ARG(1);
    stream_info(a, &w, &n, &r);
    if (add > n) { RET(ERR_ADD_TOO_BIG); return; }
    if (add) {
        memcpy(a->file + r, host(w), add);
        a->read_off = r + add;
        if (a->read_off > a->loaded) a->loaded = a->read_off;
        a->wpos = (a->wpos + add) % a->buf_size;
        a->held += add;
        // looping streams start over at the loop's frame once the file is read
        if (a->read_off >= a->file_size && a->loop_num != 0 && a->loop_start >= 0)
            a->read_off = frame_offset(a, a->loop_start) - (uint32_t)a->bpf * 2;
    }
    RET(0);
}

// (id, out, *samples, *finish, *remain)
void sceAtracDecodeData(Ctx *c) {
    Atrac *a = get(ARG(0));
    if (!a) { RET(ERR_BAD_ID); return; }
    uint32_t out = ARG(1);
    int end = a->loop_num != 0 && a->loop_end >= 0 ? a->loop_end : a->end_sample;
    if (a->cur > a->end_sample && a->loop_num == 0) {
        if (ARG(2)) mem_w32(ARG(2), 0);
        if (ARG(3)) mem_w32(ARG(3), 1);
        RET(ERR_ALL_DECODED);
        return;
    }
    int first = (a->cur + a->skip) % a->spf;  // samples of this frame already past
    int n = a->spf - first;
    if (a->cur + n > end + 1) n = end + 1 - a->cur;
    if (n < 0) n = 0;
    uint32_t off = frame_offset(a, a->cur);
    int got = 0;
    if (off + (uint32_t)a->bpf <= a->loaded && off + (uint32_t)a->bpf <= a->data_end)
        got = atrac_dec_frame(a->dec, a->file + off, a->bpf, a->pcm[0], a->pcm[1]);
    for (int i = 0; i < n; i++) {
        int16_t l = 0, r = 0;
        if (got > 0) {
            float fl = a->pcm[0][first + i], fr = a->pcm[a->channels > 1][first + i];
            l = (int16_t)(fl > 1.f ? 32767 : fl < -1.f ? -32768 : fl * 32767.f);
            r = (int16_t)(fr > 1.f ? 32767 : fr < -1.f ? -32768 : fr * 32767.f);
        }
        if (out) {
            mem_w16(out + (uint32_t)i * 4, (uint16_t)l);
            mem_w16(out + (uint32_t)i * 4 + 2, (uint16_t)r);
        }
    }
    a->cur += n;
    // the frame's bytes leave the guest ring
    a->held = a->held > (uint32_t)a->bpf ? a->held - (uint32_t)a->bpf : 0;
    int finish = 0;
    if (a->cur > end) {
        if (a->loop_num != 0 && a->loop_start >= 0) {
            if (a->loop_num > 0) a->loop_num--;
            seek(a, a->loop_start);
        } else if (a->cur > a->end_sample) {
            finish = 1;
        }
    }
    if (ARG(2)) mem_w32(ARG(2), (uint32_t)n);
    if (ARG(3)) mem_w32(ARG(3), (uint32_t)finish);
    if (ARG(4)) mem_w32(ARG(4), (uint32_t)remaining_frames(a));
    RET(0);
}

void sceAtracGetRemainFrame(Ctx *c) {
    Atrac *a = get(ARG(0));
    if (!a) { RET(ERR_BAD_ID); return; }
    mem_w32(ARG(1), (uint32_t)remaining_frames(a));
    RET(0);
}

void sceAtracGetNextDecodePosition(Ctx *c) {
    Atrac *a = get(ARG(0));
    if (!a) { RET(ERR_BAD_ID); return; }
    if (a->cur > a->end_sample) {
        mem_w32(ARG(1), 0);
        RET(ERR_ALL_DECODED);
        return;
    }
    mem_w32(ARG(1), (uint32_t)a->cur);
    RET(0);
}

void sceAtracSetLoopNum(Ctx *c) {
    Atrac *a = get(ARG(0));
    if (!a) { RET(ERR_BAD_ID); return; }
    if (a->loop_start < 0) { RET(ERR_NO_LOOP_INFO); return; }
    a->loop_num = (int)ARG(1);
    RET(0);
}

// (id, sample, bytesWrittenFirstBuf, bytesWrittenSecondBuf)
void sceAtracResetPlayPosition(Ctx *c) {
    Atrac *a = get(ARG(0));
    if (!a) { RET(ERR_BAD_ID); return; }
    int sample = (int)ARG(1);
    if (a->streamed) {
        // the game refilled the buffer from the frame holding `sample`
        uint32_t off = frame_offset(a, sample) - (sample > 0 ? (uint32_t)a->bpf : 0);
        uint32_t n = ARG(2);
        if (off + n > a->file_size) n = a->file_size - off;
        memcpy(a->file + off, host(a->buf), n);
        a->loaded = off + n;
        a->read_off = off + n;
        a->wpos = n % a->buf_size;
        a->held = n;
    }
    seek(a, sample);
    RET(0);
}
