// Software rasterizer for the GE. (First cut: block transfers and CLUT loads;
// primitives are counted only.)
#include <stdio.h>

#include "gpu.h"

static uint32_t g_prims;

uint32_t gpu_stat_prims(void) {
    uint32_t n = g_prims;
    g_prims = 0;
    return n;
}

void gpu_draw_prim(int prim, int count) {
    (void)prim;
    (void)count;
    g_prims++;
}

// Copy a rectangle between two VRAM/RAM surfaces (GE_TRANSFERSTART).
void gpu_block_transfer(void) {
    const uint32_t *r = g_ge.cmd;
    uint32_t src = (r[GE_TRANSFERSRC] & 0xFFFFF0u) | ((r[GE_TRANSFERSRCW] & 0xFF0000u) << 8);
    uint32_t dst = (r[GE_TRANSFERDST] & 0xFFFFF0u) | ((r[GE_TRANSFERDSTW] & 0xFF0000u) << 8);
    uint32_t sstride = r[GE_TRANSFERSRCW] & 0x7F8u, dstride = r[GE_TRANSFERDSTW] & 0x7F8u;
    uint32_t sx = r[GE_TRANSFERSRCPOS] & 0x3FF, sy = (r[GE_TRANSFERSRCPOS] >> 10) & 0x3FF;
    uint32_t dx = r[GE_TRANSFERDSTPOS] & 0x3FF, dy = (r[GE_TRANSFERDSTPOS] >> 10) & 0x3FF;
    uint32_t w = (r[GE_TRANSFERSIZE] & 0x3FF) + 1, h = ((r[GE_TRANSFERSIZE] >> 10) & 0x3FF) + 1;
    uint32_t bpp = (r[GE_TRANSFERSTART] & 1) ? 4 : 2;
    for (uint32_t y = 0; y < h; y++)
        memmove(host(dst + ((dy + y) * dstride + dx) * bpp), host(src + ((sy + y) * sstride + sx) * bpp),
                w * bpp);
}

// LOADCLUT: copy n*8 (or n*16) palette entries into the GE's internal palette.
void gpu_load_clut(void) {
    const uint32_t *r = g_ge.cmd;
    uint32_t addr = (r[GE_CLUTADDR] & 0xFFFFF0u) | ((r[GE_CLUTADDRUPPER] << 8) & 0x0F000000u);
    uint32_t blocks = r[GE_LOADCLUT] & 0x3F;
    uint32_t bytes = blocks * 32;
    if (bytes > sizeof g_ge.clut) bytes = sizeof g_ge.clut;
    memcpy(g_ge.clut, host(addr), bytes);
}
