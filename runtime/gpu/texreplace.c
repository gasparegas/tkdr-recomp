// Texture dumping and replacement, compatible with PPSSPP texture packs:
// same keys ("<16 hex cachekey><8 hex hash>"), same hash algorithms (quick,
// xxh32, xxh64), and textures.ini [options] / [hashes].
//
//   TEX_DIR  (default "textures/ULUS10139"): the pack folder
//   TEX_DUMP=1: save every new texture as PNG into TEX_DIR/new/
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "gpu.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

// ---------------------------------------------------------------- hashes

static uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static uint32_t xxh32(const uint8_t *p, size_t len, uint32_t seed) {
    const uint32_t P1 = 0x9E3779B1u, P2 = 0x85EBCA77u, P3 = 0xC2B2AE3Du, P4 = 0x27D4EB2Fu, P5 = 0x165667B1u;
    const uint8_t *end = p + len;
    uint32_t h;
    if (len >= 16) {
        uint32_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        for (; p + 16 <= end; p += 16) {
            v1 = rotl32(v1 + rd32(p) * P2, 13) * P1;
            v2 = rotl32(v2 + rd32(p + 4) * P2, 13) * P1;
            v3 = rotl32(v3 + rd32(p + 8) * P2, 13) * P1;
            v4 = rotl32(v4 + rd32(p + 12) * P2, 13) * P1;
        }
        h = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h = seed + P5;
    }
    h += (uint32_t)len;
    for (; p + 4 <= end; p += 4) h = rotl32(h + rd32(p) * P3, 17) * P4;
    for (; p < end; p++) h = rotl32(h + *p * P5, 11) * P1;
    h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    return h;
}

static uint64_t xxh64_round(uint64_t acc, uint64_t lane) {
    acc += lane * 0xC2B2AE3D27D4EB4Full;
    return rotl64(acc, 31) * 0x9E3779B185EBCA87ull;
}

static uint64_t xxh64(const uint8_t *p, size_t len, uint64_t seed) {
    const uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull,
                   P4 = 0x85EBCA77C2B2AE63ull, P5 = 0x27D4EB2F165667C5ull;
    const uint8_t *end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v[4] = {seed + P1 + P2, seed + P2, seed, seed - P1};
        for (; p + 32 <= end; p += 32)
            for (int k = 0; k < 4; k++) v[k] = xxh64_round(v[k], rd64(p + 8 * k));
        h = rotl64(v[0], 1) + rotl64(v[1], 7) + rotl64(v[2], 12) + rotl64(v[3], 18);
        for (int k = 0; k < 4; k++) h = (h ^ xxh64_round(0, v[k])) * P1 + P4;
    } else {
        h = seed + P5;
    }
    h += (uint64_t)len;
    for (; p + 8 <= end; p += 8) h = rotl64(h ^ xxh64_round(0, rd64(p)), 27) * P1 + P4;
    if (p + 4 <= end) {
        h = rotl64(h ^ (uint64_t)rd32(p) * P1, 23) * P2 + P3;
        p += 4;
    }
    for (; p < end; p++) h = rotl64(h ^ *p * P5, 11) * P1;
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

