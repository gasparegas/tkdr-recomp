#include <stdio.h>
#include <stdlib.h>
#ifndef __EMSCRIPTEN__
#include <execinfo.h>
#endif
#include <signal.h>
#include <unistd.h>

#include "func_table.h"
#include "gpu/gpu.h"
#include "hle/hle.h"
#include "recomp.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#ifdef RECOMP_DYNAMIC
// Hostable web build: no game code inside. The page recompiles the player's
// EBOOT in the browser (web/recomp) and leaves in the virtual FS:
//   /game/game.txt   "base size entry nimports" then "stub lib name nid" per import
//   /game/image.bin  the relocated executable image
//   /game/code.wasm  the generated module, called through game_dispatch (game_lib.js)
uint32_t g_image_base, g_image_size, g_entry;
ImportEntry *g_imports;
unsigned g_import_count;
#define IMAGE_BASE g_image_base
#define IMAGE_SIZE g_image_size
static uint8_t *g_game_code;
static uint32_t g_game_code_len;
EMSCRIPTEN_KEEPALIVE uint32_t game_code_ptr(void) { return (uint32_t)(uintptr_t)g_game_code; }
EMSCRIPTEN_KEEPALIVE uint32_t game_code_len(void) { return g_game_code_len; }
EMSCRIPTEN_KEEPALIVE uint32_t game_gmem(void) { return (uint32_t)(uintptr_t)g_mem; }

