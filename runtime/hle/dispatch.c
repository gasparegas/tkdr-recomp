// Routes calls to imported stubs to their HLE implementation.
#include <stdlib.h>

#include "../func_table.h"
#include "hle.h"

extern const HleEntry g_hle_table[];
extern const unsigned g_hle_table_count;

int g_hle_log;
static HleFunc *g_resolved;
static uint8_t *g_warned;

static HleFunc resolve(const char *name) {
    for (unsigned k = 0; k < g_hle_table_count; k++)
        if (!strcmp(g_hle_table[k].name, name)) return g_hle_table[k].fn;
    return NULL;
}

void hle_init(void) {
    g_resolved = calloc(g_import_count, sizeof *g_resolved);
    g_warned = calloc(g_import_count, 1);
    unsigned done = 0;
    for (unsigned k = 0; k < g_import_count; k++)
        if ((g_resolved[k] = resolve(g_imports[k].name))) done++;
    fprintf(stderr, "HLE: %u/%u imports implemented\n", done, g_import_count);
}

void hle_call(Ctx *c, uint32_t idx) {
    const ImportEntry *e = &g_imports[idx];
    HleFunc fn = g_resolved[idx];
    HLOG("%s(%08X, %08X, %08X, %08X) ra=%08X\n", e->name, c->r[4], c->r[5], c->r[6], c->r[7],
         c->r[31]);
    if (fn) {
        fn(c);
    } else {
        if (!g_warned[idx]) {
            g_warned[idx] = 1;
            fprintf(stderr, "HLE: unimplemented %s::%s (returning 0) ra=%08X\n", e->lib, e->name,
                    c->r[31]);
        }
        c->r[2] = 0;
    }
    if (g_hle_log && fn) HLOG("  -> %08X\n", c->r[2]);
    sched_check_events();
}

// Call back into guest code from an HLE function, on the current thread
// (below its stack pointer). The caller's registers are preserved; v0 is
// left holding the guest function's return value.
void hle_call_guest(Ctx *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    Ctx saved = *c;
    c->r[4] = a0;
    c->r[5] = a1;
    c->r[6] = a2;
    c->r[29] = (c->r[29] - 0x100) & ~15u;
    c->r[31] = 0;
    call_indirect(c, fn);
    uint32_t v0 = c->r[2];
    *c = saved;
    c->r[2] = v0;
}