// PPSSPP's "quick" texture hash (identical on every architecture).
static uint32_t quick_hash(const uint8_t *p, uint32_t size) {
    uint32_t check = 0;
    if (((uintptr_t)p & 15) == 0 && (size & 63) == 0) {
        uint16_t cur[8] = {0}, cur2[8] = {0xc00b, 0x9bd9, 0x4b73, 0xb651, 0x4d9b, 0x4309, 0x0083, 0x0001};
        for (uint32_t i = 0; i < size / 16; i += 4) {
            const uint8_t *q = p + i * 16;
            uint16_t a[8], d[8];
            uint32_t b[4], c[4], cur32[4];
            memcpy(a, q, 16);
            memcpy(b, q + 16, 16);
            memcpy(c, q + 32, 16);
            memcpy(d, q + 48, 16);
            for (int j = 0; j < 8; j++) cur[j] = (uint16_t)(cur[j] + (uint16_t)(a[j] * cur2[j]));
            memcpy(cur32, cur, 16);
            for (int j = 0; j < 4; j++) cur32[j] = (cur32[j] ^ b[j]) + c[j];
            memcpy(cur, cur32, 16);
            for (int j = 0; j < 8; j++) cur[j] ^= (uint16_t)(d[j] * cur2[j]);
            for (int j = 0; j < 8; j++) cur2[j] = (uint16_t)(cur2[j] + 0x2455);
        }
        uint32_t c32[4], c2[4];
        memcpy(c32, cur, 16);
        memcpy(c2, cur2, 16);
        for (int j = 0; j < 4; j++) c32[j] += c2[j];
        check = c32[0] + c32[1] + c32[2] + c32[3];
    } else {
        for (uint32_t i = 0; i < size / 8; i++) {
            check += rd32(p + i * 8);
            check ^= rd32(p + i * 8 + 4);
        }
    }
    return check;
}

// ---------------------------------------------------------------- pack index

enum { HASH_QUICK, HASH_XXH32, HASH_XXH64 };

typedef struct {
    uint64_t cachekey;
    uint32_t hash;
    char *file;
} PackEntry;

static PackEntry *g_entries;
static int g_nentries, g_cap;
static int g_hash_kind = HASH_QUICK;
static char g_dir[512];
static int g_init, g_enabled, g_dump;
static volatile int g_reload, g_epoch, g_set_dump = -1;
static uint32_t g_clut_last_bytes, g_clut_max_bytes;

static void add_entry(uint64_t key, uint32_t hash, const char *file) {
    if (g_nentries == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 256;
        g_entries = realloc(g_entries, sizeof *g_entries * (size_t)g_cap);
    }
    g_entries[g_nentries++] = (PackEntry){key, hash, strdup(file)};
}

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    memmove(s, b, strlen(b) + 1);
}

static void load_ini(void) {
    char path[600], line[512], section[64] = "";
    snprintf(path, sizeof path, "%s/textures.ini", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *c = strchr(line, '#');
        if (c) *c = 0;
        trim(line);
        if (!*line) continue;
        if (line[0] == '[') {
            snprintf(section, sizeof section, "%s", line);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        if (!strcasecmp(section, "[options]")) {
            if (!strcasecmp(k, "hash")) g_hash_kind = !strcasecmp(v, "xxh64") ? HASH_XXH64 : !strcasecmp(v, "xxh32") ? HASH_XXH32 : HASH_QUICK;
        } else if (!strcasecmp(section, "[hashes]")) {
            unsigned long long key = 0;
            unsigned hash = 0;
            // keys may be shortened: missing digits mean "any"
            if (sscanf(k, "%16llx%8x", &key, &hash) >= 1 && *v) add_entry(key, hash, v);
        }
    }
    fclose(f);
}

// Files named after their key anywhere in the pack folder.
static void scan_dir(const char *dir, const char *rel) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char full[1024], relp[600];
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        snprintf(relp, sizeof relp, "%s%s%s", rel, *rel ? "/" : "", de->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (strcmp(de->d_name, "new")) scan_dir(full, relp);
            continue;
        }
        unsigned long long key;
        unsigned hash;
        int level = 0;
        size_t n = strlen(de->d_name);
        if (n > 4 && !strcasecmp(de->d_name + n - 4, ".png") &&
            sscanf(de->d_name, "%16llx%8x_%d", &key, &hash, &level) >= 2 && level == 0)
            add_entry(key, hash, relp);
    }
    closedir(d);
}

