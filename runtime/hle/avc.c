// H.264 decoding for movies, on top of a minimal FFmpeg (tools/build_ffmpeg.sh).
#include <libavcodec/avcodec.h>
#include <stdlib.h>

#include "hle.h"

typedef struct {
    AVCodecContext *ctx;
    AVCodecParserContext *parser;
    AVPacket *pkt;
    AVFrame *frame;
    uint8_t *au;  // last access unit returned by avc_next_au
    int au_size, au_cap;
} Avc;

static Avc g_avc;

static void avc_init(void) {
    if (g_avc.ctx) return;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    g_avc.ctx = avcodec_alloc_context3(codec);
    g_avc.ctx->thread_count = 1;
    avcodec_open2(g_avc.ctx, codec, NULL);
    g_avc.parser = av_parser_init(AV_CODEC_ID_H264);
    g_avc.pkt = av_packet_alloc();
    g_avc.frame = av_frame_alloc();
}

void avc_reset(void) {
    avc_init();
    avcodec_flush_buffers(g_avc.ctx);
    av_parser_close(g_avc.parser);
    g_avc.parser = av_parser_init(AV_CODEC_ID_H264);
    g_avc.au_size = 0;
}

// Split the next access unit out of `data`; returns bytes consumed. *got is set
// when a complete AU is ready (it is kept for avc_decode).
int avc_next_au(const uint8_t *data, int len, int *got) {
    avc_init();
    uint8_t *out = NULL;
    int out_size = 0;
    int used = av_parser_parse2(g_avc.parser, g_avc.ctx, &out, &out_size, data, len, AV_NOPTS_VALUE,
                                AV_NOPTS_VALUE, 0);
    *got = 0;
    if (out_size > 0) {
        if (out_size + AV_INPUT_BUFFER_PADDING_SIZE > g_avc.au_cap) {
            g_avc.au_cap = out_size + AV_INPUT_BUFFER_PADDING_SIZE;
            g_avc.au = realloc(g_avc.au, (size_t)g_avc.au_cap);
        }
        memcpy(g_avc.au, out, (size_t)out_size);
        memset(g_avc.au + out_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        g_avc.au_size = out_size;
        *got = 1;
    }
    return used;
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// Decode the pending AU and write the picture (if one comes out) to guest
// memory: `stride` in pixels, `mode` 0 565, 1 5551, 2 4444, 3 8888.
int avc_decode(uint32_t dst, int stride, int mode) {
    if (!g_avc.au_size) return 0;
    g_avc.pkt->data = g_avc.au;
    g_avc.pkt->size = g_avc.au_size;
    g_avc.au_size = 0;
    if (avcodec_send_packet(g_avc.ctx, g_avc.pkt) < 0) return 0;
    if (avcodec_receive_frame(g_avc.ctx, g_avc.frame) < 0) return 0;
    AVFrame *f = g_avc.frame;
    int w = f->width < stride ? f->width : stride, h = f->height > 272 ? 272 : f->height;
    for (int y = 0; y < h; y++) {
        const uint8_t *Y = f->data[0] + y * f->linesize[0];
        const uint8_t *U = f->data[1] + (y / 2) * f->linesize[1];
        const uint8_t *V = f->data[2] + (y / 2) * f->linesize[2];
        for (int x = 0; x < w; x++) {
            int c = (Y[x] - 16) * 298, d = U[x / 2] - 128, e = V[x / 2] - 128;
            uint8_t r = clamp8((c + 409 * e + 128) >> 8);
            uint8_t g = clamp8((c - 100 * d - 208 * e + 128) >> 8);
            uint8_t b = clamp8((c + 516 * d + 128) >> 8);
            uint32_t a = dst + (uint32_t)(y * stride + x) * (mode == 3 ? 4u : 2u);
            switch (mode) {
            case 3: mem_w32(a, r | g << 8 | (uint32_t)b << 16 | 0xFF000000u); break;
            case 0: mem_w16(a, (uint16_t)(r >> 3 | (g >> 2) << 5 | (b >> 3) << 11)); break;
            case 1: mem_w16(a, (uint16_t)(r >> 3 | (g >> 3) << 5 | (b >> 3) << 10 | 0x8000)); break;
            default: mem_w16(a, (uint16_t)(r >> 4 | (g >> 4) << 4 | (b >> 4) << 8 | 0xF000)); break;
            }
        }
    }
    return 1;
}

// A minimal FFmpeg build references this AV1 film-grain helper from the H.264
// SEI code without compiling it; nothing here ever allocates film grain data.
void ff_aom_uninit_film_grain_params(void *params);
void ff_aom_uninit_film_grain_params(void *params) { (void)params; }
