// GE display list processor. Like the real hardware it runs in parallel with
// the CPU: lists are queued and executed in order by a dedicated host thread.
// Finish/signal callbacks are raised as "interrupts" that the scheduler runs
// on whichever guest thread is current (see hle_run_interrupts).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle/hle.h"
#include "glr.h"
#include "gpu.h"

GeState g_ge;

typedef struct {
    int used, done, queued;
    uint64_t seq;  // queue order
    uint32_t pc, stall;
    uint32_t stack[32][2];  // CALL return address + saved offset
    int sp;
    int cbid;
    uint32_t arg;
    int started;
} GeList;

#define MAX_LISTS 64
static GeList g_lists[MAX_LISTS];
static uint32_t g_cb_signal, g_cb_signal_arg, g_cb_finish, g_cb_finish_arg;

void gpu_set_callbacks(uint32_t signal_fn, uint32_t signal_arg, uint32_t finish_fn,
                       uint32_t finish_arg) {
    g_cb_signal = signal_fn;
    g_cb_signal_arg = signal_arg;
    g_cb_finish = finish_fn;
    g_cb_finish_arg = finish_arg;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work = PTHREAD_COND_INITIALIZER;
static uint64_t g_next_seq;

// Pending guest callbacks raised by the GPU thread
#define MAX_INTR 64
static struct { uint32_t fn, a0, a1; } g_intr[MAX_INTR];
static int g_intr_n;

static void raise_interrupt(uint32_t fn, uint32_t a0, uint32_t a1) {
    if (!fn) return;
    pthread_mutex_lock(&g_lock);
    if (g_intr_n < MAX_INTR) {
        g_intr[g_intr_n].fn = fn;
        g_intr[g_intr_n].a0 = a0;
        g_intr[g_intr_n].a1 = a1;
        g_intr_n++;
    }
    pthread_mutex_unlock(&g_lock);
    sched_wake_idle();  // the callbacks run on a guest thread
}

void hle_intr_enter(void);
void hle_intr_leave(void);

// Called by the scheduler (with the big lock held) on the current guest thread.
// Returns 1 if any callback ran.
int gpu_run_interrupts(Ctx *c) {
    int ran = 0;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        if (!g_intr_n) {
            pthread_mutex_unlock(&g_lock);
            return ran;
        }
        uint32_t fn = g_intr[0].fn, a0 = g_intr[0].a0, a1 = g_intr[0].a1;
        memmove(&g_intr[0], &g_intr[1], sizeof g_intr[0] * (size_t)(--g_intr_n));
        pthread_mutex_unlock(&g_lock);
        Ctx tmp = *c;  // run below the thread's stack, as an interrupt would
        tmp.r[4] = a0;
        tmp.r[5] = a1;
        tmp.r[29] = (c->r[29] - 0x800) & ~15u;
        tmp.r[31] = 0;
        hle_intr_enter();
        call_indirect(&tmp, fn);
        hle_intr_leave();
        ran = 1;
    }
}

static uint32_t rel_addr(uint32_t data) {
    uint32_t a = ((g_ge.cmd[GE_BASE] & 0x0F0000u) << 8) | (data & 0xFFFFFFu);
    return (a + g_ge.offset) & 0x0FFFFFFFu;
}

static void load_matrix_word(float *m, int *n, int max, uint32_t data) {
    if (*n < max) m[*n] = ge_float(data);
    (*n)++;
}