static void texrep_init(void) {
    if (g_reload) {  // requested from the UI; applied on the GPU thread
        g_reload = 0;
        for (int i = 0; i < g_nentries; i++) free(g_entries[i].file);
        g_nentries = 0;
        g_hash_kind = HASH_QUICK;
        g_init = 0;
        g_epoch++;
    }
    if (g_set_dump >= 0) {
        if (g_set_dump) setenv("TEX_DUMP", "1", 1);
        else unsetenv("TEX_DUMP");
        g_set_dump = -1;
        g_init = 0;
        g_epoch++;
    }
    if (g_init) return;
    g_init = 1;
    for (int i = 0; i < g_nentries; i++) free(g_entries[i].file);
    g_nentries = 0;
    snprintf(g_dir, sizeof g_dir, "%s", getenv("TEX_DIR") ? getenv("TEX_DIR") : "textures/ULUS10139");
    g_dump = getenv("TEX_DUMP") != NULL;
    load_ini();
    scan_dir(g_dir, "");
    g_enabled = g_nentries > 0;
    if (g_enabled || g_dump)
        fprintf(stderr, "textures: %d replacements in %s%s\n", g_nentries, g_dir, g_dump ? " (dumping)" : "");
    if (g_dump) {
        char p[600];
        snprintf(p, sizeof p, "%s/new", g_dir);
        mkdir("textures", 0755);
        mkdir(g_dir, 0755);
        mkdir(p, 0755);
    }
}

// UI hooks (any thread)
void texrep_request_reload(void) { g_reload = 1; }
void texrep_request_dump(int on) { g_set_dump = on != 0; }
int texrep_epoch(void) {
    texrep_init();
    return g_epoch;
}
void texrep_status(int *replacements, int *dumping, const char **dir) {
    *replacements = g_nentries;
    *dumping = g_dump;
    *dir = g_dir;
}

void texrep_clut_loaded(uint32_t bytes) {
    g_clut_last_bytes = bytes;
    if (bytes > g_clut_max_bytes) g_clut_max_bytes = bytes;
}

// Movie frames are textures too; like PPSSPP, never dump or replace them.
static struct { uint32_t lo, hi; } g_video[8];
static int g_nvideo;

void texrep_note_video(uint32_t addr, uint32_t bytes) {
    addr &= 0x1FFFFFFF;
    for (int i = 0; i < g_nvideo; i++)
        if (g_video[i].lo == addr) {
            g_video[i].hi = addr + bytes;
            return;
        }
    int i = g_nvideo < 8 ? g_nvideo++ : (int)(addr % 8);
    g_video[i].lo = addr;
    g_video[i].hi = addr + bytes;
}

static int is_video(uint32_t addr) {
    addr &= 0x1FFFFFFF;
    for (int i = 0; i < g_nvideo; i++)
        if (addr >= g_video[i].lo && addr < g_video[i].hi) return 1;
    return 0;
}

// ---------------------------------------------------------------- keys

static const int k_bpp[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};

static uint32_t data_hash(const uint8_t *p, uint32_t n) {
    switch (g_hash_kind) {
    case HASH_XXH32: return xxh32(p, n, 0xBACD7814u);
    case HASH_XXH64: return (uint32_t)xxh64(p, n, 0xBACD7814u);
    default: return quick_hash(p, n);
    }
}

