// PSP kernel threads and synchronisation (ThreadManForUser).
//
// Every PSP thread runs on its own host pthread, but only the thread holding
// the "baton" (g_cur) executes guest code: this reproduces the PSP's single
// core, priority-based, non-time-sliced scheduler. The big lock is held by
// whoever runs guest code; everyone else sleeps on their own condvar.
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "../func_table.h"
#include "hle.h"

enum { ST_RUNNING = 1, ST_READY = 2, ST_WAITING = 4, ST_DORMANT = 16, ST_DEAD = 32 };
enum {
    W_NONE, W_SLEEP, W_DELAY, W_THREAD_END, W_SEMA, W_EVENT, W_VBLANK, W_IO,
};
enum { OBJ_THREAD = 1, OBJ_SEMA, OBJ_EVENT, OBJ_FPL, OBJ_CALLBACK };

#define MAX_OBJ 1024
#define OBJ_ID_BASE 0x1000

typedef struct Thread Thread;

typedef struct {
    int type;
    char name[32];
    uint32_t attr;
} ObjHeader;

struct Thread {
    ObjHeader h;
    int id;
    uint32_t entry, init_prio, prio, stack_size, stack, gp;
    int status;
    uint64_t seq;  // FIFO order among equal priorities
    Ctx ctx;
    pthread_t host;
    pthread_cond_t cv;
    int host_alive, killed;
    // wait state
    int wait_type, wait_id;
    uint64_t deadline;  // 0 = none
    uint32_t timeout_ptr;
    uint32_t wait_a, wait_b, wait_c;  // per-wait parameters
    int32_t wait_result;
    int wakeup_count;
    uint32_t exit_status;
    uint32_t arglen, argp;
};

typedef struct {
    ObjHeader h;
    int32_t count, max;
} Sema;

typedef struct {
    ObjHeader h;
    uint32_t init, pattern;
} EventFlag;

typedef struct {
    ObjHeader h;
    uint32_t base, block_size, nblocks;
    uint8_t *used;
} Fpl;

typedef struct {
    ObjHeader h;
    uint32_t func, arg;
    int owner;
} Callback;

static void *g_obj[MAX_OBJ];
static pthread_mutex_t g_big = PTHREAD_MUTEX_INITIALIZER;
static Thread *g_cur;
static uint64_t g_seq;
static __thread Thread *tls_self;
static pthread_cond_t g_all_done = PTHREAD_COND_INITIALIZER;
static uint64_t g_boot_ns;

uint64_t hle_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    if (!g_boot_ns) g_boot_ns = ns;
    return (ns - g_boot_ns) / 1000u;
}

#define VBLANK_US 16683u
static uint64_t vblank_count(void) { return hle_time_us() / VBLANK_US; }

// ---------------------------------------------------------------- objects

static int obj_new(void *o, int type, uint32_t name_ptr, uint32_t attr) {
    for (int k = 1; k < MAX_OBJ; k++)
        if (!g_obj[k]) {
            ObjHeader *h = o;
            h->type = type;
            h->attr = attr;
            if (name_ptr) {
                strncpy(h->name, (const char *)host(name_ptr), 31);
                h->name[31] = 0;
            }
            g_obj[k] = o;
            return OBJ_ID_BASE + k;
        }
    return -1;
}

static void *obj_get(int id, int type) {
    int k = id - OBJ_ID_BASE;
    if (k <= 0 || k >= MAX_OBJ || !g_obj[k]) return NULL;
    return ((ObjHeader *)g_obj[k])->type == type ? g_obj[k] : NULL;
}

static void obj_del(int id) { g_obj[id - OBJ_ID_BASE] = NULL; }

#define FOR_EACH_THREAD(t)                                                          \
    for (int k_ = 1; k_ < MAX_OBJ; k_++)                                            \
        for (Thread *t = g_obj[k_] && ((ObjHeader *)g_obj[k_])->type == OBJ_THREAD \
                             ? g_obj[k_] : NULL; t; t = NULL)

// ---------------------------------------------------------------- scheduler

static void make_ready(Thread *t, int32_t result) {
    t->status = ST_READY;
    t->wait_type = W_NONE;
    t->wait_result = result;
    t->seq = ++g_seq;
    if (t->timeout_ptr) {
        uint64_t now = hle_time_us();
        uint32_t left = t->deadline > now ? (uint32_t)(t->deadline - now) : 0;
        mem_w32(t->timeout_ptr, result == (int32_t)SCE_KERNEL_ERROR_WAIT_TIMEOUT ? 0 : left);
        t->timeout_ptr = 0;
    }
    t->deadline = 0;
}

