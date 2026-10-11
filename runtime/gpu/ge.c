// GE display list processor. Lists run synchronously when enqueued (or when
// their stall address moves), so sceGeDrawSync/ListSync always find them done.
#include <stdio.h>

#include "../hle/hle.h"
#include "gpu.h"

GeState g_ge;

typedef struct {
    int used, done;
    uint32_t pc, stall;
    uint32_t stack[32][2];  // CALL return address + saved offset
    int sp;
    int cbid;
    uint32_t arg;
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

void hle_intr_enter(void);
void hle_intr_leave(void);

// Run a guest callback on the current thread, below its stack pointer, as
// an interrupt handler would (no thread switches until it returns).
static void run_callback(Ctx *c, uint32_t fn, uint32_t a0, uint32_t a1) {
    if (!fn) return;
    Ctx tmp = *c;
    tmp.r[4] = a0;
    tmp.r[5] = a1;
    tmp.r[29] = (c->r[29] - 0x800) & ~15u;
    tmp.r[31] = 0;
    hle_intr_enter();
    call_indirect(&tmp, fn);
    hle_intr_leave();
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
static int run_list(Ctx *c, GeList *l) {
    for (;;) {
        if (l->stall && l->pc == l->stall) return 0;
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
                run_callback(c, g_cb_signal, data & 0xFFFF, g_cb_signal_arg);
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
                run_callback(c, g_cb_finish, g_ge.cmd[GE_FINISH] & 0xFFFF, g_cb_finish_arg);
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

int32_t gpu_enqueue(Ctx *c, uint32_t list, uint32_t stall, int cbid, uint32_t arg, int head) {
    (void)head;
    int id = -1;
    for (int k = 0; k < MAX_LISTS; k++)
        if (!g_lists[k].used || g_lists[k].done) { id = k; break; }
    if (id < 0) return (int32_t)0x80000021u;
    GeList *l = &g_lists[id];
    memset(l, 0, sizeof *l);
    l->used = 1;
    l->pc = list & 0x0FFFFFFFu;
    l->stall = stall & 0x0FFFFFFFu;
    l->cbid = cbid;
    l->arg = arg;
    g_ge.offset = 0;
    l->done = run_list(c, l);
    return id;
}

int32_t gpu_update_stall(Ctx *c, int id, uint32_t stall) {
    if (id < 0 || id >= MAX_LISTS || !g_lists[id].used) return (int32_t)0x80000100u;
    GeList *l = &g_lists[id];
    l->stall = stall & 0x0FFFFFFFu;
    if (!l->done) l->done = run_list(c, l);
    return 0;
}

int32_t gpu_sync(Ctx *c) {
    (void)c;
    return 0;  // everything already ran
}