static void load_game(void) {
    FILE *f = fopen("/game/game.txt", "r");
    unsigned n = 0;
    if (!f || fscanf(f, "%x %x %x %u", &g_image_base, &g_image_size, &g_entry, &n) != 4) {
        fprintf(stderr, "/game/game.txt missing or invalid\n");
        exit(1);
    }
    g_imports = calloc(n, sizeof *g_imports);
    for (unsigned k = 0; k < n; k++) {
        char lib[64], name[96];
        unsigned stub, nid;
        if (fscanf(f, "%x %63s %95s %x", &stub, lib, name, &nid) != 4) break;
        g_imports[k] = (ImportEntry){strdup(lib), strdup(name), nid, stub};
        g_import_count = k + 1;
    }
    fclose(f);
    f = fopen("/game/code.wasm", "rb");
    if (!f) {
        fprintf(stderr, "/game/code.wasm missing\n");
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    g_game_code_len = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_game_code = malloc(g_game_code_len);
    if (fread(g_game_code, 1, g_game_code_len, f) != g_game_code_len) exit(1);
    fclose(f);
}

void game_dispatch(Ctx *c, uint32_t addr);  // runtime/web/game_lib.js
#else
#include "image_base.h"
#endif

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
    static uint32_t px[480 * 4 * 272 * 4];
    char path[512];
    snprintf(path, sizeof path, "%s/shot_%03d.ppm", dir, n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int S = gpu_scanout_scaled(g_display.fb_addr, g_display.stride, g_display.format, px, 4);
    if (!S) {
        frontend_scanout(px);
        S = 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", 480 * S, 272 * S);
    for (int i = 0; i < 480 * S * 272 * S; i++) {
        uint8_t rgb[3] = {(uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

uint8_t *g_mem;

#ifndef RECOMP_DYNAMIC
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
#endif

void recomp_trap(Ctx *c, uint32_t pc, const char *why) {
    fprintf(stderr, "TRAP at %08X: %s (ra=%08X sp=%08X)\n", pc, why, c->r[31], c->r[29]);
    exit(1);
}

// Trap raised by generated wasm code (web/recomp/wasmgen.mjs TRAP).
EMSCRIPTEN_KEEPALIVE void recomp_trap_code(Ctx *c, uint32_t pc, uint32_t code) {
    static const char *names[] = {"?", "syscall", "break", "halt", "cop0", "undecoded instruction",
                                  "unsupported instruction", "branch in delay slot"};
    recomp_trap(c, pc, code < sizeof names / sizeof names[0] ? names[code] : "?");
}

// An indirect call that is not a known function: an import stub, or a bug.
EMSCRIPTEN_KEEPALIVE void recomp_call_miss(Ctx *c, uint32_t addr) {
    for (unsigned k = 0; k < g_import_count; k++)
        if (g_imports[k].stub == (addr & 0x1FFFFFFFu)) {
            hle_call(c, k);
            return;
        }
    recomp_trap(c, addr, "indirect call to unknown address");
}

static uint32_t g_trace_ra;  // TRACE_RA=0x...: log indirect calls made from there

void call_indirect(Ctx *c, uint32_t addr) {
#ifdef RECOMP_DYNAMIC
    game_dispatch(c, addr);
#else
    RecompFunc fn = find_func(addr);
    if (fn) {
        uint32_t ra = c->r[31];
        fn(c);
        if (g_trace_ra && ra == g_trace_ra)
            fprintf(stderr, "trace: call %08X from %08X -> v0=%08X f0=%g\n", addr, ra, c->r[2], c->f[0].f);
        return;
    }
    recomp_call_miss(c, addr);
#endif
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
#ifdef RECOMP_DYNAMIC
    load_game();
    load_image("/game/image.bin");
#else
    load_image("image.bin");  // preloaded into the virtual FS; the ISO comes from the page
#endif
    io_init(NULL, "ms0");
#else
    // GE_REPLAY=capture.bin [GE_REPLAY_ITERS=n] [GE_REPLAY_OUT=out.ppm]: draw a captured frame offline
    if (getenv("GE_REPLAY")) {
        if (getenv("RENDER_SCALE")) gpu_set_render_scale(atoi(getenv("RENDER_SCALE")));
        gpu_resolve_all();  // applies the scale
        return gpu_replay(getenv("GE_REPLAY"), getenv("GE_REPLAY_ITERS") ? atoi(getenv("GE_REPLAY_ITERS")) : 20,
                          getenv("GE_REPLAY_OUT"));
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s image.bin game.iso\n", argv[0]);
        return 1;
    }
    load_image(argv[1]);
    io_init(argv[2], "ms0");
#endif
    g_hle_log = getenv("HLE_LOG") != NULL;
    if (getenv("RENDER_SCALE")) gpu_set_render_scale(atoi(getenv("RENDER_SCALE")));
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
                extern uint64_t g_gpu_busy_us, g_idle_us, g_flush_wait_us, g_prim_us;
                static uint64_t last_gpu, last_idle, last_flush, last_prim;
                fprintf(stderr, "t=%d fps=%u gpu=%llums (prims %llums, flush wait %llums) idle=%llums\n", t,
                        g_frames_presented - last_frames, (unsigned long long)(g_gpu_busy_us - last_gpu) / 1000,
                        (unsigned long long)(g_prim_us - last_prim) / 1000,
                        (unsigned long long)(g_flush_wait_us - last_flush) / 1000,
                        (unsigned long long)(g_idle_us - last_idle) / 1000);
                last_gpu = g_gpu_busy_us;
                last_idle = g_idle_us;
                last_flush = g_flush_wait_us;
                last_prim = g_prim_us;
                extern uint32_t g_flush_reason[5];
                fprintf(stderr, "  flushes: list %u tex %u xfer %u targets %u states %u\n", g_flush_reason[0],
                        g_flush_reason[1], g_flush_reason[2], g_flush_reason[3], g_flush_reason[4]);
                memset(g_flush_reason, 0, sizeof g_flush_reason);
                {
                    extern uint64_t g_worker_busy_us[64], g_worker_px[64];
                    uint64_t busy = 0, px = 0, mx = 0;
                    for (int k = 0; k < 64; k++) {
                        busy += g_worker_busy_us[k];
                        px += g_worker_px[k];
                        if (g_worker_busy_us[k] > mx) mx = g_worker_busy_us[k];
                    }
                    if (px)
                        fprintf(stderr, "  workers: busy %llu ms (max %llu), %.2f Mpx, %.1f ns/px\n",
                                (unsigned long long)busy / 1000, (unsigned long long)mx / 1000, px / 1e6,
                                busy * 1000.0 / px);
                    memset(g_worker_busy_us, 0, sizeof g_worker_busy_us);
                    memset(g_worker_px, 0, sizeof g_worker_px);
                }
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
