// PSP Graphics Engine (GE): display list processing and software rendering.
#pragma once
#include "../recomp.h"

typedef struct {
    uint32_t fb_addr, stride, format;  // what sceDisplay scans out
} DisplayState;
extern DisplayState g_display;

// Command numbers used by the processor and rasterizer
enum {
    GE_NOP = 0x00, GE_VADDR = 0x01, GE_IADDR = 0x02, GE_PRIM = 0x04, GE_BEZIER = 0x05,
    GE_SPLINE = 0x06, GE_BBOX = 0x07, GE_JUMP = 0x08, GE_BJUMP = 0x09, GE_CALL = 0x0A,
    GE_RET = 0x0B, GE_END = 0x0C, GE_SIGNAL = 0x0E, GE_FINISH = 0x0F, GE_BASE = 0x10,
    GE_VERTEXTYPE = 0x12, GE_OFFSETADDR = 0x13, GE_ORIGIN = 0x14,
    GE_LIGHTINGENABLE = 0x17, GE_LIGHTENABLE0 = 0x18, GE_CULLFACEENABLE = 0x1D,
    GE_TEXTUREMAPENABLE = 0x1E, GE_FOGENABLE = 0x1F, GE_DITHERENABLE = 0x20,
    GE_ALPHABLENDENABLE = 0x21, GE_ALPHATESTENABLE = 0x22, GE_ZTESTENABLE = 0x23,
    GE_STENCILTESTENABLE = 0x24, GE_COLORTESTENABLE = 0x27, GE_LOGICOPENABLE = 0x28,
    GE_BONEMATRIXNUMBER = 0x2A, GE_BONEMATRIXDATA = 0x2B, GE_MORPHWEIGHT0 = 0x2C,
    GE_WORLDMATRIXNUMBER = 0x3A, GE_WORLDMATRIXDATA = 0x3B, GE_VIEWMATRIXNUMBER = 0x3C,
    GE_VIEWMATRIXDATA = 0x3D, GE_PROJMATRIXNUMBER = 0x3E, GE_PROJMATRIXDATA = 0x3F,
    GE_TGENMATRIXNUMBER = 0x40, GE_TGENMATRIXDATA = 0x41,
    GE_VIEWPORTXSCALE = 0x42, GE_VIEWPORTYSCALE = 0x43, GE_VIEWPORTZSCALE = 0x44,
    GE_VIEWPORTXCENTER = 0x45, GE_VIEWPORTYCENTER = 0x46, GE_VIEWPORTZCENTER = 0x47,
    GE_TEXSCALEU = 0x48, GE_TEXSCALEV = 0x49, GE_TEXOFFSETU = 0x4A, GE_TEXOFFSETV = 0x4B,
    GE_OFFSETX = 0x4C, GE_OFFSETY = 0x4D, GE_SHADEMODE = 0x50, GE_REVERSENORMAL = 0x51,
    GE_MATERIALUPDATE = 0x53, GE_MATERIALEMISSIVE = 0x54, GE_MATERIALAMBIENT = 0x55,
    GE_MATERIALDIFFUSE = 0x56, GE_MATERIALSPECULAR = 0x57, GE_MATERIALALPHA = 0x58,
    GE_MATERIALSPECULARCOEF = 0x5B, GE_AMBIENTCOLOR = 0x5C, GE_AMBIENTALPHA = 0x5D,
    GE_LIGHTMODE = 0x5E, GE_LIGHTTYPE0 = 0x5F, GE_LX0 = 0x63, GE_LDX0 = 0x6F, GE_LKA0 = 0x7B,
    GE_LKS0 = 0x87, GE_LKO0 = 0x8B, GE_LAC0 = 0x8F, GE_CULL = 0x9B,
    GE_FRAMEBUFPTR = 0x9C, GE_FRAMEBUFWIDTH = 0x9D, GE_ZBUFPTR = 0x9E, GE_ZBUFWIDTH = 0x9F,
    GE_TEXADDR0 = 0xA0, GE_TEXBUFWIDTH0 = 0xA8, GE_CLUTADDR = 0xB0, GE_CLUTADDRUPPER = 0xB1,
    GE_TRANSFERSRC = 0xB2, GE_TRANSFERSRCW = 0xB3, GE_TRANSFERDST = 0xB4,
    GE_TRANSFERDSTW = 0xB5, GE_TEXSIZE0 = 0xB8, GE_TEXMAPMODE = 0xC0, GE_TEXSHADELS = 0xC1,
    GE_TEXMODE = 0xC2, GE_TEXFORMAT = 0xC3, GE_LOADCLUT = 0xC4, GE_CLUTFORMAT = 0xC5,
    GE_TEXFILTER = 0xC6, GE_TEXWRAP = 0xC7, GE_TEXLEVEL = 0xC8, GE_TEXFUNC = 0xC9,
    GE_TEXENVCOLOR = 0xCA, GE_TEXFLUSH = 0xCB, GE_TEXSYNC = 0xCC, GE_FOG1 = 0xCD,
    GE_FOG2 = 0xCE, GE_FOGCOLOR = 0xCF, GE_FRAMEBUFPIXFORMAT = 0xD2, GE_CLEARMODE = 0xD3,
    GE_SCISSOR1 = 0xD4, GE_SCISSOR2 = 0xD5, GE_MINZ = 0xD6, GE_MAXZ = 0xD7,
    GE_COLORTEST = 0xD8, GE_COLORREF = 0xD9, GE_COLORTESTMASK = 0xDA, GE_ALPHATEST = 0xDB,
    GE_STENCILTEST = 0xDC, GE_STENCILOP = 0xDD, GE_ZTEST = 0xDE, GE_BLENDMODE = 0xDF,
    GE_BLENDFIXEDA = 0xE0, GE_BLENDFIXEDB = 0xE1, GE_DITH0 = 0xE2, GE_LOGICOP = 0xE6,
    GE_ZWRITEDISABLE = 0xE7, GE_MASKRGB = 0xE8, GE_MASKALPHA = 0xE9,
    GE_TRANSFERSTART = 0xEA, GE_TRANSFERSRCPOS = 0xEB, GE_TRANSFERDSTPOS = 0xEC,
    GE_TRANSFERSIZE = 0xEE,
};

