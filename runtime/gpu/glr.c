// GPU renderer (see glr.h).
#include "glr.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#elif defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif

int g_glr_active;

// ---------------------------------------------------------------- command stream
// The GE thread writes into a private buffer and hands it over when a display
// list ends; the GL thread takes everything handed over at each frame.

enum { CMD_STATE = 1, CMD_DRAW, CMD_TEX, CMD_TEX_FREE, CMD_FB_INIT };
typedef struct {
    uint32_t type, size;  // size of the payload that follows
} CmdHdr;
typedef struct {
    uint32_t prim, n;
} DrawHdr;
typedef struct {
    uint32_t serial, w, h;
} TexHdr;
typedef struct {
    uint32_t fb, w, h;
} FbHdr;

typedef struct {
    uint8_t *d;
    size_t n, cap;
} Buf;

static Buf g_rec;      // GE thread only
static Buf g_pending;  // handed over, not yet taken (under g_lock)
static Buf g_exec;     // GL thread only
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_taken = PTHREAD_COND_INITIALIZER;
static int g_lists_pending;
static long g_open_draw = -1;  // offset in g_rec of the draw being extended

static void *buf_add(Buf *b, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : (1 << 20);
        while (cap < b->n + n) cap *= 2;
        b->d = realloc(b->d, cap);
        b->cap = cap;
    }
    void *p = b->d + b->n;
    b->n += n;
    return p;
}

static void *cmd(uint32_t type, size_t size) {
    CmdHdr *h = buf_add(&g_rec, sizeof *h + size);
    h->type = type;
    h->size = (uint32_t)size;
    return h + 1;
}

void glr_state(const GlrState *s) {
    g_open_draw = -1;
    memcpy(cmd(CMD_STATE, sizeof *s), s, sizeof *s);
}

GlrVtx *glr_vertices(int prim, int n) {
    if (g_open_draw >= 0) {
        DrawHdr *d = (DrawHdr *)(g_rec.d + g_open_draw + sizeof(CmdHdr));
        if ((int)d->prim == prim) {
            void *p = buf_add(&g_rec, sizeof(GlrVtx) * (size_t)n);
            d = (DrawHdr *)(g_rec.d + g_open_draw + sizeof(CmdHdr));  // buf_add may move it
            d->n += (uint32_t)n;
            ((CmdHdr *)(g_rec.d + g_open_draw))->size += (uint32_t)(sizeof(GlrVtx) * (size_t)n);
            return p;
        }
    }
    g_open_draw = (long)g_rec.n;
    DrawHdr *d = cmd(CMD_DRAW, sizeof *d + sizeof(GlrVtx) * (size_t)n);
    d->prim = (uint32_t)prim;
    d->n = (uint32_t)n;
    return (GlrVtx *)(d + 1);
}

// Textures the GL side holds, by serial, least recently used evicted first.
#define TEX_SLOTS 2048
#define TEX_KEEP 1024
static struct {
    uint32_t serial, used;
} g_tex_slots[TEX_SLOTS];
static int g_tex_count;
static uint32_t g_tex_tick;

static int tex_find(uint32_t serial) {
    for (uint32_t k = serial * 2654435761u % TEX_SLOTS, i = 0; i < TEX_SLOTS; i++, k = (k + 1) % TEX_SLOTS) {
        if (g_tex_slots[k].serial == serial) return (int)k;
        if (!g_tex_slots[k].serial) return -1 - (int)k;
    }
    return -TEX_SLOTS - 1;
}

static void tex_forget(uint32_t serial) {  // remove from the open-addressing table
    int k = tex_find(serial);
    if (k < 0) return;
    g_tex_slots[k].serial = 0;
    g_tex_count--;
    // re-insert the cluster that follows
    for (int j = (k + 1) % TEX_SLOTS; g_tex_slots[j].serial; j = (j + 1) % TEX_SLOTS) {
        uint32_t s = g_tex_slots[j].serial, u = g_tex_slots[j].used;
        g_tex_slots[j].serial = 0;
        int f = -1 - tex_find(s);
        g_tex_slots[f].serial = s;
        g_tex_slots[f].used = u;
    }
}

int glr_has_texture(uint32_t serial) {
    int k = tex_find(serial);
    if (k < 0) return 0;
    g_tex_slots[k].used = ++g_tex_tick;
    return 1;
}

