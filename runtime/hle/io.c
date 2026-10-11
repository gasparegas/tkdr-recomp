// File I/O (IoFileMgrForUser): disc0:/umd0: read straight from the ISO image,
// ms0: (memory stick, used for save data) mapped to a host directory.
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hle.h"

#define SCE_ERROR_ERRNO_ENOENT 0x80010002u
#define SCE_ERROR_ERRNO_EBADF 0x80010009u
#define SCE_ERROR_ERRNO_EEXIST 0x80010011u
#define SCE_KERNEL_ERROR_ASYNC_BUSY 0x80020329u
#define SCE_KERNEL_ERROR_NOASYNC 0x8002032Au

#define SECTOR 2048u

// ---------------------------------------------------------------- ISO9660

static FILE *g_iso;
static const char *g_ms_root = "ms0";

typedef struct {
    uint32_t lba, size;
    int is_dir;
} IsoEntry;

static int iso_read(uint64_t off, void *buf, size_t n) {
    if (fseeko(g_iso, (off_t)off, SEEK_SET)) return -1;
    return fread(buf, 1, n, g_iso) == n ? 0 : -1;
}

static IsoEntry g_root;

void io_init(const char *iso_path, const char *ms_root) {
    if (ms_root) g_ms_root = ms_root;
    g_iso = fopen(iso_path, "rb");
    if (!g_iso) {
        perror(iso_path);
        exit(1);
    }
    uint8_t pvd[SECTOR];
    iso_read(16 * SECTOR, pvd, sizeof pvd);
    if (memcmp(pvd + 1, "CD001", 5)) {
        fprintf(stderr, "%s: not an ISO9660 image\n", iso_path);
        exit(1);
    }
    const uint8_t *root = pvd + 156;
    memcpy(&g_root.lba, root + 2, 4);
    memcpy(&g_root.size, root + 10, 4);
    g_root.is_dir = 1;
    mkdir(g_ms_root, 0755);
}

// Find `name` (case-insensitive, ";1" ignored) inside directory `dir`.
static int iso_find(const IsoEntry *dir, const char *name, size_t len, IsoEntry *out) {
    uint8_t *buf = malloc(dir->size);
    if (!buf || iso_read((uint64_t)dir->lba * SECTOR, buf, dir->size)) {
        free(buf);
        return -1;
    }
    int found = -1;
    for (uint32_t o = 0; o < dir->size;) {
        uint8_t rl = buf[o];
        if (!rl) {  // records never cross sectors: skip to the next one
            o = (o / SECTOR + 1) * SECTOR;
            continue;
        }
        uint8_t nl = buf[o + 32];
        const char *n = (const char *)buf + o + 33;
        size_t cmp = nl;
        for (size_t k = 0; k < nl; k++)
            if (n[k] == ';') { cmp = k; break; }
        if (cmp == len && !strncasecmp(n, name, len)) {
            memcpy(&out->lba, buf + o + 2, 4);
            memcpy(&out->size, buf + o + 10, 4);
            out->is_dir = (buf[o + 25] & 2) != 0;
            found = 0;
            break;
        }
        o += rl;
    }
    free(buf);
    return found;
}

static int iso_lookup(const char *path, IsoEntry *out) {
    *out = g_root;
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        const char *end = strchr(path, '/');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        IsoEntry next;
        if (iso_find(out, path, len, &next)) return -1;
        *out = next;
        path += len;
    }
    return 0;
}

// ---------------------------------------------------------------- file table

typedef struct {
    int used, is_iso, is_dir;
    uint32_t lba, size;  // iso file
    uint64_t pos;
    FILE *f;  // host file
    DIR *d;
    char host_path[512];
    // directory listing for iso dirs
    uint8_t *dir_buf;
    uint32_t dir_pos, dir_size;
    // async
    int async_pending;
    uint64_t async_result;
} OpenFile;

#define MAX_FD 64
static OpenFile g_files[MAX_FD];

static int fd_alloc(void) {
    for (int k = 3; k < MAX_FD; k++)
        if (!g_files[k].used) {
            memset(&g_files[k], 0, sizeof g_files[k]);
            g_files[k].used = 1;
            return k;
        }
    return -1;
}

static OpenFile *fd_get(uint32_t fd) {
    return fd < MAX_FD && g_files[fd].used ? &g_files[fd] : NULL;
}

