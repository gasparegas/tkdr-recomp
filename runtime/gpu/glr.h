// GPU renderer: the GE thread (raster.c) records what to draw; the thread that
// owns the GL context (the frontend's main loop) executes it with OpenGL 3.3 /
// OpenGL ES 3.0 (WebGL 2). Vertices are transformed, lit and clipped on the CPU
// exactly as for the software renderer; the GPU rasterizes, textures, tests and
// blends. Every PSP framebuffer is an FBO at the internal resolution.
#pragma once
#include <stdint.h>

typedef struct {
    float x, y, z, w;  // clip space
    float u, v;        // normalized texture coordinates
    uint8_t c[4];      // RGBA
} GlrVtx;

typedef struct {
    uint32_t fb, fb_stride, fb_fmt;
    int16_t sx1, sy1, sx2, sy2;  // scissor, PSP pixels, inclusive
    uint32_t tex_serial;         // 0: untextured
    uint8_t tex_linear, tex_clamp_u, tex_clamp_v, tex_func, tex_rgba, tex_dbl;
    uint8_t alpha_test, at_func, at_ref, at_mask;
    uint8_t z_test, z_func, z_write;
    uint8_t blend, bl_src, bl_dst, bl_eq;
    uint8_t clear, clear_color, clear_alpha, clear_depth;
    uint32_t env, fix_a, fix_b, write_mask;
} GlrState;

enum { GLR_TRIANGLES, GLR_LINES };

// ---- GE thread
extern int g_glr_active;  // the GE thread records for the GPU instead of rasterizing
void glr_state(const GlrState *s);
// texture `serial` (RGBA8888, w x h): sent once, kept by the GPU side until evicted
int glr_has_texture(uint32_t serial);
void glr_texture(uint32_t serial, int w, int h, const uint32_t *pixels);
GlrVtx *glr_vertices(int prim, int n);  // room for n vertices of the current draw
void glr_list_end(void);  // a display list finished: may wait for the GPU side to catch up
// first time a framebuffer is drawn: start its FBO from VRAM (w x h pixels, RGBA)
void glr_fb_init(uint32_t fb, int w, int h, const uint32_t *pixels);

// ---- GL thread
int glr_init(int gles);  // 0 if the GL context cannot run the renderer
void glr_execute(void);  // run everything recorded so far
// draw framebuffer `fb` (480x272 PSP pixels) into the current GL viewport; 0 if no FBO for it
int glr_present(uint32_t fb, int x, int y, int w, int h);
// draw an RGBA image (software renderer output) into the viewport
void glr_present_pixels(const uint32_t *px, int w, int h, int x, int y, int vw, int vh);
void glr_set_scale(int scale);  // FBO resolution multiplier
int glr_read_fb(uint32_t fb, uint32_t *out, int *scale);  // debugging: GL_SHOTS