void glr_texture(uint32_t serial, int w, int h, const uint32_t *pixels) {
    if (g_tex_count >= TEX_KEEP) {  // evict the least recently used
        int best = -1;
        for (int k = 0; k < TEX_SLOTS; k++)
            if (g_tex_slots[k].serial && (best < 0 || g_tex_slots[k].used < g_tex_slots[best].used)) best = k;
        uint32_t old = g_tex_slots[best].serial;
        tex_forget(old);
        *(uint32_t *)cmd(CMD_TEX_FREE, 4) = old;
    }
    int f = -1 - tex_find(serial);
    g_tex_slots[f].serial = serial;
    g_tex_slots[f].used = ++g_tex_tick;
    g_tex_count++;
    g_open_draw = -1;
    TexHdr *t = cmd(CMD_TEX, sizeof *t + (size_t)w * (size_t)h * 4);
    t->serial = serial;
    t->w = (uint32_t)w;
    t->h = (uint32_t)h;
    memcpy(t + 1, pixels, (size_t)w * (size_t)h * 4);
}

void glr_fb_init(uint32_t fb, int w, int h, const uint32_t *pixels) {
    g_open_draw = -1;
    FbHdr *f = cmd(CMD_FB_INIT, sizeof *f + (size_t)w * (size_t)h * 4);
    f->fb = fb;
    f->w = (uint32_t)w;
    f->h = (uint32_t)h;
    memcpy(f + 1, pixels, (size_t)w * (size_t)h * 4);
}

void glr_list_end(void) {
    g_open_draw = -1;
    pthread_mutex_lock(&g_lock);
    if (!g_pending.n) {
        Buf t = g_pending;
        g_pending = g_rec;
        g_rec = t;
    } else {
        memcpy(buf_add(&g_pending, g_rec.n), g_rec.d, g_rec.n);
    }
    g_rec.n = 0;
    g_lists_pending++;
    // don't run more than a couple of lists ahead of what the GPU has taken
    while (g_lists_pending > 3 && g_glr_active) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&g_taken, &g_lock, &ts);
    }
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------- GL side

static int g_gles, g_scale = 1;
static GLuint g_prog, g_vao, g_vbo, g_samplers[8];
static GLuint g_blank;  // 1x1 texture bound when nothing is sampled (WebGL rejects a
                        // draw while the target FBO's own texture is bound, even unused)
static GLuint g_present_prog, g_present_vao, g_present_vbo, g_present_tex;
static int g_present_tw, g_present_th;
static struct {
    GLint tex_on, func, rgba, dbl, env, atest, afunc, aref, amask, srcfix, fixa, tex;
} U;
static GLint g_present_u_tex;

#define FB_LOGICAL 512  // FBOs cover 512 x 512 PSP pixels
typedef struct {
    uint32_t fb;
    GLuint fbo, color, depth;
} Fbo;
#define MAX_FBOS 16
static Fbo g_fbos[MAX_FBOS];
static int g_fbo_created;  // fbo_get changed GL bindings behind the state cache

#define GPU_TEX_SLOTS 4096
static struct {
    uint32_t serial;
    GLuint tex;
} g_gpu_tex[GPU_TEX_SLOTS];

static GLuint gpu_tex_get(uint32_t serial) {
    for (uint32_t k = serial * 2654435761u % GPU_TEX_SLOTS, i = 0; i < GPU_TEX_SLOTS; i++, k = (k + 1) % GPU_TEX_SLOTS) {
        if (g_gpu_tex[k].serial == serial) return g_gpu_tex[k].tex;
        if (!g_gpu_tex[k].serial) return 0;
    }
    return 0;
}

static void gpu_tex_put(uint32_t serial, GLuint tex) {
    for (uint32_t k = serial * 2654435761u % GPU_TEX_SLOTS, i = 0; i < GPU_TEX_SLOTS; i++, k = (k + 1) % GPU_TEX_SLOTS)
        if (!g_gpu_tex[k].serial || g_gpu_tex[k].serial == serial) {
            g_gpu_tex[k].serial = serial;
            g_gpu_tex[k].tex = tex;
            return;
        }
}

