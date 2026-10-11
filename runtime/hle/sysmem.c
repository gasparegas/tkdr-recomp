// User memory partition allocator and small kernel libraries
// (SysMemUserForUser, Kernel_Library, LoadExec, ModuleMgr, Utils, ...).
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "hle.h"

// ---------------------------------------------------------------- allocator

typedef struct Block {
    uint32_t addr, size;
    int used;
    char name[32];
    struct Block *next;
} Block;

static Block *g_blocks;  // sorted by address, covers the whole free range

void sysmem_init(uint32_t start, uint32_t end) {
    g_blocks = calloc(1, sizeof(Block));
    g_blocks->addr = start;
    g_blocks->size = end - start;
}

static void split(Block *b, uint32_t at) {
    Block *n = calloc(1, sizeof *n);
    n->addr = at;
    n->size = b->addr + b->size - at;
    n->next = b->next;
    b->size = at - b->addr;
    b->next = n;
}

// type: 0 = lowest address, 1 = highest address, 2 = exact address
uint32_t sysmem_alloc(uint32_t size, int type, uint32_t addr, uint32_t align) {
    if (align < 0x100) align = 0x100;
    size = (size + 0xFF) & ~0xFFu;
    Block *pick = NULL;
    uint32_t at = 0;
    for (Block *b = g_blocks; b; b = b->next) {
        if (b->used) continue;
        uint32_t end = b->addr + b->size, a;
        if (type == 2) {
            if (addr < b->addr || addr + size > end) continue;
            a = addr;
        } else if (type == 1) {
            if (size > b->size) continue;
            a = (end - size) & ~(align - 1);
            if (a < b->addr) continue;
        } else {
            a = (b->addr + align - 1) & ~(align - 1);
            if (a + size > end) continue;
        }
        if (type != 1) {  // first fit from the bottom
            pick = b;
            at = a;
            break;
        }
        pick = b;  // keep going: we want the highest fit
        at = a;
    }
    if (!pick) {
        fprintf(stderr, "sysmem: cannot allocate %x bytes (type %d); blocks:\n", size, type);
        for (Block *b = g_blocks; b; b = b->next)
            fprintf(stderr, "  %08X-%08X %s %s\n", b->addr, b->addr + b->size, b->used ? "used" : "FREE", b->name);
        return 0;
    }
    if (at > pick->addr) {
        split(pick, at);
        pick = pick->next;
    }
    if (pick->size > size) split(pick, at + size);
    pick->used = 1;
    return at;
}

void sysmem_free(uint32_t addr) {
    for (Block *b = g_blocks; b; b = b->next)
        if (b->addr == addr && b->used) {
            b->used = 0;
            break;
        }
    for (Block *b = g_blocks; b && b->next;) {  // merge free neighbours
        if (!b->used && !b->next->used) {
            Block *n = b->next;
            b->size += n->size;
            b->next = n->next;
            free(n);
        } else {
            b = b->next;
        }
    }
}

// Partition blocks are referenced by uid; keep a tiny table.
#define MAX_BLOCK_UIDS 256
static uint32_t g_block_uid[MAX_BLOCK_UIDS];

void sceKernelAllocPartitionMemory(Ctx *c) {
    uint32_t addr = sysmem_alloc(ARG(3), (int)ARG(2), ARG(4), 0x100);
    HLOG("  alloc '%s' size=%x type=%u -> %08X\n", (char *)host(ARG(1)), ARG(3), ARG(2), addr);
    if (!addr) { RET(SCE_KERNEL_ERROR_NO_MEMORY); return; }
    for (int k = 1; k < MAX_BLOCK_UIDS; k++)
        if (!g_block_uid[k]) {
            g_block_uid[k] = addr;
            RET(0x2000 + k);
            return;
        }
    RET(SCE_KERNEL_ERROR_NO_MEMORY);
}

static int block_slot(uint32_t uid) {
    int k = (int)uid - 0x2000;
    return k > 0 && k < MAX_BLOCK_UIDS && g_block_uid[k] ? k : 0;
}

void sceKernelGetBlockHeadAddr(Ctx *c) {
    int k = block_slot(ARG(0));
    RET(k ? g_block_uid[k] : 0x800200CCu);
}

void sceKernelFreePartitionMemory(Ctx *c) {
    int k = block_slot(ARG(0));
    if (!k) { RET(0x800200CCu); return; }
    sysmem_free(g_block_uid[k]);
    g_block_uid[k] = 0;
    RET(0);
}