// Split "dev:/path" -> device kind; returns pointer to the path part.
enum { DEV_DISC, DEV_MS, DEV_OTHER };
static int g_cwd_dev = DEV_DISC;
static char g_cwd[256] = "/PSP_GAME/USRDIR";

static const char *parse_path(const char *p, int *dev, char *tmp, size_t tmpsz) {
    const char *colon = strchr(p, ':');
    if (!colon) {  // relative to the current directory
        *dev = g_cwd_dev;
        snprintf(tmp, tmpsz, "%s/%s", g_cwd, p);
        return tmp;
    }
    if (!strncmp(p, "disc0:", 6) || !strncmp(p, "umd0:", 5) || !strncmp(p, "umd1:", 5) ||
        !strncmp(p, "isofs0:", 7))
        *dev = DEV_DISC;
    else if (!strncmp(p, "ms0:", 4) || !strncmp(p, "fatms0:", 7))
        *dev = DEV_MS;
    else
        *dev = DEV_OTHER;
    return colon + 1;
}

static void host_path(char *out, size_t n, const char *p) {
    snprintf(out, n, "%s/%s", g_ms_root, *p == '/' ? p + 1 : p);
}

// "/sce_lbn0x5fa0_size0x1000": raw sector range on the disc
static int parse_lbn(const char *p, uint32_t *lba, uint32_t *size) {
    const char *s = strstr(p, "sce_lbn");
    if (!s) return 0;
    char *e;
    *lba = (uint32_t)strtoul(s + 7, &e, 0);
    const char *z = strstr(e, "size");
    *size = z ? (uint32_t)strtoul(z + 4, NULL, 0) : 0;
    return 1;
}

#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT 0x0200
#define PSP_O_TRUNC 0x0400
#define PSP_O_EXCL 0x0800

static int32_t do_open(uint32_t path_ptr, uint32_t flags) {
    char tmp[512];
    int dev;
    const char *p = parse_path((const char *)host(path_ptr), &dev, tmp, sizeof tmp);
    if (dev == DEV_DISC) {
        uint32_t lba, size;
        IsoEntry e;
        if (parse_lbn(p, &lba, &size)) {
            e.lba = lba;
            e.size = size;
            e.is_dir = 0;
        } else if (iso_lookup(p, &e) || e.is_dir) {
            HLOG("  open %s: not found\n", (char *)host(path_ptr));
            return (int32_t)SCE_ERROR_ERRNO_ENOENT;
        }
        int fd = fd_alloc();
        g_files[fd].is_iso = 1;
        g_files[fd].lba = e.lba;
        g_files[fd].size = e.size;
        HLOG("  open %s -> fd %d (lba %x size %x)\n", (char *)host(path_ptr), fd, e.lba, e.size);
        return fd;
    }
    if (dev == DEV_MS) {
        char hp[512];
        host_path(hp, sizeof hp, p);
        const char *mode;
        if ((flags & PSP_O_EXCL) && (flags & PSP_O_CREAT) && access(hp, F_OK) == 0)
            return (int32_t)SCE_ERROR_ERRNO_EEXIST;
        if (flags & PSP_O_WRONLY) {
            if (flags & PSP_O_TRUNC) mode = (flags & PSP_O_RDONLY) ? "w+b" : "wb";
            else if (access(hp, F_OK) != 0) {
                if (!(flags & PSP_O_CREAT)) return (int32_t)SCE_ERROR_ERRNO_ENOENT;
                mode = "w+b";
            } else mode = "r+b";
        } else {
            mode = "rb";
        }
        FILE *f = fopen(hp, mode);
        if (!f) return (int32_t)SCE_ERROR_ERRNO_ENOENT;
        if (flags & PSP_O_APPEND) fseek(f, 0, SEEK_END);
        int fd = fd_alloc();
        g_files[fd].f = f;
        snprintf(g_files[fd].host_path, sizeof g_files[fd].host_path, "%s", hp);
        HLOG("  open %s -> fd %d (host)\n", hp, fd);
        return fd;
    }
    fprintf(stderr, "io: unsupported device in %s\n", (char *)host(path_ptr));
    return (int32_t)SCE_ERROR_ERRNO_ENOENT;
}