static void gpu_tex_del(uint32_t serial) {
    uint32_t k = serial * 2654435761u % GPU_TEX_SLOTS;
    for (uint32_t i = 0; i < GPU_TEX_SLOTS; i++, k = (k + 1) % GPU_TEX_SLOTS) {
        if (!g_gpu_tex[k].serial) return;
        if (g_gpu_tex[k].serial == serial) break;
    }
    if (g_gpu_tex[k].serial != serial) return;
    glDeleteTextures(1, &g_gpu_tex[k].tex);
    g_gpu_tex[k].serial = 0;
    for (uint32_t j = (k + 1) % GPU_TEX_SLOTS; g_gpu_tex[j].serial; j = (j + 1) % GPU_TEX_SLOTS) {
        uint32_t s = g_gpu_tex[j].serial;
        GLuint t = g_gpu_tex[j].tex;
        g_gpu_tex[j].serial = 0;
        gpu_tex_put(s, t);
    }
}

static const char *k_vs =
    "in vec4 a_pos;\n"
    "in vec2 a_uv;\n"
    "in vec4 a_col;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_col;\n"
    "void main() { gl_Position = a_pos; v_uv = a_uv; v_col = a_col; }\n";

// The GE's texture functions and alpha test (raster.c shade() is the reference)
static const char *k_fs =
    "uniform sampler2D u_tex;\n"
    "uniform int u_tex_on, u_func, u_rgba, u_dbl, u_atest, u_afunc, u_aref, u_amask, u_srcfix;\n"
    "uniform vec3 u_env, u_fixa;\n"
    "in vec2 v_uv;\n"
    "in vec4 v_col;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "  vec4 c = v_col;\n"
    "  if (u_tex_on != 0) {\n"
    "    vec4 t = texture(u_tex, v_uv);\n"
    "    if (u_func == 0) { c.rgb *= t.rgb; if (u_rgba != 0) c.a *= t.a; }\n"
    "    else if (u_func == 1) { c.rgb = u_rgba != 0 ? mix(c.rgb, t.rgb, t.a) : t.rgb; }\n"
    "    else if (u_func == 2) { c.rgb = mix(c.rgb, u_env, t.rgb); if (u_rgba != 0) c.a *= t.a; }\n"
    "    else if (u_func == 3) { c.rgb = t.rgb; if (u_rgba != 0) c.a = t.a; }\n"
    "    else { c.rgb = min(c.rgb + t.rgb, vec3(1.0)); if (u_rgba != 0) c.a *= t.a; }\n"
    "    if (u_dbl != 0) c.rgb = min(c.rgb * 2.0, vec3(1.0));\n"
    "  }\n"
    "  if (u_atest != 0) {\n"
    "    int a = int(c.a * 255.0 + 0.5) & u_amask, r = u_aref & u_amask;\n"
    "    bool pass;\n"
    "    if (u_afunc == 0) pass = false;\n"
    "    else if (u_afunc == 1) pass = true;\n"
    "    else if (u_afunc == 2) pass = a == r;\n"
    "    else if (u_afunc == 3) pass = a != r;\n"
    "    else if (u_afunc == 4) pass = a < r;\n"
    "    else if (u_afunc == 5) pass = a <= r;\n"
    "    else if (u_afunc == 6) pass = a > r;\n"
    "    else pass = a >= r;\n"
    "    if (!pass) discard;\n"
    "  }\n"
    "  if (u_srcfix != 0) c.rgb *= u_fixa;  // fixed source blend factor\n"
    "  gl_FragDepth = floor(gl_FragCoord.z * 65535.0) / 65535.0;  // the GE's 16-bit depth: coplanar layers tie as on the PSP\n"
    "  o_col = c;\n"
    "}\n";

static const char *k_present_vs =
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";
static const char *k_present_fs =
    "uniform sampler2D u_tex;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_col;\n"
    "void main() { o_col = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n";

// Some drivers (Apple's) judge completeness from the texture's own parameters
// even when a sampler object is bound: give every texture a single level.
static void tex_single_level(void) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

static GLuint compile(GLenum type, const char *src) {
    const char *head = g_gles ? "#version 300 es\nprecision highp float;\nprecision highp int;\n" : "#version 330 core\n";
    const char *parts[2] = {head, src};
    GLuint s = glCreateShader(type);
    glShaderSource(s, 2, parts, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "glr: shader error:\n%s\n", log);
        return 0;
    }
    return s;
}

