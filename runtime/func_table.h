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

extern const FuncEntry g_funcs[];  // sorted by address
extern const unsigned g_func_count;
extern const uint32_t g_entry;
extern const ImportEntry g_imports[];  // sorted by stub address
extern const unsigned g_import_count;