static Thread *pick_next(void) {
    Thread *best = NULL;
    FOR_EACH_THREAD(t) {
        if (t->status != ST_READY && t->status != ST_RUNNING) continue;
        if (!best || t->prio < best->prio || (t->prio == best->prio && t->seq < best->seq)) best = t;
    }
    return best;
}

static void wake_waiters(int wait_type, int id, int32_t result);
static void events_locked(void);

static void *host_thread_main(void *arg);

static void start_host(Thread *t) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 4u << 20);
    t->host_alive = 1;
    t->killed = 0;
    if (pthread_create(&t->host, &a, host_thread_main, t) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        exit(1);
    }
    pthread_detach(t->host);
    pthread_attr_destroy(&a);
}

// Guest interrupt handlers (GE callbacks) run inside HLE calls; no thread
// switch may happen until they return.
static int g_in_intr;
void hle_intr_enter(void) { g_in_intr++; }
void hle_intr_leave(void) { g_in_intr--; }

// Give the baton to the best ready thread and block until it comes back.
// Called with g_big held by `self` (which may be waiting, dormant or dead).
static void reschedule(Thread *self) {
    if (g_in_intr && self->status == ST_RUNNING) return;
    for (;;) {
        events_locked();
        Thread *next = pick_next();
        if (next) {
            if (next == self) {
                self->status = ST_RUNNING;
                g_cur = self;
                return;
            }
            if (g_cur == self) g_cur = next;
            if (self->status == ST_RUNNING) self->status = ST_READY;
            next->status = ST_RUNNING;
            g_cur = next;
            if (!next->host_alive) start_host(next);
            else pthread_cond_signal(&next->cv);
            if (self->status == ST_DORMANT || self->status == ST_DEAD) return;  // exiting
            while (g_cur != self && !self->killed) pthread_cond_wait(&self->cv, &g_big);
            if (self->killed) {
                self->host_alive = 0;
                pthread_mutex_unlock(&g_big);
                pthread_exit(NULL);
            }
            continue;  // we hold the baton again; re-check events
        }
        // nobody can run: idle until the next deadline or vblank
        int any = 0;
        uint64_t now = hle_time_us(), wake = (vblank_count() + 1) * VBLANK_US;
        FOR_EACH_THREAD(t) {
            if (t->status == ST_WAITING) any = 1;
            if (t->status == ST_WAITING && t->deadline && t->deadline < wake) wake = t->deadline;
        }
        if (!any) {
            fprintf(stderr, "all threads finished or deadlocked\n");
            pthread_cond_broadcast(&g_all_done);
            exit(0);
        }
        if (wake > now) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t ns = (uint64_t)ts.tv_nsec + (wake - now) * 1000u;
            ts.tv_sec += (time_t)(ns / 1000000000u);
            ts.tv_nsec = (long)(ns % 1000000000u);
            pthread_cond_timedwait(&self->cv, &g_big, &ts);
        }
    }
}

static uint64_t g_last_vblank;

static void events_locked(void) {
    uint64_t now = hle_time_us(), vb = vblank_count();
    if (vb != g_last_vblank) {
        g_last_vblank = vb;
        wake_waiters(W_VBLANK, 0, 0);
    }
    FOR_EACH_THREAD(t) {
        if (t->status != ST_WAITING || !t->deadline || t->deadline > now) continue;
        if (t->wait_type == W_DELAY) make_ready(t, 0);
        else make_ready(t, (int32_t)SCE_KERNEL_ERROR_WAIT_TIMEOUT);
    }
}

// Block the calling thread; returns the wait result.
static int32_t wait_on(Ctx *c, int type, int id, uint32_t timeout_ptr) {
    Thread *self = tls_self;
    (void)c;
    self->status = ST_WAITING;
    self->wait_type = type;
    self->wait_id = id;
    self->timeout_ptr = 0;
    self->deadline = 0;
    if (timeout_ptr) {
        self->timeout_ptr = timeout_ptr;
        self->deadline = hle_time_us() + mem_r32(timeout_ptr);
    }
    reschedule(self);
    return self->wait_result;
}