typedef struct {
    uint32_t cmd[256];  // last value (24 bits) written to every register
    float world[12], view[12], proj[16], tgen[12], bone[8][12];
    int world_n, view_n, proj_n, tgen_n, bone_n;
    uint32_t base, offset;  // address composition
    uint32_t vaddr, iaddr;
    uint32_t clut[256];  // loaded palette (as 32-bit entries, raw)
    float morph[8];
} GeState;
extern GeState g_ge;

void gpu_set_callbacks(uint32_t signal_fn, uint32_t signal_arg, uint32_t finish_fn,
                       uint32_t finish_arg);
int32_t gpu_enqueue(Ctx *c, uint32_t list, uint32_t stall, int cbid, uint32_t arg, int head);
int32_t gpu_update_stall(Ctx *c, int id, uint32_t stall);
int32_t gpu_sync(Ctx *c, int list_id, int mode);
int gpu_busy(int list_id);
int gpu_run_interrupts(Ctx *c);

// raster.c
void gpu_draw_prim(int prim, int count);
void gpu_block_transfer(void);
void gpu_load_clut(void);
void gpu_tex_invalidate(void);
void raster_flush(void);  // wait for queued primitives  // texture memory may have changed
uint32_t gpu_stat_prims(void);
// internal resolution (1..4); applied at the end of the next display list
void gpu_set_render_scale(int n);
int gpu_render_scale(void);
void gpu_resolve_all(void);
int gpu_scanout_scaled(uint32_t addr, uint32_t stride, uint32_t fmt, uint32_t *out, int max_scale);

// texreplace.c: PPSSPP-compatible texture packs (TEX_DIR) and dumping (TEX_DUMP)
uint32_t *texrep_lookup(uint32_t addr, int fmt, int w, int h, uint32_t texbufwidth, uint32_t texsize,
                        uint32_t clutfmt, const uint32_t *decoded, int *rw, int *rh);
void texrep_clut_loaded(uint32_t bytes);
int texrep_epoch(void);  // changes when the pack is reloaded or dumping toggled  // primitives drawn since the last call

// Address of a register-relative pointer: (BASE high byte << 24) | value
static inline uint32_t ge_addr(uint32_t hi_cmd_val, uint32_t lo24) {
    return ((hi_cmd_val & 0x0F0000u) << 8) | (lo24 & 0xFFFFFFu);
}
static inline float ge_float(uint32_t v24) {
    uint32_t u = v24 << 8;
    float f;
    memcpy(&f, &u, 4);
    return f;
}