// Execute until END/FINISH or the stall address. Returns 1 when the list ended.
static int run_list(GeList *l) {
    for (;;) {
        if (l->stall && l->pc == __atomic_load_n(&l->stall, __ATOMIC_ACQUIRE)) return 0;
        uint32_t w = mem_r32(l->pc), op = w >> 24, data = w & 0xFFFFFFu;
        uint32_t here = l->pc;
        l->pc += 4;
        switch (op) {
        case GE_JUMP:
            l->pc = rel_addr(data) & ~3u;
            continue;
        case GE_BJUMP:  // jump if the last bounding box test failed (we never cull)
            continue;
        case GE_CALL:
            if (l->sp < 32) {
                l->stack[l->sp][0] = l->pc;
                l->stack[l->sp][1] = g_ge.offset;
                l->sp++;
            }
            l->pc = rel_addr(data) & ~3u;
            continue;
        case GE_RET:
            if (l->sp > 0) {
                l->sp--;
                l->pc = l->stack[l->sp][0];
                g_ge.offset = l->stack[l->sp][1];
            }
            continue;
        case GE_SIGNAL: {
            uint32_t next = mem_r32(l->pc);  // always paired with an END
            l->pc += 4;
            uint32_t behaviour = (data >> 16) & 0xFF;
            if (behaviour >= 1 && behaviour <= 3)  // handler signals
                raise_interrupt(g_cb_signal, data & 0xFFFF, g_cb_signal_arg);
            else if (behaviour == 0x10)  // jump
                l->pc = (((data & 0xFFFF) << 16) | (next & 0xFFFF)) & ~3u;
            else if (behaviour == 0x11) {  // call
                if (l->sp < 32) { l->stack[l->sp][0] = l->pc; l->stack[l->sp][1] = g_ge.offset; l->sp++; }
                l->pc = (((data & 0xFFFF) << 16) | (next & 0xFFFF)) & ~3u;
            } else if (behaviour == 0x12 && l->sp > 0) {  // return
                l->sp--;
                l->pc = l->stack[l->sp][0];
                g_ge.offset = l->stack[l->sp][1];
            }
            continue;
        }
        case GE_FINISH:
            g_ge.cmd[op] = data;
            continue;
        case GE_END:
            if ((mem_r32(here - 4) >> 24) == GE_FINISH) {
                raise_interrupt(g_cb_finish, g_ge.cmd[GE_FINISH] & 0xFFFF, g_cb_finish_arg);
                return 1;
            }
            continue;  // an END without FINISH just continues
        case GE_BASE:
            g_ge.cmd[op] = data;
            continue;
        case GE_OFFSETADDR:
            g_ge.offset = data << 8;
            continue;
        case GE_ORIGIN:
            g_ge.offset = here;
            continue;
        case GE_VADDR:
            g_ge.vaddr = rel_addr(data);
            continue;
        case GE_IADDR:
            g_ge.iaddr = rel_addr(data);
            continue;
        case GE_PRIM:
            gpu_draw_prim((int)((data >> 16) & 7), (int)(data & 0xFFFF));
            continue;
        case GE_BEZIER:
        case GE_SPLINE:
            continue;  // not used by the game so far
        case GE_WORLDMATRIXNUMBER: g_ge.world_n = (int)(data & 0xF); continue;
        case GE_WORLDMATRIXDATA: load_matrix_word(g_ge.world, &g_ge.world_n, 12, data); continue;
        case GE_VIEWMATRIXNUMBER: g_ge.view_n = (int)(data & 0xF); continue;
        case GE_VIEWMATRIXDATA: load_matrix_word(g_ge.view, &g_ge.view_n, 12, data); continue;
        case GE_PROJMATRIXNUMBER: g_ge.proj_n = (int)(data & 0xF); continue;
        case GE_PROJMATRIXDATA: load_matrix_word(g_ge.proj, &g_ge.proj_n, 16, data); continue;
        case GE_TGENMATRIXNUMBER: g_ge.tgen_n = (int)(data & 0xF); continue;
        case GE_TGENMATRIXDATA: load_matrix_word(g_ge.tgen, &g_ge.tgen_n, 12, data); continue;
        case GE_BONEMATRIXNUMBER: g_ge.bone_n = (int)(data & 0x7F); continue;
        case GE_BONEMATRIXDATA:
            if (g_ge.bone_n < 96) g_ge.bone[g_ge.bone_n / 12][g_ge.bone_n % 12] = ge_float(data);
            g_ge.bone_n++;
            continue;
        case GE_TRANSFERSTART:
            g_ge.cmd[op] = data;
            gpu_block_transfer();
            gpu_tex_invalidate();
            continue;
        case GE_TEXFLUSH:
            gpu_tex_invalidate();
            continue;
        case GE_LOADCLUT:
            g_ge.cmd[op] = data;
            gpu_load_clut();
            continue;
        default:
            if (op >= GE_MORPHWEIGHT0 && op < GE_MORPHWEIGHT0 + 8) g_ge.morph[op - GE_MORPHWEIGHT0] = ge_float(data);
            g_ge.cmd[op] = data;
            continue;
        }
    }
}

// The list the GPU thread should work on next: oldest queued one.
static GeList *next_list(void) {
    GeList *best = NULL;
    for (int k = 0; k < MAX_LISTS; k++)
        if (g_lists[k].queued && (!best || g_lists[k].seq < best->seq)) best = &g_lists[k];
    return best;
}

uint64_t g_gpu_busy_us;  // time spent executing lists (stats)