// After waking someone up, let a higher-priority thread run right away.
static void yield_if_needed(void) {
    Thread *self = tls_self;
    reschedule(self);
}

void sched_check_events(void) {
    if (tls_self) reschedule(tls_self);
}

Ctx *sched_current_ctx(void) { return tls_self ? &tls_self->ctx : NULL; }

static void thread_finish(Thread *t, uint32_t status, int del) {
    t->exit_status = status;
    t->status = ST_DORMANT;
    FOR_EACH_THREAD(w) {
        if (w->status == ST_WAITING && w->wait_type == W_THREAD_END && w->wait_id == t->id)
            make_ready(w, (int32_t)status);
    }
    if (del) {
        t->status = ST_DEAD;
        if (t->stack) sysmem_free(t->stack);
        t->stack = 0;
        obj_del(t->id);
    }
}

static void exit_current(uint32_t status, int del) {
    Thread *self = tls_self;
    thread_finish(self, status, del);
    self->host_alive = 0;
    reschedule(self);  // hands the baton over and returns (we're dormant)
    pthread_mutex_unlock(&g_big);
    if (del) free(self);  // NB: object already unlinked
    pthread_exit(NULL);
}

static void *host_thread_main(void *arg) {
    Thread *t = arg;
    pthread_mutex_lock(&g_big);
    tls_self = t;
    while (g_cur != t) pthread_cond_wait(&t->cv, &g_big);
    t->status = ST_RUNNING;
    call_indirect(&t->ctx, t->entry);
    exit_current(t->ctx.r[2], 0);  // returning from the entry = sceKernelExitThread(v0)
    return NULL;
}

static Thread *thread_new(uint32_t name, uint32_t entry, uint32_t prio, uint32_t stack_size,
                          uint32_t attr) {
    Thread *t = calloc(1, sizeof *t);
    t->id = obj_new(t, OBJ_THREAD, name, attr);
    t->entry = entry;
    t->init_prio = t->prio = prio;
    t->stack_size = (stack_size + 0xFF) & ~0xFFu;
    t->status = ST_DORMANT;
    pthread_cond_init(&t->cv, NULL);
    if (t->stack_size) {
        t->stack = sysmem_alloc(t->stack_size, 1, 0, 0x100);
        if (!t->stack) {
            fprintf(stderr, "out of memory for thread stack (%u bytes)\n", t->stack_size);
            exit(1);
        }
        memset(host(t->stack), 0xFF, t->stack_size);
    }
    return t;
}

static void thread_prepare(Thread *t, uint32_t arglen, uint32_t argp, uint32_t gp) {
    memset(&t->ctx, 0, sizeof t->ctx);
    uint32_t sp = t->stack + t->stack_size;
    if (argp && arglen) {
        sp -= (arglen + 15) & ~15u;
        memmove(host(sp), host(argp), arglen);
        t->ctx.r[5] = sp;
    }
    t->ctx.r[4] = arglen;
    t->ctx.r[29] = sp - 64;
    t->ctx.r[28] = gp;
    t->ctx.r[31] = 0;
    t->ctx.fcr31 = 0;
    t->ctx.vfpu_ctrl[0] = t->ctx.vfpu_ctrl[1] = 0xE4;  // no source prefix
}

// Boot: run module_start as a thread at priority 32, then hand over to it.
void sched_boot(Ctx *boot_ctx) {
    pthread_mutex_lock(&g_big);
    Thread *t = thread_new(0, g_entry, 32, 0x40000, 0);
    strcpy(t->h.name, "module_start");
    thread_prepare(t, boot_ctx->r[4], boot_ctx->r[5], boot_ctx->r[28]);
    t->status = ST_RUNNING;
    t->seq = ++g_seq;
    g_cur = t;
    start_host(t);
    pthread_mutex_unlock(&g_big);  // the main host thread goes on to run the frontend
}

// ---------------------------------------------------------------- waiting lists

static void wake_waiters(int wait_type, int id, int32_t result) {
    FOR_EACH_THREAD(t) {
        if (t->status == ST_WAITING && t->wait_type == wait_type && t->wait_id == id) {
            make_ready(t, result);
        }
    }
}

// ---------------------------------------------------------------- threads

