// Browser glue: disc reads go through the page (Module.discRead), on the main thread.
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Read [off, off+len) of the user's ISO into ptr, then set *flag = 1.
// Runs asynchronously on the main thread; the caller (a worker) waits.
static void request_read(double off, void *ptr, int len, int *flag) {
    MAIN_THREAD_ASYNC_EM_ASM(
        {
            var off = $0;
            var ptr = $1;
            var len = $2;
            var flag = $3;
            // Module.discRead: the page's reader (plain ISO or compressed CSO, see web/disc.mjs)
            var src = Module.isoUrl
                ? fetch(Module.isoUrl, {headers: {Range: 'bytes=' + off + '-' + (off + len - 1)}})
                      .then(function(r) { return r.arrayBuffer(); })
                : Module.discRead(off, len);
            src.then(function(b) {
                var bytes = new Uint8Array(b instanceof Uint8Array ? b.buffer : b,
                                           b instanceof Uint8Array ? b.byteOffset : 0);
                HEAPU8.set(bytes.subarray(0, Math.min(bytes.length, len)), ptr);
                Atomics.store(HEAP32, flag >> 2, 1);
                Atomics.notify(HEAP32, flag >> 2);
            });
        },
        off, ptr, len, flag);
}

static void read_blocking(uint64_t off, void *buf, size_t n) {
    int flag = 0;
    request_read((double)off, buf, (int)n, &flag);
    while (!__atomic_load_n(&flag, __ATOMIC_ACQUIRE)) emscripten_futex_wait(&flag, 0, INFINITY);
}

// LRU cache of 256 KB blocks: games read in small chunks.
#define BLOCK (256 * 1024)
#define NBLOCKS 48
static struct {
    uint64_t base;
    uint64_t used;
    uint8_t *data;
} g_blocks[NBLOCKS];
static uint64_t g_tick;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

int web_iso_read(uint64_t off, void *buf, size_t n) {
    uint8_t *out = buf;
    pthread_mutex_lock(&g_lock);
    while (n) {
        uint64_t base = off / BLOCK * BLOCK;
        int slot = -1, victim = 0;
        for (int k = 0; k < NBLOCKS; k++) {
            if (g_blocks[k].data && g_blocks[k].base == base) slot = k;
            if (g_blocks[k].used < g_blocks[victim].used) victim = k;
        }
        if (slot < 0) {
            slot = victim;
            if (!g_blocks[slot].data) g_blocks[slot].data = malloc(BLOCK);
            read_blocking(base, g_blocks[slot].data, BLOCK);
            g_blocks[slot].base = base;
        }
        g_blocks[slot].used = ++g_tick;
        size_t in = (size_t)(off - base), take = BLOCK - in < n ? BLOCK - in : n;
        memcpy(out, g_blocks[slot].data + in, take);
        out += take;
        off += take;
        n -= take;
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

// Save data lives in /ms0, an IndexedDB-backed directory mounted by the page.
void web_persist(void) {
    MAIN_THREAD_ASYNC_EM_ASM({
        FS.syncfs(false, function(err) {
            if (err) console.warn('saving to IndexedDB failed', err);
        });
    });
}
#else
void web_persist(void) {}
#endif