// ---------------------------------------------------------------- frame capture
// GE_CAPTURE=seconds: save the first display list started after that guest time,
// with the GE state at its start, VRAM at its start and RAM at its end, to
// GE_CAPTURE_FILE (default ge_capture.bin). gpu_replay() draws it again offline:
// repeatable benchmarks and before/after image comparisons.

#define CAP_VRAM 0x04000000u
#define CAP_VRAM_SIZE 0x00200000u
#define CAP_RAM 0x08000000u
#define CAP_RAM_SIZE 0x02000000u
static GeList *g_cap_list;
static GeState g_cap_ge;
static uint32_t g_cap_pc;
static uint8_t *g_cap_vram;

static void capture_begin(GeList *l) {
    static int done;
    const char *at = getenv("GE_CAPTURE");
    if (done || !at || (double)hle_time_us() / 1e6 < atof(at)) return;
    done = 1;
    g_cap_list = l;
    g_cap_ge = g_ge;
    g_cap_pc = l->pc;
    g_cap_vram = malloc(CAP_VRAM_SIZE);
    memcpy(g_cap_vram, host(CAP_VRAM), CAP_VRAM_SIZE);
}

static void capture_end(GeList *l) {
    if (l != g_cap_list) return;
    g_cap_list = NULL;
    const char *path = getenv("GE_CAPTURE_FILE") ? getenv("GE_CAPTURE_FILE") : "ge_capture.bin";
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t hdr[4] = {0x43454754u /* "TGEC" */, 1, g_cap_pc, (uint32_t)sizeof g_cap_ge};
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(&g_cap_ge, sizeof g_cap_ge, 1, f);
    fwrite(g_cap_vram, CAP_VRAM_SIZE, 1, f);
    fwrite(host(CAP_RAM), CAP_RAM_SIZE, 1, f);
    fclose(f);
    free(g_cap_vram);
    fprintf(stderr, "GE list at %08x captured to %s\n", g_cap_pc, path);
}

// Draw a captured list `iters` times (VRAM/RAM restored once, GE state each
// time) and report the time per frame; optionally save the result as PPM.
int gpu_replay(const char *path, int iters, const char *out_ppm) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "replay: cannot open %s\n", path);
        return 1;
    }
    uint32_t hdr[4];
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != 0x43454754u || hdr[3] != sizeof(GeState)) {
        fprintf(stderr, "replay: not a capture from this build\n");
        fclose(f);
        return 1;
    }
    GeState ge;
    size_t ok = fread(&ge, sizeof ge, 1, f);
    ok += fread(host(CAP_VRAM), CAP_VRAM_SIZE, 1, f);
    ok += fread(host(CAP_RAM), CAP_RAM_SIZE, 1, f);
    fclose(f);
    if (ok != 3) {
        fprintf(stderr, "replay: truncated capture\n");
        return 1;
    }
    uint64_t best = ~0ull, total = 0;
    for (int i = 0; i < iters; i++) {
        g_ge = ge;
        GeList l = {.used = 1, .queued = 1, .pc = hdr[2]};
        uint64_t t0 = hle_time_us();
        run_list(&l);
        raster_flush();
        gpu_resolve_all();
        uint64_t dt = hle_time_us() - t0;
        total += dt;
        if (dt < best) best = dt;
    }
    fprintf(stderr, "replay: %d frames at %dx, best %.2f ms, mean %.2f ms\n", iters, gpu_render_scale(),
            best / 1000.0, total / 1000.0 / iters);
    if (out_ppm) {
        uint32_t fb = 0x04000000u | (g_ge.cmd[GE_FRAMEBUFPTR] & 0xFFFFF0u);
        uint32_t stride = g_ge.cmd[GE_FRAMEBUFWIDTH] & 0x7FC, fmt = g_ge.cmd[GE_FRAMEBUFPIXFORMAT] & 3;
        static uint32_t px[480 * 4 * 272 * 4];
        int S = gpu_scanout_scaled(fb, stride, fmt, px, 4);
        if (!S) {  // 1x: straight from VRAM
            S = 1;
            for (int y = 0; y < 272; y++)
                for (int x = 0; x < 480; x++) {
                    uint32_t a = fb + (uint32_t)(y * (int)stride + x) * (fmt == 3 ? 4u : 2u), v, r, g, b;
                    if (fmt == 3) { v = mem_r32(a); r = v & 255; g = v >> 8 & 255; b = v >> 16 & 255; }
                    else {
                        v = mem_r16(a);
                        if (fmt == 0) { r = (v & 31) << 3; g = (v >> 5 & 63) << 2; b = (v >> 11 & 31) << 3; }
                        else if (fmt == 1) { r = (v & 31) << 3; g = (v >> 5 & 31) << 3; b = (v >> 10 & 31) << 3; }
                        else { r = (v & 15) << 4; g = (v >> 4 & 15) << 4; b = (v >> 8 & 15) << 4; }
                    }
                    px[y * 480 + x] = r | g << 8 | b << 16;
                }
        }
        FILE *o = fopen(out_ppm, "wb");
        if (o) {
            fprintf(o, "P6\n%d %d\n255\n", 480 * S, 272 * S);
            for (int i = 0; i < 480 * S * 272 * S; i++) {
                uint8_t rgb[3] = {(uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16)};
                fwrite(rgb, 3, 1, o);
            }
            fclose(o);
        }
    }
    return 0;
}