void sceKernelCreateThread(Ctx *c) {
    uint32_t prio = ARG(2);
    if (prio < 8 || prio > 119) {
        RET(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY);
        return;
    }
    Thread *t = thread_new(ARG(0), ARG(1), prio, ARG(3), ARG(4));
    t->gp = c->r[28];
    HLOG("  thread %x '%s' entry=%08X prio=%u stack=%x\n", t->id, t->h.name, t->entry, prio, ARG(3));
    RET(t->id);
}

void sceKernelStartThread(Ctx *c) {
    Thread *t = obj_get((int)ARG(0), OBJ_THREAD);
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t->status != ST_DORMANT) { RET(SCE_KERNEL_ERROR_NOT_DORMANT); return; }
    thread_prepare(t, ARG(1), ARG(2), t->gp);
    t->status = ST_READY;
    t->seq = ++g_seq;
    t->host_alive = 0;
    RET(0);
    yield_if_needed();
}

void sceKernelExitThread(Ctx *c) { exit_current(ARG(0), 0); }
void sceKernelExitDeleteThread(Ctx *c) { exit_current(ARG(0), 1); }

static void terminate(Thread *t, int del) {
    if (t->host_alive) {
        t->killed = 1;
        pthread_cond_signal(&t->cv);
    }
    t->host_alive = 0;
    thread_finish(t, 0x800201ACu, del);
}

void sceKernelTerminateThread(Ctx *c) {
    Thread *t = obj_get((int)ARG(0), OBJ_THREAD);
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t == tls_self) { RET(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT); return; }
    terminate(t, 0);
    RET(0);
}

void sceKernelTerminateDeleteThread(Ctx *c) {
    Thread *t = obj_get((int)ARG(0), OBJ_THREAD);
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t == tls_self) { RET(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT); return; }
    terminate(t, 1);
    RET(0);
}

void sceKernelDeleteThread(Ctx *c) {
    Thread *t = obj_get((int)ARG(0), OBJ_THREAD);
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t->status != ST_DORMANT) { RET(SCE_KERNEL_ERROR_NOT_DORMANT); return; }
    if (t->stack) sysmem_free(t->stack);
    obj_del(t->id);
    RET(0);
}

void sceKernelGetThreadId(Ctx *c) { RET(tls_self->id); }

void sceKernelGetThreadCurrentPriority(Ctx *c) { RET(tls_self->prio); }

void sceKernelChangeThreadPriority(Ctx *c) {
    Thread *t = ARG(0) ? obj_get((int)ARG(0), OBJ_THREAD) : tls_self;
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    uint32_t prio = ARG(1) ? ARG(1) : tls_self->prio;
    if (prio < 8 || prio > 119) { RET(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY); return; }
    t->prio = prio;
    t->seq = ++g_seq;  // goes to the back of its new priority queue
    RET(0);
    yield_if_needed();
}

void sceKernelChangeCurrentThreadAttr(Ctx *c) {
    tls_self->h.attr = (tls_self->h.attr & ~ARG(0)) | ARG(1);
    RET(0);
}

void sceKernelReferThreadStatus(Ctx *c) {
    Thread *t = ARG(0) ? obj_get((int)ARG(0), OBJ_THREAD) : tls_self;
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    uint32_t p = ARG(1);
    uint32_t size = mem_r32(p);
    uint8_t buf[0x68] = {0};
    uint32_t v[] = {0x68};
    memcpy(buf, v, 4);
    memcpy(buf + 4, t->h.name, 32);
#define PUT(off, val) do { uint32_t x_ = (val); memcpy(buf + (off), &x_, 4); } while (0)
    PUT(36, t->h.attr);
    PUT(40, (uint32_t)t->status);
    PUT(44, t->entry);
    PUT(48, t->stack);
    PUT(52, t->stack_size);
    PUT(56, t->gp);
    PUT(60, t->init_prio);
    PUT(64, t->prio);
    PUT(68, (uint32_t)t->wait_type);
    PUT(72, (uint32_t)t->wait_id);
    PUT(76, (uint32_t)t->wakeup_count);
    PUT(80, t->exit_status);
#undef PUT
    memcpy(host(p), buf, size < sizeof buf ? size : sizeof buf);
    RET(0);
}

static void wait_thread_end(Ctx *c) {
    Thread *t = obj_get((int)ARG(0), OBJ_THREAD);
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t->status == ST_DORMANT) { RET(t->exit_status); return; }
    RET(wait_on(c, W_THREAD_END, t->id, ARG(1)));
}
void sceKernelWaitThreadEnd(Ctx *c) { wait_thread_end(c); }
void sceKernelWaitThreadEndCB(Ctx *c) { wait_thread_end(c); }