void sceKernelSetCompiledSdkVersion(Ctx *c) { RET(0); }
void sceKernelSetCompilerVersion(Ctx *c) { RET(0); }
void sceKernelPrintf(Ctx *c) {
    fprintf(stderr, "[guest printf] %s", (char *)host(ARG(0)));
    RET(0);
}

// ---------------------------------------------------------------- interrupts

static uint32_t g_intr_enabled = 1;
void sceKernelCpuSuspendIntr(Ctx *c) {
    RET(g_intr_enabled);
    g_intr_enabled = 0;
}
void sceKernelCpuResumeIntr(Ctx *c) {
    g_intr_enabled = ARG(0);
    RET(0);
}

// ---------------------------------------------------------------- modules
// The game loads Sony's own libraries from USRDIR (sascore, atrac, mpeg...).
// Their exports are implemented here as HLE, so loading just succeeds.

static int g_next_module = 0x3001;
void sceKernelLoadModule(Ctx *c) {
    HLOG("  load module %s\n", (char *)host(ARG(0)));
    RET(g_next_module++);
}
void sceKernelStartModule(Ctx *c) {
    if (ARG(3)) mem_w32(ARG(3), 0);
    RET(ARG(0));
}
void sceKernelStopModule(Ctx *c) { RET(0); }
void sceKernelUnloadModule(Ctx *c) { RET(0); }
void sceKernelSelfStopUnloadModule(Ctx *c) { RET(0); }
void sceKernelGetModuleId(Ctx *c) { RET(0x3000); }
void sceKernelGetModuleIdByAddress(Ctx *c) { RET(0x3000); }

void sceKernelExitGame(Ctx *c) {
    (void)c;
    fprintf(stderr, "game called sceKernelExitGame\n");
    exit(0);
}
void sceKernelRegisterExitCallback(Ctx *c) { RET(0); }
void scePowerRegisterCallback(Ctx *c) { RET(0); }

// ---------------------------------------------------------------- stdio/utils

void sceKernelStdin(Ctx *c) { RET(0x10); }
void sceKernelStdout(Ctx *c) { RET(0x11); }
void sceKernelStderr(Ctx *c) { RET(0x12); }

void sceKernelDcacheWritebackAll(Ctx *c) { RET(0); }
void sceKernelDcacheWritebackRange(Ctx *c) { RET(0); }
void sceKernelDcacheWritebackInvalidateRange(Ctx *c) { RET(0); }
void sceKernelDcacheInvalidateRange(Ctx *c) { RET(0); }
void sceKernelSetGPO(Ctx *c) { RET(0); }
void sceKernelGetGPI(Ctx *c) { RET(0); }

void sceKernelLibcClock(Ctx *c) { RET((uint32_t)hle_time_us()); }
void sceKernelLibcTime(Ctx *c) {
    uint32_t t = (uint32_t)time(NULL);
    if (ARG(0)) mem_w32(ARG(0), t);
    RET(t);
}
void sceKernelLibcGettimeofday(Ctx *c) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (ARG(0)) {
        mem_w32(ARG(0), (uint32_t)tv.tv_sec);
        mem_w32(ARG(0) + 4, (uint32_t)tv.tv_usec);
    }
    RET(0);
}

void sceKernelPowerTick(Ctx *c) { RET(0); }
void sceKernelPowerLock(Ctx *c) { RET(0); }
void sceKernelPowerUnlock(Ctx *c) { RET(0); }

// The 4MB "volatile" memory at 0x08400000 (normally used by the system)
static int g_volatile_locked;
void sceKernelVolatileMemTryLock(Ctx *c) {
    if (g_volatile_locked) { RET(0x802B0200u); return; }
    g_volatile_locked = 1;
    mem_w32(ARG(1), 0x08400000u);
    mem_w32(ARG(2), 0x00400000u);
    RET(0);
}
void sceKernelVolatileMemUnlock(Ctx *c) {
    g_volatile_locked = 0;
    RET(0);
}

void sceDmacMemcpy(Ctx *c) {
    memmove(host(ARG(0)), host(ARG(1)), ARG(2));
    RET(0);
}

void sceOpenPSIDGetOpenPSID(Ctx *c) {
    memset(host(ARG(0)), 0x42, 16);
    RET(0);
}
void sceWlanGetSwitchState(Ctx *c) { RET(0); }