static int32_t do_read(OpenFile *f, uint32_t buf, uint32_t n) {
    if (f->is_iso) {
        if (f->pos >= f->size) return 0;
        if (n > f->size - f->pos) n = (uint32_t)(f->size - f->pos);
        if (iso_read((uint64_t)f->lba * SECTOR + f->pos, host(buf), n)) return -1;
        f->pos += n;
        return (int32_t)n;
    }
    return (int32_t)fread(host(buf), 1, n, f->f);
}

static int64_t do_seek(OpenFile *f, int64_t off, int whence) {
    if (f->is_iso) {
        int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->pos : (int64_t)f->size;
        if (base + off < 0) return (int64_t)(int32_t)0x80010016u;  // EINVAL
        f->pos = (uint64_t)(base + off);
        return (int64_t)f->pos;
    }
    fseeko(f->f, (off_t)off, whence);
    return (int64_t)ftello(f->f);
}

static void do_close(uint32_t fd) {
    OpenFile *f = fd_get(fd);
    if (!f) return;
    if (f->f) fclose(f->f);
    if (f->d) closedir(f->d);
    free(f->dir_buf);
    f->used = 0;
}

// ---------------------------------------------------------------- sync API

void sceIoOpen(Ctx *c) { RET(do_open(ARG(0), ARG(1))); }

void sceIoClose(Ctx *c) {
    if (!fd_get(ARG(0))) { RET(SCE_ERROR_ERRNO_EBADF); return; }
    do_close(ARG(0));
    RET(0);
}

void sceIoRead(Ctx *c) {
    OpenFile *f = fd_get(ARG(0));
    if (!f) { RET(SCE_ERROR_ERRNO_EBADF); return; }
    RET(do_read(f, ARG(1), ARG(2)));
}

void sceIoWrite(Ctx *c) {
    uint32_t fd = ARG(0);
    if (fd == 1 || fd == 2 || fd == 0x11 || fd == 0x12) {  // stdout / stderr
        fwrite(host(ARG(1)), 1, ARG(2), stderr);
        RET(ARG(2));
        return;
    }
    OpenFile *f = fd_get(fd);
    if (!f || !f->f) { RET(SCE_ERROR_ERRNO_EBADF); return; }
    RET(fwrite(host(ARG(1)), 1, ARG(2), f->f));
}

// sceIoLseek(fd, s64 offset [a2:a3], whence [t0])
void sceIoLseek(Ctx *c) {
    OpenFile *f = fd_get(ARG(0));
    if (!f) { RET64((uint64_t)(int64_t)(int32_t)SCE_ERROR_ERRNO_EBADF); return; }
    int64_t off = (int64_t)(ARG(2) | ((uint64_t)ARG(3) << 32));
    RET64((uint64_t)do_seek(f, off, (int)ARG(4)));
}

// ---------------------------------------------------------------- async API
// Operations complete immediately; the result is kept until waited for.

static void async_done(OpenFile *f, uint64_t result) {
    f->async_pending = 1;
    f->async_result = result;
}

void sceIoOpenAsync(Ctx *c) {
    int32_t fd = do_open(ARG(0), ARG(1));
    if (fd < 0) {  // the PSP still hands out an fd to report the error through
        int k = fd_alloc();
        g_files[k].is_iso = 1;
        async_done(&g_files[k], (uint64_t)(int64_t)fd);
        RET(k);
        return;
    }
    async_done(&g_files[fd], (uint64_t)fd);
    RET(fd);
}

void sceIoCloseAsync(Ctx *c) {
    OpenFile *f = fd_get(ARG(0));
    if (!f) { RET(SCE_ERROR_ERRNO_EBADF); return; }
    if (f->f) { fclose(f->f); f->f = NULL; }
    async_done(f, 0);
    f->is_dir = -1;  // closed once the result is collected
    RET(0);
}

void sceIoReadAsync(Ctx *c) {
    OpenFile *f = fd_get(ARG(0));
    if (!f) { RET(SCE_ERROR_ERRNO_EBADF); return; }
    async_done(f, (uint64_t)(int64_t)do_read(f, ARG(1), ARG(2)));
    RET(0);
}

void sceIoLseekAsync(Ctx *c) {
    OpenFile *f = fd_get(ARG(0));
    if (!f) { RET(SCE_ERROR_ERRNO_EBADF); return; }
    int64_t off = (int64_t)(ARG(2) | ((uint64_t)ARG(3) << 32));
    async_done(f, (uint64_t)do_seek(f, off, (int)ARG(4)));
    RET(0);
}

