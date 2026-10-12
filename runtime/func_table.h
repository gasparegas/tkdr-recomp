#pragma once
#include "recomp.h"

typedef struct {
    uint32_t addr;
    RecompFunc fn;
} FuncEntry;

typedef struct {
    const char *lib;
    const char *name;
    uint32_t nid;
    uint32_t stub;
} ImportEntry;

#ifdef RECOMP_DYNAMIC
// The game's code is a WebAssembly module generated in the browser from the
// player's own files (web/recomp); the rest comes from /game at startup.
extern ImportEntry *g_imports;  // sorted by stub address
extern unsigned g_import_count;
extern uint32_t g_entry;
#else
extern const FuncEntry g_funcs[];  // sorted by address
extern const unsigned g_func_count;
extern const uint32_t g_entry;
extern const ImportEntry g_imports[];  // sorted by stub address
extern const unsigned g_import_count;
#endif