static GLuint link(const char *vs, const char *fs, const char *const *attribs, int nattribs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    for (int i = 0; i < nattribs; i++) glBindAttribLocation(p, (GLuint)i, attribs[i]);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "glr: link error:\n%s\n", log);
        return 0;
    }
    return p;
}

int glr_init(int gles) {
    g_gles = gles;
    static const char *const attribs[] = {"a_pos", "a_uv", "a_col"};
    g_prog = link(k_vs, k_fs, attribs, 3);
    static const char *const pattribs[] = {"a_pos", "a_uv"};
    g_present_prog = link(k_present_vs, k_present_fs, pattribs, 2);
    if (!g_prog || !g_present_prog) return 0;
#define LOC(n) U.n = glGetUniformLocation(g_prog, "u_" #n)
    LOC(tex_on); LOC(func); LOC(rgba); LOC(dbl); LOC(env); LOC(atest); LOC(afunc); LOC(aref); LOC(amask);
    LOC(srcfix); LOC(fixa); LOC(tex);
#undef LOC
    g_present_u_tex = glGetUniformLocation(g_present_prog, "u_tex");

    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GlrVtx), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GlrVtx), (void *)16);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GlrVtx), (void *)24);

    glGenVertexArrays(1, &g_present_vao);
    glBindVertexArray(g_present_vao);
    glGenBuffers(1, &g_present_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_present_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glBindVertexArray(0);

    glGenSamplers(8, g_samplers);
    for (int k = 0; k < 8; k++) {
        GLint filt = (k & 1) ? GL_LINEAR : GL_NEAREST;
        glSamplerParameteri(g_samplers[k], GL_TEXTURE_MIN_FILTER, filt);
        glSamplerParameteri(g_samplers[k], GL_TEXTURE_MAG_FILTER, filt);
        glSamplerParameteri(g_samplers[k], GL_TEXTURE_WRAP_S, (k & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(g_samplers[k], GL_TEXTURE_WRAP_T, (k & 4) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    }
    static const uint32_t white = 0xFFFFFFFFu;
    glGenTextures(1, &g_blank);
    glBindTexture(GL_TEXTURE_2D, g_blank);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    tex_single_level();
    fprintf(stderr, "glr: GPU renderer ready (%s)\n", (const char *)glGetString(GL_VERSION));
    return 1;
}

static Fbo *fbo_get(uint32_t fb) {
    fb &= 0x0FFFFFF0u;
    Fbo *freeslot = NULL;
    for (int k = 0; k < MAX_FBOS; k++) {
        if (g_fbos[k].fbo && g_fbos[k].fb == fb) return &g_fbos[k];
        if (!g_fbos[k].fbo && !freeslot) freeslot = &g_fbos[k];
    }
    if (!freeslot) {  // recycle the first one
        freeslot = &g_fbos[0];
        glDeleteFramebuffers(1, &freeslot->fbo);
        glDeleteTextures(1, &freeslot->color);
        glDeleteRenderbuffers(1, &freeslot->depth);
        memset(freeslot, 0, sizeof *freeslot);
    }
    Fbo *f = freeslot;
    int size = FB_LOGICAL * g_scale;
    f->fb = fb;
    g_fbo_created = 1;
    glGenTextures(1, &f->color);
    glBindTexture(GL_TEXTURE_2D, f->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    tex_single_level();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &f->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, f->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
    glGenFramebuffers(1, &f->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, f->depth);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClearDepthf(0);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return f;
}

void glr_set_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale == g_scale) return;
    g_scale = scale;
    for (int k = 0; k < MAX_FBOS; k++)
        if (g_fbos[k].fbo) {
            glDeleteFramebuffers(1, &g_fbos[k].fbo);
            glDeleteTextures(1, &g_fbos[k].color);
            glDeleteRenderbuffers(1, &g_fbos[k].depth);
            memset(&g_fbos[k], 0, sizeof g_fbos[k]);
        }
}

static const GLenum k_zfunc[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};

// PSP blend factor -> GL. Source and destination differ only for codes 0/1;
// the doubled-alpha factors are approximated by the plain ones.
static GLenum blend_factor(int f, int is_src) {
    switch (f) {
    case 0: return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 1: return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 2: case 6: return GL_SRC_ALPHA;
    case 3: case 7: return GL_ONE_MINUS_SRC_ALPHA;
    case 4: case 8: return GL_DST_ALPHA;
    case 5: case 9: return GL_ONE_MINUS_DST_ALPHA;
    default: return is_src ? GL_ONE : GL_CONSTANT_COLOR;  // fixed: source done in the shader
    }
}

static void rgb_of(uint32_t c, float *o) {
    o[0] = (float)(c & 0xFF) / 255.f;
    o[1] = (float)(c >> 8 & 0xFF) / 255.f;
    o[2] = (float)(c >> 16 & 0xFF) / 255.f;
}

// Last applied values, to skip redundant GL calls (they are slow in WebGL).
static struct {
    GLuint fbo, tex, sampler;
    int scissor[4], depth_test, depth_func, depth_mask, blend, bsrc, bdst, beq, cmask;
    uint32_t blend_color;
    int tex_on, func, rgba, dbl, atest, afunc, aref, amask, srcfix;
    uint32_t env, fixa;
    int valid;
} S;

static void apply(const GlrState *st) {
    Fbo *f = fbo_get(st->fb);
    if (g_fbo_created) {
        g_fbo_created = 0;
        S.valid = 0;
    }
    int size = FB_LOGICAL * g_scale;
    if (!S.valid || S.fbo != f->fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
        glViewport(0, 0, size, size);
        S.fbo = f->fbo;
    }
    int sc[4] = {st->sx1 * g_scale, st->sy1 * g_scale, (st->sx2 - st->sx1 + 1) * g_scale,
                 (st->sy2 - st->sy1 + 1) * g_scale};
    if (sc[2] < 0) sc[2] = 0;
    if (sc[3] < 0) sc[3] = 0;
    if (!S.valid || memcmp(sc, S.scissor, sizeof sc)) {
        glScissor(sc[0], sc[1], sc[2], sc[3]);
        memcpy(S.scissor, sc, sizeof sc);
    }
    int clear = st->clear;
    int depth_test = clear ? 1 : st->z_test;
    int depth_func = clear ? 1 : st->z_func;
    int depth_mask = clear ? st->clear_depth : (st->z_test && st->z_write);
    if (!S.valid || S.depth_test != depth_test) {
        if (depth_test) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);
        S.depth_test = depth_test;
    }
    if (!S.valid || S.depth_func != depth_func) {
        glDepthFunc(k_zfunc[depth_func & 7]);
        S.depth_func = depth_func;
    }
    if (!S.valid || S.depth_mask != depth_mask) {
        glDepthMask((GLboolean)depth_mask);
        S.depth_mask = depth_mask;
    }
    // colour write mask: a channel is written unless fully masked
    int cmask;
    if (clear) cmask = (st->clear_color ? 7 : 0) | (st->clear_alpha ? 8 : 0);
    else {
        uint32_t m = st->write_mask;
        cmask = ((m & 0xFF) != 0xFF ? 1 : 0) | ((m >> 8 & 0xFF) != 0xFF ? 2 : 0) | ((m >> 16 & 0xFF) != 0xFF ? 4 : 0) |
                ((m >> 24) != 0xFF ? 8 : 0);
        if (st->blend) cmask &= 7;  // blending never changes alpha (it holds the stencil)
    }
    if (!S.valid || S.cmask != cmask) {
        glColorMask(cmask & 1, (cmask >> 1) & 1, (cmask >> 2) & 1, (cmask >> 3) & 1);
        S.cmask = cmask;
    }
    int blend = !clear && st->blend;
    if (!S.valid || S.blend != blend) {
        if (blend) glEnable(GL_BLEND);
        else glDisable(GL_BLEND);
        S.blend = blend;
    }
    int srcfix = 0;
    if (blend) {
        int bsrc = (int)blend_factor(st->bl_src, 1), bdst = (int)blend_factor(st->bl_dst, 0);
        static const GLenum eqs[6] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_ADD};
        int beq = (int)eqs[st->bl_eq < 6 ? st->bl_eq : 0];
        srcfix = st->bl_src >= 10;
        if (!S.valid || S.bsrc != bsrc || S.bdst != bdst) {
            glBlendFuncSeparate((GLenum)bsrc, (GLenum)bdst, GL_ZERO, GL_ONE);
            S.bsrc = bsrc;
            S.bdst = bdst;
        }
        if (!S.valid || S.beq != beq) {
            glBlendEquation((GLenum)beq);
            S.beq = beq;
        }
        if (st->bl_dst >= 10 && (!S.valid || S.blend_color != st->fix_b)) {
            float c[3];
            rgb_of(st->fix_b, c);
            glBlendColor(c[0], c[1], c[2], 1.f);
            S.blend_color = st->fix_b;
        }
    }
    // shader inputs
    int tex_on = !clear && st->tex_serial != 0;
    GLuint tex = tex_on ? gpu_tex_get(st->tex_serial) : 0;
    if (tex_on && !tex) tex_on = 0;  // never uploaded (should not happen)
#define SETI(field, val) \
    if (!S.valid || S.field != (val)) { glUniform1i(U.field, (val)); S.field = (val); }
    SETI(tex_on, tex_on);
    if (tex_on) {
        SETI(func, st->tex_func);
        SETI(rgba, st->tex_rgba);
        SETI(dbl, st->tex_dbl);
        if (!S.valid || S.env != st->env) {
            float c[3];
            rgb_of(st->env, c);
            glUniform3f(U.env, c[0], c[1], c[2]);
            S.env = st->env;
        }
        if (!S.valid || S.tex != tex) {
            glBindTexture(GL_TEXTURE_2D, tex);
            S.tex = tex;
        }
        GLuint sm = g_samplers[(st->tex_linear ? 1 : 0) | (st->tex_clamp_u ? 2 : 0) | (st->tex_clamp_v ? 4 : 0)];
        if (!S.valid || S.sampler != sm) {
            glBindSampler(0, sm);
            S.sampler = sm;
        }
    }
    int atest = !clear && st->alpha_test;
    if (!tex_on && S.tex != g_blank) {
        glBindTexture(GL_TEXTURE_2D, g_blank);
        S.tex = g_blank;
    }
    SETI(atest, atest);
    if (atest) {
        SETI(afunc, st->at_func);
        SETI(aref, st->at_ref);
        SETI(amask, st->at_mask);
    }
    SETI(srcfix, srcfix);
    if (srcfix && (!S.valid || S.fixa != st->fix_a)) {
        float c[3];
        rgb_of(st->fix_a, c);
        glUniform3f(U.fixa, c[0], c[1], c[2]);
        S.fixa = st->fix_a;
    }
#undef SETI
    S.valid = 1;
}

