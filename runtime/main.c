#include <stdio.h>
#include <stdlib.h>
#ifndef __EMSCRIPTEN__
#include <execinfo.h>
#endif
#include <signal.h>
#include <unistd.h>

#include "func_table.h"
#include "hle/hle.h"
#include "image_base.h"
#include "recomp.h"

void hle_init(void);
void io_init(const char *iso_path, const char *ms_root);
void frontend_run(void);
void frontend_scanout(uint32_t *out);

extern uint32_t g_buttons;

// "START+CROSS" -> PSP button bits
static uint32_t button_mask(const char *s) {
    static const struct { const char *n; uint32_t b; } k[] = {
        {"SELECT", 0x1}, {"START", 0x8}, {"UP", 0x10}, {"RIGHT", 0x20}, {"DOWN", 0x40}, {"LEFT", 0x80},
        {"L", 0x100}, {"R", 0x200}, {"TRIANGLE", 0x1000}, {"CIRCLE", 0x2000}, {"CROSS", 0x4000}, {"SQUARE", 0x8000}};
    uint32_t m = 0;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", s);
    char *sp;
    for (char *t = strtok_r(buf, "+", &sp); t; t = strtok_r(NULL, "+", &sp))
        for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
            if (!strcmp(t, k[i].n)) m |= k[i].b;
    return m;
}

// INPUT="120:START,120.3:,125:CROSS" sets the held buttons at given guest times.
// Called from the headless loop and from the frontend's frame callback.
void script_tick(void) {
    // apply at most one event per tick so that a press is never swallowed by
    // its own release when ticks are far apart (e.g. a throttled browser tab)
    static int next;
    const char *input = getenv("INPUT");
    if (!input) return;
    double now = (double)hle_time_us() / 1e6;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", input);
    char *sp;
    int k = 0;
    for (char *e = strtok_r(buf, ",", &sp); e; e = strtok_r(NULL, ",", &sp), k++) {
        if (k < next) continue;
        const char *colon = strchr(e, ':');
        if (colon && atof(e) <= now) {
            g_buttons = button_mask(colon + 1);
            next = k + 1;
        }
        return;
    }
}