void sceKernelSleepThread(Ctx *c) {
    Thread *self = tls_self;
    if (self->wakeup_count > 0) {
        self->wakeup_count--;
        RET(0);
        return;
    }
    RET(wait_on(c, W_SLEEP, 0, 0));
}

void sceKernelWakeupThread(Ctx *c) {
    Thread *t = obj_get((int)ARG(0), OBJ_THREAD);
    if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
    if (t->status == ST_WAITING && t->wait_type == W_SLEEP) make_ready(t, 0);
    else t->wakeup_count++;
    RET(0);
    yield_if_needed();
}

void thread_delay(Ctx *c, uint64_t usec, int allow_cb) {
    (void)allow_cb;
    Thread *self = tls_self;
    if (usec < 200) usec = 200;  // the PSP never delays for less than this
    self->status = ST_WAITING;
    self->wait_type = W_DELAY;
    self->timeout_ptr = 0;
    self->deadline = hle_time_us() + usec;
    reschedule(self);
    RET(0);
}
void sceKernelDelayThread(Ctx *c) { thread_delay(c, ARG(0), 0); }
void sceKernelDelayThreadCB(Ctx *c) { thread_delay(c, ARG(0), 1); }

void hle_wait_vblank(Ctx *c, int allow_cb) {
    (void)allow_cb;
    wait_on(c, W_VBLANK, 0, 0);
    RET(0);
}

// ---------------------------------------------------------------- semaphores

void sceKernelCreateSema(Ctx *c) {
    Sema *s = calloc(1, sizeof *s);
    s->count = (int32_t)ARG(2);
    s->max = (int32_t)ARG(3);
    RET(obj_new(s, OBJ_SEMA, ARG(0), ARG(1)));
}

void sceKernelDeleteSema(Ctx *c) {
    Sema *s = obj_get((int)ARG(0), OBJ_SEMA);
    if (!s) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    FOR_EACH_THREAD(t) {
        if (t->status == ST_WAITING && t->wait_type == W_SEMA && t->wait_id == (int)ARG(0))
            make_ready(t, (int32_t)SCE_KERNEL_ERROR_WAIT_DELETE);
    }
    obj_del((int)ARG(0));
    free(s);
    RET(0);
    yield_if_needed();
}

// Hand the count to waiters in priority (attr 0x100) or FIFO order.
static void sema_wake(int id, Sema *s) {
    for (;;) {
        Thread *best = NULL;
        FOR_EACH_THREAD(t) {
            if (t->status != ST_WAITING || t->wait_type != W_SEMA || t->wait_id != id) continue;
            if (!best || ((s->h.attr & 0x100) ? t->prio < best->prio : t->seq < best->seq)) best = t;
        }
        if (!best || (int32_t)best->wait_a > s->count) return;
        s->count -= (int32_t)best->wait_a;
        make_ready(best, 0);
    }
}

void sceKernelSignalSema(Ctx *c) {
    Sema *s = obj_get((int)ARG(0), OBJ_SEMA);
    if (!s) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    if (s->count + (int32_t)ARG(1) > s->max) { RET(SCE_KERNEL_ERROR_SEMA_OVF); return; }
    s->count += (int32_t)ARG(1);
    sema_wake((int)ARG(0), s);
    RET(0);
    yield_if_needed();
}

static void wait_sema(Ctx *c) {
    Sema *s = obj_get((int)ARG(0), OBJ_SEMA);
    if (!s) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    int32_t n = (int32_t)ARG(1);
    if (n <= 0 || n > s->max) { RET(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT); return; }
    if (s->count >= n) {
        s->count -= n;
        RET(0);
        return;
    }
    tls_self->wait_a = (uint32_t)n;
    RET(wait_on(c, W_SEMA, (int)ARG(0), ARG(2)));
}
void sceKernelWaitSema(Ctx *c) { wait_sema(c); }
void sceKernelWaitSemaCB(Ctx *c) { wait_sema(c); }

void sceKernelPollSema(Ctx *c) {
    Sema *s = obj_get((int)ARG(0), OBJ_SEMA);
    if (!s) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
    int32_t n = (int32_t)ARG(1);
    if (n <= 0) { RET(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT); return; }
    if (s->count < n) { RET(SCE_KERNEL_ERROR_SEMA_ZERO); return; }
    s->count -= n;
    RET(0);
}

