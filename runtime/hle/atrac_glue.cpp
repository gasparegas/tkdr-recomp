// C interface to the FFmpeg-derived ATRAC3/ATRAC3+ decoders (third_party/at3_standalone).
#include "at3_decoders.h"

extern "C" {

struct AtracDecoder {
    ATRAC3PContext *p;
    ATRAC3Context *a3;
};

AtracDecoder *atrac_dec_new(int plus, int channels, int block_align, const uint8_t *extra, int extra_size) {
    AtracDecoder *d = new AtracDecoder{nullptr, nullptr};
    int ba = block_align;
    if (plus) d->p = atrac3p_alloc(channels, &ba);
    else d->a3 = atrac3_alloc(channels, &ba, extra, extra_size);
    if (!d->p && !d->a3) {
        delete d;
        return nullptr;
    }
    return d;
}

void atrac_dec_free(AtracDecoder *d) {
    if (!d) return;
    if (d->p) atrac3p_free(d->p);
    if (d->a3) atrac3_free(d->a3);
    delete d;
}

void atrac_dec_flush(AtracDecoder *d) {
    if (d->p) atrac3p_flush_buffers(d->p);
    if (d->a3) atrac3_flush_buffers(d->a3);
}

// Decode one frame into planar float; returns samples per channel (<= 0 on error).
int atrac_dec_frame(AtracDecoder *d, const uint8_t *buf, int size, float *left, float *right) {
    float *out[2] = {left, right};
    int n = 0;
    int r = d->p ? atrac3p_decode_frame(d->p, out, &n, buf, size) : atrac3_decode_frame(d->a3, out, &n, buf, size);
    return r < 0 ? r : n;
}
}