static void *gpu_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        GeList *l = next_list();
        if (!l || (l->stall && l->pc == l->stall)) {
            pthread_cond_wait(&g_work, &g_lock);
            continue;
        }
        pthread_mutex_unlock(&g_lock);
        uint64_t t0 = hle_time_us();
        if (!l->started) {
            l->started = 1;
            capture_begin(l);
        }
        int done = run_list(l);
        extern uint32_t g_flush_reason[5];
        if (done) {  // the list is complete only once its pixels are
            g_flush_reason[0]++;
            raster_flush();
            gpu_resolve_all();  // and VRAM holds what was drawn at the internal resolution
            capture_end(l);
            if (g_glr_active) glr_list_end();  // hand the list to the GPU side
        }
        g_gpu_busy_us += hle_time_us() - t0;
        pthread_mutex_lock(&g_lock);
        if (done) {
            l->done = 1;
            l->queued = 0;
        }
        pthread_cond_broadcast(&g_work);
        if (done) {  // a guest thread may be waiting for it (sceGeDrawSync, sceGeListSync)
            pthread_mutex_unlock(&g_lock);
            sched_wake_idle();
            pthread_mutex_lock(&g_lock);
        }
    }
    return NULL;
}

static void start_gpu_thread(void) {
    static int started;
    if (started) return;
    started = 1;
    pthread_t t;
    pthread_create(&t, NULL, gpu_thread, NULL);
    pthread_detach(t);
}

int32_t gpu_enqueue(Ctx *c, uint32_t list, uint32_t stall, int cbid, uint32_t arg, int head) {
    (void)c;
    (void)head;
    start_gpu_thread();
    pthread_mutex_lock(&g_lock);
    int id = -1;
    for (int k = 0; k < MAX_LISTS; k++)
        if (!g_lists[k].used || g_lists[k].done) { id = k; break; }
    if (id < 0) {
        pthread_mutex_unlock(&g_lock);
        return (int32_t)0x80000021u;
    }
    GeList *l = &g_lists[id];
    memset(l, 0, sizeof *l);
    l->used = 1;
    l->queued = 1;
    l->seq = ++g_next_seq;
    l->pc = list & 0x0FFFFFFFu;
    l->stall = stall & 0x0FFFFFFFu;
    l->cbid = cbid;
    l->arg = arg;
    pthread_cond_broadcast(&g_work);
    pthread_mutex_unlock(&g_lock);
    return id;
}

int32_t gpu_update_stall(Ctx *c, int id, uint32_t stall) {
    (void)c;
    if (id < 0 || id >= MAX_LISTS || !g_lists[id].used) return (int32_t)0x80000100u;
    pthread_mutex_lock(&g_lock);
    __atomic_store_n(&g_lists[id].stall, stall & 0x0FFFFFFFu, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_work);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

// 1 while any list is queued (or stalled), or interrupts are waiting to run.
int gpu_busy(int list_id) {
    pthread_mutex_lock(&g_lock);
    int busy = 0;
    if (list_id >= 0 && list_id < MAX_LISTS) busy = g_lists[list_id].queued;
    else
        for (int k = 0; k < MAX_LISTS; k++) busy |= g_lists[k].queued;
    busy |= g_intr_n > 0;
    pthread_mutex_unlock(&g_lock);
    return busy;
}

void hle_wait_gpu(Ctx *c, int list_id);

// mode 0: wait until done; mode 1: just report (0 done, 2 drawing)
int32_t gpu_sync(Ctx *c, int list_id, int mode) {
    if (mode == 1) return gpu_busy(list_id) ? 2 : 0;
    if (gpu_busy(list_id)) hle_wait_gpu(c, list_id);
    return 0;
}