// ---------------------------------------------------------------- event flags

#define EVF_AND 0x00
#define EVF_OR 0x01
#define EVF_CLEARALL 0x10
#define EVF_CLEARPAT 0x20

static int evf_match(EventFlag *e, uint32_t bits, uint32_t mode) {
    return (mode & EVF_OR) ? (e->pattern & bits) != 0 : (e->pattern & bits) == bits;
}

static void evf_consume(EventFlag *e, uint32_t bits, uint32_t mode, uint32_t out_ptr) {
    if (out_ptr) mem_w32(out_ptr, e->pattern);
    if (mode & EVF_CLEARALL) e->pattern = 0;
    else if (mode & EVF_CLEARPAT) e->pattern &= ~bits;
}

void sceKernelCreateEventFlag(Ctx *c) {
    EventFlag *e = calloc(1, sizeof *e);
    e->init = e->pattern = ARG(2);
    RET(obj_new(e, OBJ_EVENT, ARG(0), ARG(1)));
}

void sceKernelDeleteEventFlag(Ctx *c) {
    EventFlag *e = obj_get((int)ARG(0), OBJ_EVENT);
    if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    FOR_EACH_THREAD(t) {
        if (t->status == ST_WAITING && t->wait_type == W_EVENT && t->wait_id == (int)ARG(0))
            make_ready(t, (int32_t)SCE_KERNEL_ERROR_WAIT_DELETE);
    }
    obj_del((int)ARG(0));
    free(e);
    RET(0);
    yield_if_needed();
}

void sceKernelSetEventFlag(Ctx *c) {
    EventFlag *e = obj_get((int)ARG(0), OBJ_EVENT);
    if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    e->pattern |= ARG(1);
    FOR_EACH_THREAD(t) {
        if (t->status != ST_WAITING || t->wait_type != W_EVENT || t->wait_id != (int)ARG(0)) continue;
        if (evf_match(e, t->wait_a, t->wait_b)) {
            evf_consume(e, t->wait_a, t->wait_b, t->wait_c);
            make_ready(t, 0);
        }
    }
    RET(0);
    yield_if_needed();
}

void sceKernelClearEventFlag(Ctx *c) {
    EventFlag *e = obj_get((int)ARG(0), OBJ_EVENT);
    if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    e->pattern &= ARG(1);
    RET(0);
}

static void wait_event(Ctx *c) {
    EventFlag *e = obj_get((int)ARG(0), OBJ_EVENT);
    if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    uint32_t bits = ARG(1), mode = ARG(2), out = ARG(3);
    if (!bits) { RET(SCE_KERNEL_ERROR_EVF_ILPAT); return; }
    if (evf_match(e, bits, mode)) {
        evf_consume(e, bits, mode, out);
        RET(0);
        return;
    }
    Thread *self = tls_self;
    self->wait_a = bits;
    self->wait_b = mode;
    self->wait_c = out;
    int32_t r = wait_on(c, W_EVENT, (int)ARG(0), ARG(4));
    if (r && out) mem_w32(out, e ? e->pattern : 0);
    RET(r);
}
void sceKernelWaitEventFlag(Ctx *c) { wait_event(c); }
void sceKernelWaitEventFlagCB(Ctx *c) { wait_event(c); }

void sceKernelPollEventFlag(Ctx *c) {
    EventFlag *e = obj_get((int)ARG(0), OBJ_EVENT);
    if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    if (!evf_match(e, ARG(1), ARG(2))) {
        if (ARG(3)) mem_w32(ARG(3), e->pattern);
        RET(SCE_KERNEL_ERROR_EVF_COND);
        return;
    }
    evf_consume(e, ARG(1), ARG(2), ARG(3));
    RET(0);
}

void sceKernelReferEventFlagStatus(Ctx *c) {
    EventFlag *e = obj_get((int)ARG(0), OBJ_EVENT);
    if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
    uint32_t p = ARG(1), waiting = 0;
    FOR_EACH_THREAD(t) {
        if (t->status == ST_WAITING && t->wait_type == W_EVENT && t->wait_id == (int)ARG(0)) waiting++;
    }
    mem_w32(p, 52);
    memcpy(host(p + 4), e->h.name, 32);
    mem_w32(p + 36, e->h.attr);
    mem_w32(p + 40, e->init);
    mem_w32(p + 44, e->pattern);
    mem_w32(p + 48, waiting);
    RET(0);
}