// The key PPSSPP would give this texture (level 0).
static int texture_key(uint32_t addr, int fmt, int w, int h, uint32_t texbufwidth, uint32_t texsize, uint32_t clutfmt,
                       uint64_t *cachekey, uint32_t *hash) {
    static const uint32_t align[11] = {0x7F8, 0x7F8, 0x7F8, 0x7FC, 0x7E0, 0x7F0, 0x7F8, 0x7FC, 0x7FF, 0x7FF, 0x7FF};
    if (fmt > 10) return 0;
    uint32_t bpp = (uint32_t)k_bpp[fmt];
    uint32_t bufw = texbufwidth & align[fmt];
    if (!bufw) bufw = 128 / bpp;
    const uint8_t *p = host(addr);
    if (bufw <= (uint32_t)w) {
        uint32_t size = bpp * (bufw * (uint32_t)h + ((uint32_t)w - bufw)) / 8;
        if ((addr & 0x1FFFFFFF) + size > GUEST_MEM_SIZE) return 0;
        *hash = data_hash(p, size);
    } else {
        uint32_t line = bpp * (uint32_t)w / 8, stride = bpp * bufw / 8, r = 0;
        if ((addr & 0x1FFFFFFF) + (uint32_t)(h - 1) * stride + line > GUEST_MEM_SIZE) return 0;
        for (int y = 0; y < h; y++) r = (r * 11) ^ data_hash(p + (uint32_t)y * stride, line);
        *hash = r;
    }
    uint64_t key = ((uint64_t)(addr & 0x3FFFFFFF) << 32) | (texsize & 0xF0F);
    if (fmt >= 4 && fmt <= 7) {  // CLUT formats: XXH32 of the palette as loaded
        uint32_t entry = (clutfmt & 3) == 3 ? 4 : 2;
        uint32_t base = ((clutfmt >> 16) & 0x1F) * 16 * entry;
        uint32_t n = g_clut_last_bytes + base;
        if (n > g_clut_max_bytes) n = g_clut_max_bytes;
        if (n > sizeof g_ge.clut) n = sizeof g_ge.clut;
        // PPSSPP also xors in the whole CLUTFORMAT command word (opcode included)
        key ^= xxh32((const uint8_t *)g_ge.clut, n, 0xC0108888u) ^ (0xC5000000u | (clutfmt & 0xFFFFFF));
    }
    *cachekey = key;
    return 1;
}

static const PackEntry *find(uint64_t key, uint32_t hash) {
    // 1) exact key  2) same size/palette/content at another address (our heap
    // layout differs from PPSSPP's, and ignoreAddress packs omit it)  3) any
    // content at that address (packs with the hash part zeroed)
    for (int i = 0; i < g_nentries; i++)
        if (g_entries[i].cachekey == key && g_entries[i].hash == hash) return &g_entries[i];
    for (int i = 0; i < g_nentries; i++)
        if ((uint32_t)g_entries[i].cachekey == (uint32_t)key && g_entries[i].hash == hash) return &g_entries[i];
    for (int i = 0; i < g_nentries; i++)
        if (g_entries[i].hash == 0 && (g_entries[i].cachekey == key || g_entries[i].cachekey == (uint32_t)key))
            return &g_entries[i];
    return NULL;
}

// Called when a texture is (re)decoded. Returns replacement RGBA pixels
// (malloc'd, size *rw x *rh) or NULL; dumps the original if enabled.
uint32_t *texrep_lookup(uint32_t addr, int fmt, int w, int h, uint32_t texbufwidth, uint32_t texsize,
                        uint32_t clutfmt, const uint32_t *decoded, int *rw, int *rh) {
    texrep_init();
    if ((!g_enabled && !g_dump) || is_video(addr)) return NULL;
    uint64_t key;
    uint32_t hash;
    if (!texture_key(addr, fmt, w, h, texbufwidth, texsize, clutfmt, &key, &hash)) return NULL;
    if (g_enabled) {
        const PackEntry *e = find(key, hash);
        if (e) {
            char path[1100];
            snprintf(path, sizeof path, "%s/%s", g_dir, e->file);
            int n;
            uint8_t *img = stbi_load(path, rw, rh, &n, 4);
            if (img) return (uint32_t *)img;  // RGBA bytes = our ABGR words
            fprintf(stderr, "textures: cannot load %s\n", path);
        }
    }
    if (g_dump) {
        char path[700];
        snprintf(path, sizeof path, "%s/new/%016llx%08x.png", g_dir, (unsigned long long)key, hash);
        struct stat st;
        if (stat(path, &st)) stbi_write_png(path, w, h, 4, decoded, w * 4);
    }
    return NULL;
}
