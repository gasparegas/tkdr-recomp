// sceUtility dialogs: savedata (stored unencrypted under ms0/PSP/SAVEDATA),
// message dialogs (auto-confirmed, text logged), network dialogs (cancelled).
#include <stdlib.h>
#include <sys/stat.h>

#include "hle.h"

enum { ST_NONE = 0, ST_INIT = 1, ST_VISIBLE = 2, ST_QUIT = 3, ST_FINISHED = 4 };

typedef struct {
    int status;
    uint32_t param;
} Dialog;

// GetStatus walks INIT -> VISIBLE -> QUIT; ShutdownStart -> FINISHED -> NONE.
static int dialog_status(Dialog *d) {
    int s = d->status;
    if (d->status == ST_INIT) d->status = ST_VISIBLE;
    else if (d->status == ST_FINISHED) d->status = ST_NONE;
    return s;
}

// ---------------------------------------------------------------- savedata

#define SD_ERR_LOAD_NO_DATA 0x80110307u
#define SD_ERR_LOAD_NO_MS 0x80110301u

static Dialog g_save;

static void save_dir(uint32_t p, uint32_t save_name, char *out, size_t n) {
    char game[14] = {0}, name[21] = {0};
    memcpy(game, host(p + 0x3C), 13);
    memcpy(name, host(save_name), 20);
    snprintf(out, n, "ms0/PSP/SAVEDATA/%s%s", game, name);
}

static uint32_t first_save_name(uint32_t p) {
    uint32_t list = mem_r32(p + 0x60);
    if (*(char *)host(p + 0x4C) || !list) return p + 0x4C;
    return list;  // list modes: first entry of the candidate list
}

static uint32_t do_load(uint32_t p) {
    char dir[256], path[300], file[14] = {0};
    save_dir(p, first_save_name(p), dir, sizeof dir);
    memcpy(file, host(p + 0x64), 13);
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "rb");
    if (!f) return SD_ERR_LOAD_NO_DATA;
    uint32_t cap = mem_r32(p + 0x78);
    size_t n = fread(host(mem_r32(p + 0x74)), 1, cap, f);
    fclose(f);
    mem_w32(p + 0x7C, (uint32_t)n);
    if (first_save_name(p) != p + 0x4C) memcpy(host(p + 0x4C), host(first_save_name(p)), 20);
    return 0;
}

static void mkdirs(const char *path) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *s = tmp + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(tmp, 0755);
            *s = '/';
        }
    mkdir(tmp, 0755);
}

static void write_blob(const char *dir, const char *name, uint32_t ptr, uint32_t size) {
    if (!ptr || !size) return;
    char path[320];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(host(ptr), 1, size, f);
    fclose(f);
}

static uint32_t do_save(uint32_t p) {
    char dir[256], file[14] = {0};
    save_dir(p, first_save_name(p), dir, sizeof dir);
    memcpy(file, host(p + 0x64), 13);
    mkdirs(dir);
    write_blob(dir, file, mem_r32(p + 0x74), mem_r32(p + 0x7C));
    write_blob(dir, "ICON0.PNG", mem_r32(p + 0x584), mem_r32(p + 0x584 + 8));
    write_blob(dir, "PIC1.PNG", mem_r32(p + 0x5A4), mem_r32(p + 0x5A4 + 8));
    // titles, so a human can tell saves apart
    char meta[320];
    snprintf(meta, sizeof meta, "%s/TITLE.TXT", dir);
    FILE *f = fopen(meta, "wb");
    if (f) {
        fprintf(f, "%s\n%s\n%s\n", (char *)host(p + 0x80), (char *)host(p + 0x100), (char *)host(p + 0x180));
        fclose(f);
    }
    return 0;
}

static uint32_t do_sizes(uint32_t p) {
    uint32_t ms_free = mem_r32(p + 0x5D0), ms_data = mem_r32(p + 0x5D4), used = mem_r32(p + 0x5D8);
    if (ms_free) {  // clusterSize, freeClusters, freeSpaceKB, freeSpaceStr[8]
        mem_w32(ms_free, 32768);
        mem_w32(ms_free + 4, 30000);
        mem_w32(ms_free + 8, 30000 * 32);
        snprintf((char *)host(ms_free + 12), 8, "%uMB", 30000u * 32 / 1024);
    }
    uint32_t r = 0;
    if (ms_data) {  // gameName[13]+pad, saveName[20], info{clusters, kb, str[8], kb32, str32[8]}
        char dir[256];
        char game[14] = {0}, name[21] = {0};
        memcpy(game, host(ms_data), 13);
        memcpy(name, host(ms_data + 16), 20);
        snprintf(dir, sizeof dir, "ms0/PSP/SAVEDATA/%s%s", game, name);
        struct stat st;
        if (stat(dir, &st)) r = SD_ERR_LOAD_NO_DATA;
        memset(host(ms_data + 36), 0, 36);
    }
    if (used) memset(host(used), 0, 36);
    return r;
}