// ---------------------------------------------------------------- fixed pools

void sceKernelCreateFpl(Ctx *c) {
    uint32_t bs = ARG(3), n = ARG(4);
    uint32_t align = 4;
    if (ARG(5) && mem_r32(ARG(5)) >= 8) align = mem_r32(ARG(5) + 4) ? mem_r32(ARG(5) + 4) : 4;
    uint32_t stride = (bs + align - 1) & ~(align - 1);
    uint32_t base = sysmem_alloc(stride * n, (ARG(2) & 0x4000) ? 1 : 0, 0, align);
    if (!base) { RET(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    Fpl *f = calloc(1, sizeof *f);
    f->base = base;
    f->block_size = stride;
    f->nblocks = n;
    f->used = calloc(n, 1);
    RET(obj_new(f, OBJ_FPL, ARG(0), ARG(2)));
}

void sceKernelDeleteFpl(Ctx *c) {
    Fpl *f = obj_get((int)ARG(0), OBJ_FPL);
    if (!f) { RET(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
    sysmem_free(f->base);
    obj_del((int)ARG(0));
    free(f->used);
    free(f);
    RET(0);
}

void sceKernelTryAllocateFpl(Ctx *c) {
    Fpl *f = obj_get((int)ARG(0), OBJ_FPL);
    if (!f) { RET(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
    for (uint32_t k = 0; k < f->nblocks; k++)
        if (!f->used[k]) {
            f->used[k] = 1;
            mem_w32(ARG(1), f->base + k * f->block_size);
            RET(0);
            return;
        }
    RET(SCE_KERNEL_ERROR_NO_FREE);
}

void sceKernelFreeFpl(Ctx *c) {
    Fpl *f = obj_get((int)ARG(0), OBJ_FPL);
    if (!f) { RET(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
    uint32_t k = (ARG(1) - f->base) / f->block_size;
    if (ARG(1) < f->base || k >= f->nblocks || !f->used[k]) {
        RET(SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT);
        return;
    }
    f->used[k] = 0;
    RET(0);
}

// ---------------------------------------------------------------- callbacks

void sceKernelCreateCallback(Ctx *c) {
    Callback *cb = calloc(1, sizeof *cb);
    cb->func = ARG(1);
    cb->arg = ARG(2);
    cb->owner = tls_self->id;
    RET(obj_new(cb, OBJ_CALLBACK, ARG(0), 0));
}

void sceKernelDeleteCallback(Ctx *c) {
    Callback *cb = obj_get((int)ARG(0), OBJ_CALLBACK);
    if (!cb) { RET(SCE_KERNEL_ERROR_UNKNOWN_CBID); return; }
    obj_del((int)ARG(0));
    free(cb);
    RET(0);
}

// ---------------------------------------------------------------- time

void sceKernelGetSystemTime(Ctx *c) {
    uint64_t t = hle_time_us();
    mem_w32(ARG(0), (uint32_t)t);
    mem_w32(ARG(0) + 4, (uint32_t)(t >> 32));
    RET(0);
}
void sceKernelGetSystemTimeLow(Ctx *c) { RET((uint32_t)hle_time_us()); }
void sceKernelGetSystemTimeWide(Ctx *c) { RET64(hle_time_us()); }

// Split a clock into seconds + microseconds; with no seconds pointer the
// whole value goes to the microseconds output (games rely on this).
static void clock_split(uint64_t t, uint32_t sec_ptr, uint32_t usec_ptr) {
    if (sec_ptr) {
        mem_w32(sec_ptr, (uint32_t)(t / 1000000u));
        if (usec_ptr) mem_w32(usec_ptr, (uint32_t)(t % 1000000u));
    } else if (usec_ptr) {
        mem_w32(usec_ptr, (uint32_t)t);
    }
}
void sceKernelSysClock2USec(Ctx *c) {
    clock_split(mem_r32(ARG(0)) | ((uint64_t)mem_r32(ARG(0) + 4) << 32), ARG(1), ARG(2));
    RET(0);
}
void sceKernelSysClock2USecWide(Ctx *c) {
    clock_split(ARG(0) | ((uint64_t)ARG(1) << 32), ARG(2), ARG(3));
    RET(0);
}
void sceKernelUSec2SysClockWide(Ctx *c) { RET64(ARG(0)); }