static uint8_t *g_verts;  // all vertices of one execute, uploaded at once
static size_t g_verts_cap;

void glr_execute(void) {
    pthread_mutex_lock(&g_lock);
    Buf t = g_exec;
    g_exec = g_pending;
    g_pending = t;
    g_pending.n = 0;
    g_lists_pending = 0;
    pthread_cond_broadcast(&g_taken);
    pthread_mutex_unlock(&g_lock);
    if (!g_exec.n) return;

    // pass 1: gather the vertices into one buffer
    size_t nv = 0;
    for (size_t o = 0; o < g_exec.n;) {
        CmdHdr *h = (CmdHdr *)(g_exec.d + o);
        if (h->type == CMD_DRAW) nv += ((DrawHdr *)(h + 1))->n;
        o += sizeof *h + h->size;
    }
    if (nv * sizeof(GlrVtx) > g_verts_cap) {
        g_verts_cap = nv * sizeof(GlrVtx) * 2;
        g_verts = realloc(g_verts, g_verts_cap);
    }
    size_t at = 0;
    for (size_t o = 0; o < g_exec.n;) {
        CmdHdr *h = (CmdHdr *)(g_exec.d + o);
        if (h->type == CMD_DRAW) {
            DrawHdr *d = (DrawHdr *)(h + 1);
            memcpy(g_verts + at * sizeof(GlrVtx), d + 1, d->n * sizeof(GlrVtx));
            at += d->n;
        }
        o += sizeof *h + h->size;
    }
    glUseProgram(g_prog);
    glUniform1i(U.tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * sizeof(GlrVtx)), g_verts, GL_STREAM_DRAW);
    glEnable(GL_SCISSOR_TEST);
    S.valid = 0;
    glBindTexture(GL_TEXTURE_2D, g_blank);

    // pass 2: run
    const GlrState *st = NULL;
    size_t first = 0;
    for (size_t o = 0; o < g_exec.n;) {
        CmdHdr *h = (CmdHdr *)(g_exec.d + o);
        void *p = h + 1;
        switch (h->type) {
        case CMD_STATE: st = p; break;
        case CMD_DRAW: {
            DrawHdr *d = p;
            if (st) {
                apply(st);
                glDrawArrays(d->prim == GLR_LINES ? GL_LINES : GL_TRIANGLES, (GLint)first, (GLsizei)d->n);
            }
            first += d->n;
            break;
        }
        case CMD_TEX: {
            TexHdr *th = p;
            GLuint tex;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)th->w, (GLsizei)th->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, th + 1);
            tex_single_level();
            gpu_tex_put(th->serial, tex);
            glBindTexture(GL_TEXTURE_2D, g_blank);
            S.tex = g_blank;
            break;
        }
        case CMD_TEX_FREE: {
            uint32_t serial = *(uint32_t *)p;
            GLuint tex = gpu_tex_get(serial);
            if (tex && S.tex == tex) S.tex = 0;
            gpu_tex_del(serial);
            break;
        }
        case CMD_FB_INIT: {  // VRAM contents -> FBO (drawn as a textured quad at the FBO's scale)
            FbHdr *fh = p;
            Fbo *f = fbo_get(fh->fb);
            GLuint tex;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)fh->w, (GLsizei)fh->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, fh + 1);
            tex_single_level();
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
            int size = FB_LOGICAL * g_scale;
            glViewport(0, 0, (GLsizei)fh->w * g_scale, (GLsizei)fh->h * g_scale);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glColorMask(1, 1, 1, 1);
            glUseProgram(g_present_prog);
            glBindSampler(0, 0);
            glUniform1i(g_present_u_tex, 0);
            // rows go down in PSP memory and up in GL: v = 0 at the bottom of the quad
            float q[16] = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
            glBindVertexArray(g_present_vao);
            glBindBuffer(GL_ARRAY_BUFFER, g_present_vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDeleteTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, g_blank);
            (void)size;
            // back to drawing
            glUseProgram(g_prog);
            glBindVertexArray(g_vao);
            glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
            glEnable(GL_SCISSOR_TEST);
            S.valid = 0;
            break;
        }
        }
        o += sizeof *h + h->size;
    }
    glBindVertexArray(0);
    glBindSampler(0, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_exec.n = 0;
}