static void savedata_run(uint32_t p) {
    uint32_t mode = mem_r32(p + 0x30), r;
    char game[14] = {0}, name[21] = {0}, file[14] = {0};
    memcpy(game, host(p + 0x3C), 13);
    memcpy(name, host(first_save_name(p)), 20);
    memcpy(file, host(p + 0x64), 13);
    switch (mode) {
    case 0: case 2: case 4: r = do_load(p); break;  // (auto/list) load
    case 1: case 3: case 5: r = do_save(p); break;  // (auto/list) save
    case 8: r = do_sizes(p); break;
    default: r = 0; break;
    }
    fprintf(stderr, "savedata: mode %u %s%s/%s -> %08X\n", mode, game, name, file, r);
    mem_w32(p + 0x1C, r);
}

void sceUtilitySavedataInitStart(Ctx *c) {
    if (g_save.status != ST_NONE) { RET(0x80110001u); return; }
    g_save.param = ARG(0);
    g_save.status = ST_INIT;
    savedata_run(ARG(0));
    RET(0);
}
void sceUtilitySavedataGetStatus(Ctx *c) { RET(dialog_status(&g_save)); }
void sceUtilitySavedataUpdate(Ctx *c) {
    if (g_save.status == ST_VISIBLE) g_save.status = ST_QUIT;
    RET(0);
}
void sceUtilitySavedataShutdownStart(Ctx *c) {
    g_save.status = ST_FINISHED;
    RET(0);
}

// ---------------------------------------------------------------- message dialog

static Dialog g_msg;

void sceUtilityMsgDialogInitStart(Ctx *c) {
    uint32_t p = ARG(0);
    if (g_msg.status != ST_NONE) { RET(0x80110001u); return; }
    g_msg.param = p;
    g_msg.status = ST_INIT;
    uint32_t mode = mem_r32(p + 0x34);
    if (mode == 0) fprintf(stderr, "msgdialog: error %08X\n", mem_r32(p + 0x38));
    else fprintf(stderr, "msgdialog: \"%.512s\" (options %08X)\n", (char *)host(p + 0x3C), mem_r32(p + 0x23C));
    mem_w32(p + 0x1C, 0);
    mem_w32(p + 0x240, 1);  // "yes" / OK
    RET(0);
}
void sceUtilityMsgDialogGetStatus(Ctx *c) { RET(dialog_status(&g_msg)); }
void sceUtilityMsgDialogUpdate(Ctx *c) {
    if (g_msg.status == ST_VISIBLE) g_msg.status = ST_QUIT;
    RET(0);
}
void sceUtilityMsgDialogShutdownStart(Ctx *c) {
    g_msg.status = ST_FINISHED;
    RET(0);
}

// ---------------------------------------------------------------- network / sharing
// No ad-hoc or infrastructure networking yet: the dialogs report "cancelled".

static Dialog g_net, g_share;

void sceUtilityNetconfInitStart(Ctx *c) {
    g_net.status = ST_INIT;
    mem_w32(ARG(0) + 0x1C, 1);  // cancelled
    RET(0);
}
void sceUtilityNetconfGetStatus(Ctx *c) { RET(dialog_status(&g_net)); }
void sceUtilityNetconfUpdate(Ctx *c) {
    if (g_net.status == ST_VISIBLE) g_net.status = ST_QUIT;
    RET(0);
}
void sceUtilityNetconfShutdownStart(Ctx *c) { g_net.status = ST_FINISHED; RET(0); }

void sceUtilityGameSharingInitStart(Ctx *c) {
    g_share.status = ST_INIT;
    mem_w32(ARG(0) + 0x1C, 1);
    RET(0);
}
void sceUtilityGameSharingGetStatus(Ctx *c) { RET(dialog_status(&g_share)); }
void sceUtilityGameSharingUpdate(Ctx *c) {
    if (g_share.status == ST_VISIBLE) g_share.status = ST_QUIT;
    RET(0);
}
void sceUtilityGameSharingShutdownStart(Ctx *c) { g_share.status = ST_FINISHED; RET(0); }

void sceUtilityLoadNetModule(Ctx *c) { RET(0); }
void sceUtilityUnloadNetModule(Ctx *c) { RET(0); }
void sceUtilityGetNetParamLatestID(Ctx *c) { mem_w32(ARG(0), 0); RET(0); }

// sceUtilityGetSystemParamString(id, buf, len): only the nickname exists
void sceUtilityGetSystemParamString(Ctx *c) {
    if (ARG(0) == 1) {
        snprintf((char *)host(ARG(1)), ARG(2), "%s", "PLAYER");
        RET(0);
        return;
    }
    RET(0x80110103u);
}