static void collect(Ctx *c, int poll) {
    (void)poll;
    OpenFile *f = fd_get(ARG(0));
    if (!f) { RET(SCE_ERROR_ERRNO_EBADF); return; }
    if (!f->async_pending) { RET(SCE_KERNEL_ERROR_NOASYNC); return; }
    mem_w32(ARG(1), (uint32_t)f->async_result);
    mem_w32(ARG(1) + 4, (uint32_t)(f->async_result >> 32));
    f->async_pending = 0;
    if (f->is_dir == -1) do_close(ARG(0));
    RET(0);
}
void sceIoWaitAsync(Ctx *c) { collect(c, 0); }
void sceIoWaitAsyncCB(Ctx *c) { collect(c, 0); }
void sceIoPollAsync(Ctx *c) { collect(c, 1); }

// ---------------------------------------------------------------- metadata

// st_private[0] holds the start sector of UMD files (games use it to build
// "sce_lbn" raw paths).
static void write_stat(uint32_t p, int is_dir, uint64_t size, uint32_t lba) {
    memset(host(p), 0, 0x58);
    mem_w32(p + 0x40, lba);
    mem_w32(p, is_dir ? 0x11FF : 0x21FF);
    mem_w32(p + 4, is_dir ? 0x10 : 0x20);
    mem_w32(p + 8, (uint32_t)size);
    mem_w32(p + 12, (uint32_t)(size >> 32));
    for (uint32_t t = 0x10; t < 0x40; t += 0x10) {  // ScePspDateTime: 2006-06-24
        mem_w16(p + t, 2006);
        mem_w16(p + t + 2, 6);
        mem_w16(p + t + 4, 24);
    }
}

void sceIoGetstat(Ctx *c) {
    char tmp[512];
    int dev;
    const char *p = parse_path((const char *)host(ARG(0)), &dev, tmp, sizeof tmp);
    if (dev == DEV_DISC) {
        IsoEntry e;
        if (iso_lookup(p, &e)) { RET(SCE_ERROR_ERRNO_ENOENT); return; }
        write_stat(ARG(1), e.is_dir, e.size, e.lba);
        RET(0);
        return;
    }
    char hp[512];
    host_path(hp, sizeof hp, p);
    struct stat st;
    if (dev != DEV_MS || stat(hp, &st)) { RET(SCE_ERROR_ERRNO_ENOENT); return; }
    write_stat(ARG(1), S_ISDIR(st.st_mode), (uint64_t)st.st_size, 0);
    RET(0);
}

void sceIoChstat(Ctx *c) { RET(0); }

static int ms_op(Ctx *c, uint32_t path_ptr, char *hp, size_t n) {
    char tmp[512];
    int dev;
    const char *p = parse_path((const char *)host(path_ptr), &dev, tmp, sizeof tmp);
    if (dev != DEV_MS) return -1;
    (void)c;
    host_path(hp, n, p);
    return 0;
}

void sceIoMkdir(Ctx *c) {
    char hp[512];
    if (ms_op(c, ARG(0), hp, sizeof hp)) { RET(SCE_ERROR_ERRNO_ENOENT); return; }
    RET(mkdir(hp, 0755) == 0 ? 0 : (errno == EEXIST ? SCE_ERROR_ERRNO_EEXIST : SCE_ERROR_ERRNO_ENOENT));
}
void sceIoRmdir(Ctx *c) {
    char hp[512];
    if (ms_op(c, ARG(0), hp, sizeof hp)) { RET(SCE_ERROR_ERRNO_ENOENT); return; }
    RET(rmdir(hp) == 0 ? 0 : SCE_ERROR_ERRNO_ENOENT);
}
void sceIoRemove(Ctx *c) {
    char hp[512];
    if (ms_op(c, ARG(0), hp, sizeof hp)) { RET(SCE_ERROR_ERRNO_ENOENT); return; }
    RET(unlink(hp) == 0 ? 0 : SCE_ERROR_ERRNO_ENOENT);
}
void sceIoRename(Ctx *c) {
    char a[512], b[512];
    if (ms_op(c, ARG(0), a, sizeof a) || ms_op(c, ARG(1), b, sizeof b)) {
        RET(SCE_ERROR_ERRNO_ENOENT);
        return;
    }
    RET(rename(a, b) == 0 ? 0 : SCE_ERROR_ERRNO_ENOENT);
}