static void present_quad(GLuint tex, float u1, float v1, int x, int y, int w, int h, int flip) {
    glViewport(x, y, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(g_present_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, 0);
    glUniform1i(g_present_u_tex, 0);
    // the screen's top shows v = 0 for FBOs (PSP row 0 is GL row 0)
    float vt = flip ? 0.f : v1, vb = flip ? v1 : 0.f;
    float q[16] = {-1, -1, 0, vb, 1, -1, u1, vb, -1, 1, 0, vt, 1, 1, u1, vt};
    glBindVertexArray(g_present_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_present_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

// Read framebuffer `fb` back (480x272 times the scale, RGBA, top row first); 0 if no FBO.
int glr_read_fb(uint32_t fb, uint32_t *out, int *scale) {
    fb &= 0x0FFFFFF0u;
    for (int k = 0; k < MAX_FBOS; k++)
        if (g_fbos[k].fbo && g_fbos[k].fb == fb) {
            glBindFramebuffer(GL_FRAMEBUFFER, g_fbos[k].fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, 480 * g_scale, 272 * g_scale, GL_RGBA, GL_UNSIGNED_BYTE, out);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            *scale = g_scale;
            return 1;
        }
    return 0;
}

int glr_present(uint32_t fb, int x, int y, int w, int h) {
    fb &= 0x0FFFFFF0u;
    for (int k = 0; k < MAX_FBOS; k++)
        if (g_fbos[k].fbo && g_fbos[k].fb == fb) {
            present_quad(g_fbos[k].color, 480.f / FB_LOGICAL, 272.f / FB_LOGICAL, x, y, w, h, 1);
            return 1;
        }
    return 0;
}

void glr_present_pixels(const uint32_t *px, int w, int h, int x, int y, int vw, int vh) {
    if (!g_present_tex) glGenTextures(1, &g_present_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_present_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w != g_present_tw || h != g_present_th) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        tex_single_level();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, w > 480 ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_present_tw = w;
        g_present_th = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    present_quad(g_present_tex, 1.f, 1.f, x, y, vw, vh, 1);
}