// SHOTS=dir: save the displayed framebuffer as dir/shot_NNN.ppm every second
static void save_shot(const char *dir, int n) {
    static uint32_t px[480 * 272];
    char path[512];
    snprintf(path, sizeof path, "%s/shot_%03d.ppm", dir, n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    frontend_scanout(px);
    fprintf(f, "P6\n480 272\n255\n");
    for (int i = 0; i < 480 * 272; i++) {
        uint8_t rgb[3] = {(uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

uint8_t *g_mem;

static RecompFunc find_func(uint32_t addr) {
    addr &= 0x1FFFFFFFu;
    unsigned lo = 0, hi = g_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_funcs[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_func_count && g_funcs[lo].addr == addr ? g_funcs[lo].fn : NULL;
}

void recomp_trap(Ctx *c, uint32_t pc, const char *why) {
    fprintf(stderr, "TRAP at %08X: %s (ra=%08X sp=%08X)\n", pc, why, c->r[31], c->r[29]);
    exit(1);
}

static uint32_t g_trace_ra;  // TRACE_RA=0x...: log indirect calls made from there

void call_indirect(Ctx *c, uint32_t addr) {
    RecompFunc fn = find_func(addr);
    if (fn) {
        uint32_t ra = c->r[31];
        fn(c);
        if (g_trace_ra && ra == g_trace_ra)
            fprintf(stderr, "trace: call %08X from %08X -> v0=%08X f0=%g\n", addr, ra, c->r[2], c->f[0].f);
        return;
    }
    for (unsigned k = 0; k < g_import_count; k++)
        if (g_imports[k].stub == (addr & 0x1FFFFFFFu)) {
            hle_call(c, k);
            return;
        }
    recomp_trap(c, addr, "indirect call to unknown address");
}

void recomp_probe(Ctx *c, uint32_t func) {
    fprintf(stderr, "probe f_%08X a0=%08X a1=%08X a2=%08X a3=%08X ra=%08X sp=%08X\n", func, c->r[4], c->r[5],
            c->r[6], c->r[7], c->r[31], c->r[29]);
    // PROBE_CHAIN="6:84:440": start from register 6, then follow mem[v + offset]
    const char *chain = getenv("PROBE_CHAIN");
    if (chain) {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", chain);
        char *sp, *t = strtok_r(buf, ":", &sp);
        uint32_t v = c->r[atoi(t) & 31];
        fprintf(stderr, "  chain r%s=%08X", t, v);
        while ((t = strtok_r(NULL, ":", &sp))) {
            uint32_t a = v + (uint32_t)atoi(t);
            v = (a & 0x1FFFFFFF) < GUEST_MEM_SIZE - 4 ? mem_r32(a) : 0xDEADBEEF;
            fprintf(stderr, " -> [+%s]=%08X", t, v);
        }
        fputc('\n', stderr);
    }
}

void recomp_probe_ret(Ctx *c, uint32_t func) {
    fprintf(stderr, "probe f_%08X returns v0=%08X\n", func, c->r[2]);
}

// Diagnostic build (RECOMP_CHECK=1): a function returned with a callee-saved
// register changed. The stack pointer is checked too.
void check_saved(Ctx *c, const uint32_t *saved, uint32_t func, uint32_t at) {
    static int reported;
    for (int k = 0; k < 10; k++) {
        int reg = k < 8 ? 16 + k : (k == 8 ? 29 : 30);
        if (c->r[reg] != saved[k] && reported < 20) {
            reported++;
            fprintf(stderr, "check: f_%08X (jr at %08X) changed $%d %08X -> %08X\n", func, at, reg, saved[k],
                    c->r[reg]);
        }
    }
}

#ifndef __EMSCRIPTEN__
static void on_crash(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uintptr_t off = (uintptr_t)si->si_addr - (uintptr_t)g_mem;
    fprintf(stderr, "\n*** signal %d at host %p (guest offset %08lX)\n", sig, si->si_addr,
            off < 0x20000000u ? (unsigned long)off : 0xFFFFFFFFul);
    void *bt[48];
    int n = backtrace(bt, 48);
    backtrace_symbols_fd(bt, n, 2);
    _exit(128 + sig);
}
#endif

static void load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    size_t n = fread(host(IMAGE_BASE), 1, IMAGE_SIZE, f);
    fclose(f);
    if (n != IMAGE_SIZE) {
        fprintf(stderr, "%s: short read (%zu of %u bytes)\n", path, n, IMAGE_SIZE);
        exit(1);
    }
}

int main(int argc, char **argv) {
    g_mem = calloc(1, GUEST_MEM_SIZE);
#ifndef __EMSCRIPTEN__
    struct sigaction sa = {0};
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#endif
#ifdef __EMSCRIPTEN__
    (void)argc;
    (void)argv;
    load_image("image.bin");  // preloaded into the virtual FS; the ISO comes from the page
    io_init(NULL, "ms0");
#else
    if (argc < 3) {
        fprintf(stderr, "usage: %s image.bin game.iso\n", argv[0]);
        return 1;
    }
    load_image(argv[1]);
    io_init(argv[2], "ms0");
#endif
    g_hle_log = getenv("HLE_LOG") != NULL;
    if (getenv("TRACE_RA")) g_trace_ra = (uint32_t)strtoul(getenv("TRACE_RA"), NULL, 0);

    // user partition: everything after the loaded image up to the end of RAM
    sysmem_init((IMAGE_BASE + IMAGE_SIZE + 0xFF) & ~0xFFu, 0x0A000000u);
    hle_init();

    static Ctx boot;
    const char *boot_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    uint32_t argp = 0x08800000u;  // scratch area below the image
    strcpy((char *)host(argp), boot_path);
    boot.r[4] = (uint32_t)strlen(boot_path) + 1;  // module_start(args, argp)
    boot.r[5] = argp;
    fprintf(stderr, "starting module_start at %08X\n", g_entry);
    sched_boot(&boot);
    if (getenv("HEADLESS")) {
        extern void audio_start_dump(void);
        audio_start_dump();
        // WATCH=addr,addr,...: print those words (as hex and float) every second
        const char *watch = getenv("WATCH");
        int secs = getenv("RUN_SECONDS") ? atoi(getenv("RUN_SECONDS")) : 0;
        // INPUT="120:START,120.3:,125:CROSS,125.3:" sets the held buttons at given times
        int last_sec = -1;
        for (int tick = 0;; tick++) {  // polls every 10 ms; times are in guest seconds
            usleep(10000);
            double now = (double)hle_time_us() / 1e6;
            script_tick();
            int t = (int)now;
            if (t == last_sec) continue;
            last_sec = t;
            if (secs && t >= secs) exit(0);
            if (getenv("SHOTS")) save_shot(getenv("SHOTS"), t);
            if (getenv("FPS")) {
                extern uint32_t g_frames_presented;
                static uint32_t last_frames;
                extern uint64_t g_gpu_busy_us, g_idle_us;
                static uint64_t last_gpu, last_idle;
                fprintf(stderr, "t=%d fps=%u gpu=%llums idle=%llums\n", t, g_frames_presented - last_frames,
                        (unsigned long long)(g_gpu_busy_us - last_gpu) / 1000, (unsigned long long)(g_idle_us - last_idle) / 1000);
                last_gpu = g_gpu_busy_us;
                last_idle = g_idle_us;
                last_frames = g_frames_presented;
            }
            if (!watch) continue;
            char buf[512];
            snprintf(buf, sizeof buf, "%s", watch);
            for (char *w = strtok(buf, ","); w; w = strtok(NULL, ",")) {
                uint32_t a = (uint32_t)strtoul(w, NULL, 0), v = mem_r32(a);
                float f;
                memcpy(&f, &v, 4);
                fprintf(stderr, "watch %08X = %08X (%g)\n", a, v, f);
            }
        }
    }
    frontend_run();
    return 0;
}