void sceIoChdir(Ctx *c) {
    char tmp[512];
    int dev;
    const char *p = parse_path((const char *)host(ARG(0)), &dev, tmp, sizeof tmp);
    g_cwd_dev = dev;
    snprintf(g_cwd, sizeof g_cwd, "%s", p);
    RET(0);
}

// ---------------------------------------------------------------- directories

void sceIoDopen(Ctx *c) {
    char tmp[512];
    int dev;
    const char *p = parse_path((const char *)host(ARG(0)), &dev, tmp, sizeof tmp);
    int fd = fd_alloc();
    if (fd < 0) { RET(SCE_ERROR_ERRNO_ENOENT); return; }
    OpenFile *f = &g_files[fd];
    if (dev == DEV_DISC) {
        IsoEntry e;
        if (iso_lookup(p, &e) || !e.is_dir) { f->used = 0; RET(SCE_ERROR_ERRNO_ENOENT); return; }
        f->is_iso = f->is_dir = 1;
        f->dir_buf = malloc(e.size);
        f->dir_size = e.size;
        iso_read((uint64_t)e.lba * SECTOR, f->dir_buf, e.size);
    } else {
        char hp[512];
        host_path(hp, sizeof hp, p);
        f->d = opendir(hp);
        if (!f->d) { f->used = 0; RET(SCE_ERROR_ERRNO_ENOENT); return; }
        f->is_dir = 1;
        snprintf(f->host_path, sizeof f->host_path, "%s", hp);
    }
    RET(fd);
}

void sceIoDread(Ctx *c) {
    OpenFile *f = fd_get(ARG(0));
    if (!f || !f->is_dir) { RET(SCE_ERROR_ERRNO_EBADF); return; }
    uint32_t ent = ARG(1);
    memset(host(ent), 0, 0x58 + 256);
    if (f->is_iso) {
        while (f->dir_pos < f->dir_size) {
            uint8_t *r = f->dir_buf + f->dir_pos, rl = r[0];
            if (!rl) { f->dir_pos = (f->dir_pos / SECTOR + 1) * SECTOR; continue; }
            f->dir_pos += rl;
            uint8_t nl = r[32];
            if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;  // "." and ".."
            uint32_t size, lba;
            memcpy(&size, r + 10, 4);
            memcpy(&lba, r + 2, 4);
            write_stat(ent, (r[25] & 2) != 0, size, lba);
            char *name = (char *)host(ent + 0x58);
            int k = 0;
            for (; k < nl && r[33 + k] != ';'; k++) name[k] = (char)r[33 + k];
            name[k] = 0;
            RET(1);
            return;
        }
        RET(0);
        return;
    }
    struct dirent *de;
    while ((de = readdir(f->d)) && de->d_name[0] == '.' && (!de->d_name[1] || de->d_name[1] == '.'))
        ;
    if (!de) { RET(0); return; }
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", f->host_path, de->d_name);
    struct stat st;
    stat(full, &st);
    write_stat(ent, S_ISDIR(st.st_mode), (uint64_t)st.st_size, 0);
    snprintf((char *)host(ent + 0x58), 256, "%s", de->d_name);
    RET(1);
}

void sceIoDclose(Ctx *c) {
    if (!fd_get(ARG(0))) { RET(SCE_ERROR_ERRNO_EBADF); return; }
    do_close(ARG(0));
    RET(0);
}

// sceIoDevctl(dev, cmd, indata, inlen, outdata, outlen)
void sceIoDevctl(Ctx *c) {
    uint32_t cmd = ARG(1), out = ARG(4), outlen = ARG(5);
    HLOG("  devctl %s cmd=%08X\n", (char *)host(ARG(0)), cmd);
    switch (cmd) {
    case 0x02425818:  // ms0: free space -> pointer to a 5-word struct
        if (ARG(2)) {
            uint32_t p = mem_r32(ARG(2));
            mem_w32(p, 0x10000);  // total clusters
            mem_w32(p + 4, 0x8000);  // free clusters
            mem_w32(p + 8, 0x8000);
            mem_w32(p + 12, 512);  // sector size
            mem_w32(p + 16, 64);  // sectors per cluster
        }
        break;
    case 0x02425823:  // ms0: inserted?
    case 0x02025806:
        if (out && outlen >= 4) mem_w32(out, 1);
        break;
    default:
        if (out && outlen >= 4) mem_w32(out, 0);
        break;
    }
    RET(0);
}
