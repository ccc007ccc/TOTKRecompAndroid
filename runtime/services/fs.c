// fsp-srv: RomFS as a raw storage image, save data and SD card backed by host directories.
#include "../diagnostics_build.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../mods.h"
#include "../hle.h"
#include "../ipc.h"
#include "../romfs_build.h"
#include "../test_boot.h"
#include "../test_session.h"
#include "fs.h"

#define FS_PATH_NOT_FOUND MAKE_RESULT(2, 1)
#define FS_PATH_EXISTS MAKE_RESULT(2, 2)
#define FS_TARGET_LOCKED MAKE_RESULT(2, 7)
#define FS_NOT_EMPTY MAKE_RESULT(2, 8)
#define FS_TARGET_NOT_FOUND MAKE_RESULT(2, 1002)
#define FS_INVALID_ARGUMENT MAKE_RESULT(2, 6001)
#define FS_UNSUPPORTED MAKE_RESULT(2, 6300)

const char *g_save_dir = "save";
static int romfs_fd = -1;

// ------------------------------------------------------------------ RomFS file index (offset -> path)
typedef struct { uint64_t start, size; char *path; int fd; const uint8_t *mem; } Overlay;
// in-image patches (copies of RomFS tables with changes): header, dir meta, file hash, file meta
typedef struct { uint64_t off, size; uint8_t *data; } Patch;
static Patch patches[8];
static int npatches;
static Overlay *ov;
static int ov_n, ov_cap;
static uint8_t *ov_meta;
static uint64_t ov_meta_off, ov_meta_size, ov_end;
// Lets the runtime see which game files are being streamed (e.g. which level is loading).
typedef struct { uint64_t off, size; char *path; uint32_t meta; } RomFile;
static RomFile *rom_files;
static int rom_nfiles;
static uint64_t rom_data_off;

// Reboot: the next boot builds its own index / overlay when the game opens the RomFS again.
RT_ON_RESET(fs_reset) {
    for (int i = 0; i < ov_n; i++) {
        if (ov[i].fd >= 0) close(ov[i].fd);
        free(ov[i].path);
    }
    for (int i = 0; i < npatches; i++) free(patches[i].data);  // (ov_meta and the extended table are among these)
    for (int i = 0; i < rom_nfiles; i++) free(rom_files[i].path);
    free(ov), free(rom_files);
    ov = NULL, ov_n = ov_cap = 0;
    ov_meta = NULL, ov_meta_off = ov_meta_size = ov_end = 0;
    npatches = 0;
    rom_files = NULL, rom_nfiles = 0, rom_data_off = 0;
    if (romfs_fd >= 0) close(romfs_fd);
    romfs_fd = -1;
    g_save_dir = "save";
}

// RomFS mods: LARECOMP_MODS=<folder> with a romfs/ laid out like the game's; its files replace the game's files of the
// same path, and files the game doesn't have are added (in directories it has).
static const char *mods_dir(void) {
    const char *e = getenv("LARECOMP_MODS");
    return e && *e ? e : NULL;
}

static bool mods_override(const char *path, char *out, size_t cap) {
    const char *d = mods_dir();
    if (!d) return false;
    snprintf(out, cap, "%s/romfs/%s", d, path);
    struct stat sb;
    return stat(out, &sb) == 0 && S_ISREG(sb.st_mode);
}

static bool test_boot_override(int fd, const RomFile *file, char *out, size_t cap) {
    int slot = test_session_save_slot();
    if (slot < 0 || !test_boot_target(file->path)) return false;
    if (file->size > 32768) FATAL("Unsupported direct-load sequence: %s\n", file->path);
    uint8_t *data = malloc((size_t)file->size);
    if (!data || pread(fd, data, file->size, (off_t)file->off) != (ssize_t)file->size ||
        test_boot_patch(file->path, data, (size_t)file->size, slot) != 1)
        FATAL("Direct loading requires the original TotK 1.4.3 sequence: %s\n", file->path);
    int n = snprintf(out, cap, "%s/boot-%s", test_session_directory(), strrchr(file->path, '/') + 1);
    if (n < 0 || (size_t)n >= cap) FATAL("Direct-load path is too long\n");
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(data, 1, file->size, f) != file->size || fclose(f))
        FATAL("Cannot write direct-load sequence: %s\n", out);
    free(data);
    LOG("[test-session] direct slot %02d: %s\n", slot, file->path);
    return true;
}

static int rom_cmp(const void *a, const void *b) {
    const RomFile *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

static void rom_dir_path(const uint8_t *dmeta, uint32_t dsize, uint32_t dir, char *out, size_t cap) {
    char parts[16][256];
    int n = 0;
    while (dir != 0 && dir < dsize && n < 16) {  // root (offset 0) has an empty name
        uint32_t parent, namelen;
        memcpy(&parent, dmeta + dir, 4);
        memcpy(&namelen, dmeta + dir + 0x14, 4);
        if (namelen > 255) namelen = 255;
        memcpy(parts[n], dmeta + dir + 0x18, namelen);
        parts[n][namelen] = 0;
        n++;
        dir = parent;
    }
    out[0] = 0;
    for (int i = n - 1; i >= 0; i--) { strlcat(out, parts[i], cap); strlcat(out, "/", cap); }
}

// ---- files a mod ADDS (names the game's RomFS doesn't have). The game parses the
// RomFS tables itself, so they're extended: a copy of the file-meta table with the new entries appended (linked
// into their directory's file list and the name hash buckets) is placed past the end of the image, the header,
// directory table and hash table are patched to match. New files must go into existing directories.
static uint32_t romfs_hash(uint32_t parent, const char *name, uint32_t len) {
    uint32_t hash = parent ^ 123456789;
    for (uint32_t i = 0; i < len; i++) { hash = (hash >> 5) | (hash << 27); hash ^= (uint8_t)name[i]; }
    return hash;
}

static int path_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

typedef struct { char rel[1024]; char host[1200]; uint64_t size; } NewFile;

static void walk_new(const char *root, const char *rel, char **known, int nknown, NewFile **out, int *n, int *cap) {
    char dirp[1200];
    snprintf(dirp, sizeof dirp, "%s/%s", root, rel);
    DIR *d = opendir(dirp);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char r2[1024], h2[1200];
        snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(h2, sizeof h2, "%s/%s", root, r2);
        struct stat sb;
        if (stat(h2, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) { walk_new(root, r2, known, nknown, out, n, cap); continue; }
        const char *key = r2;
        if (bsearch(&key, known, nknown, sizeof *known, path_cmp)) continue;  // replaces an existing file
        if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *out = realloc(*out, sizeof **out * *cap); }
        strlcpy((*out)[*n].rel, r2, sizeof (*out)[*n].rel);
        strlcpy((*out)[*n].host, h2, sizeof (*out)[*n].host);
        (*out)[*n].size = (uint64_t)sb.st_size;
        (*n)++;
    }
    closedir(d);
}

// directory entry offset for "a/b/c" (NULL-terminated walk from the root, offset 0), -1 if missing
static int64_t dir_lookup(const uint8_t *dm, uint64_t dsz, const char *path) {
    uint32_t cur = 0;
    char buf[1024];
    strlcpy(buf, path, sizeof buf);
    for (char *tok = strtok(buf, "/"); tok; tok = strtok(NULL, "/")) {
        uint32_t child;
        memcpy(&child, dm + cur + 8, 4);
        while (child != 0xFFFFFFFFu && child + 0x18 <= dsz) {
            uint32_t nl;
            memcpy(&nl, dm + child + 0x14, 4);
            if (nl == strlen(tok) && !memcmp(dm + child + 0x18, tok, nl)) break;
            memcpy(&child, dm + child + 4, 4);
        }
        if (child == 0xFFFFFFFFu || child + 0x18 > dsz) return -1;
        cur = child;
    }
    return cur;
}

static void add_new_files(int fd, const uint64_t *h, const uint8_t *dmeta, uint64_t dsz, uint64_t fsz) {
    const char *mods = mods_dir();
    if (!mods) return;
    char root[1100];
    snprintf(root, sizeof root, "%s/romfs", mods);
    char **known = malloc(sizeof *known * rom_nfiles);
    for (int i = 0; i < rom_nfiles; i++) known[i] = rom_files[i].path;
    qsort(known, rom_nfiles, sizeof *known, path_cmp);
    NewFile *nf = NULL;
    int n = 0, cap = 0;
    walk_new(root, "", known, rom_nfiles, &nf, &n, &cap);
    free(known);
    if (!n) return;
    rom_files = realloc(rom_files, sizeof(RomFile) * (rom_nfiles + n));
    uint64_t hoff = h[5], hsz = h[6];
    uint8_t *dcopy = malloc(dsz), *hcopy = malloc(hsz), *head = malloc(0x50);
    memcpy(dcopy, dmeta, dsz);
    if (pread(fd, hcopy, hsz, (off_t)hoff) != (ssize_t)hsz || pread(fd, head, 0x50, 0) != 0x50) return;
    // the file-meta table to extend: the override-patched copy if there is one
    const uint8_t *base_meta = ov_meta;
    uint8_t *tmp = NULL;
    if (!base_meta) { tmp = malloc(fsz); pread(fd, tmp, fsz, (off_t)h[7]); base_meta = tmp; }
    uint64_t extra = 0;
    for (int i = 0; i < n; i++) {
        const char *name = strrchr(nf[i].rel, '/');
        name = name ? name + 1 : nf[i].rel;
        extra += 0x20 + ((strlen(name) + 3) & ~3u);
    }
    uint64_t nsz = fsz + extra;
    uint8_t *meta = calloc(1, nsz);
    memcpy(meta, base_meta, fsz);
    free(tmp);
    if (!ov_end) {
        struct stat rs;
        fstat(fd, &rs);
        ov_end = (rs.st_size + 0xF) & ~0xFull;
    }
    uint64_t meta_at = ov_end;  // the new table first, then the new files' data
    uint64_t data_at = (meta_at + nsz + 0xF) & ~0xFull;
    uint32_t nb = (uint32_t)(hsz / 4);
    uint64_t o = fsz;
    int added = 0;
    for (int i = 0; i < n; i++) {
        char dirp[1024];
        strlcpy(dirp, nf[i].rel, sizeof dirp);
        char *slash = strrchr(dirp, '/');
        const char *name = slash ? slash + 1 : nf[i].rel;
        if (slash) *slash = 0; else dirp[0] = 0;
        int64_t dir = dir_lookup(dcopy, dsz, dirp);
        if (dir < 0) { LOG("[fs] mod file %s: directory not in the game's RomFS, skipped\n", nf[i].rel); continue; }
        uint32_t nl = (uint32_t)strlen(name), d32 = (uint32_t)dir, entry = (uint32_t)o, child;
        memcpy(&child, dcopy + dir + 0xC, 4);  // dir's first file -> becomes our sibling
        uint64_t rel = data_at - rom_data_off, size = nf[i].size;
        uint32_t bucket = romfs_hash(d32, name, nl) % nb, prev;
        memcpy(&prev, hcopy + 4 * bucket, 4);
        memcpy(meta + o, &d32, 4);
        memcpy(meta + o + 4, &child, 4);
        memcpy(meta + o + 8, &rel, 8);
        memcpy(meta + o + 0x10, &size, 8);
        memcpy(meta + o + 0x18, &prev, 4);
        memcpy(meta + o + 0x1C, &nl, 4);
        memcpy(meta + o + 0x20, name, nl);
        memcpy(dcopy + dir + 0xC, &entry, 4);
        memcpy(hcopy + 4 * bucket, &entry, 4);
        o += 0x20 + ((nl + 3) & ~3u);
        if (ov_n == ov_cap) { ov_cap = ov_cap ? ov_cap * 2 : 256; ov = realloc(ov, sizeof *ov * ov_cap); }
        ov[ov_n++] = (Overlay){data_at, size, strdup(nf[i].host), -1, NULL};
        rom_files[rom_nfiles++] = (RomFile){data_at, size, strdup(nf[i].rel), entry};
        data_at = (data_at + size + 0xF) & ~0xFull;
        added++;
    }
    // the new table goes before the new data (keep the overlay list sorted by start)
    if (ov_n == ov_cap) { ov_cap = ov_cap ? ov_cap * 2 : 256; ov = realloc(ov, sizeof *ov * ov_cap); }
    memmove(ov + (ov_n - added) + 1, ov + (ov_n - added), sizeof *ov * added);
    ov[ov_n - added] = (Overlay){meta_at, nsz, NULL, -1, meta};
    ov_n++;
    ov_end = data_at;
    uint64_t fmeta_off = meta_at, fmeta_size = o;
    memcpy(head + 7 * 8, &fmeta_off, 8);
    memcpy(head + 8 * 8, &fmeta_size, 8);
    patches[npatches++] = (Patch){0, 0x50, head};
    patches[npatches++] = (Patch){h[3], dsz, dcopy};
    patches[npatches++] = (Patch){hoff, hsz, hcopy};
    LOG("[fs] mods: %d new romfs files\n", added);
}

static void rom_index(int fd) {
    uint64_t h[10];
    if (pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h) return;
    // header: size, dir hash (off,size), dir meta (off,size), file hash (off,size), file meta (off,size), data off
    uint64_t doff = h[3], dsz = h[4], foff = h[7], fsz = h[8];
    rom_data_off = h[9];
    uint8_t *dmeta = malloc(dsz), *fmeta = malloc(fsz);
    if (pread(fd, dmeta, dsz, (off_t)doff) != (ssize_t)dsz || pread(fd, fmeta, fsz, (off_t)foff) != (ssize_t)fsz) return;
    int cap = 4096;
    rom_files = malloc(sizeof(RomFile) * cap);
    for (uint64_t o = 0; o + 0x20 <= fsz;) {
        uint32_t parent, namelen;
        uint64_t data, size;
        memcpy(&parent, fmeta + o, 4);
        memcpy(&data, fmeta + o + 8, 8);
        memcpy(&size, fmeta + o + 0x10, 8);
        memcpy(&namelen, fmeta + o + 0x1C, 4);
        char dir[1024], name[256];
        rom_dir_path(dmeta, (uint32_t)dsz, parent, dir, sizeof dir);
        uint32_t nl = namelen > 255 ? 255 : namelen;
        memcpy(name, fmeta + o + 0x20, nl);
        name[nl] = 0;
        if (rom_nfiles == cap) { cap *= 2; rom_files = realloc(rom_files, sizeof(RomFile) * cap); }
        char *path = malloc(strlen(dir) + nl + 1);
        strcpy(path, dir);
        strcat(path, name);
        rom_files[rom_nfiles++] = (RomFile){rom_data_off + data, size, path, (uint32_t)o};
        o += 0x20 + ((namelen + 3) & ~3u);
    }
    // Mods: replacement files are served from the mods folder (LARECOMP_MODS), then from the game mods turned on in
    // the menu's Mods page (runtime/mods.c: mods/<mod>/romfs, Android). The file-meta table is patched (in a copy) to
    // point them at data appended past the end of the image; reads there go to host files.
    mods_scan();
    for (int i = 0; i < rom_nfiles; i++) {
        char host[1200];
        if (!test_boot_override(fd, &rom_files[i], host, sizeof host) &&
            !mods_override(rom_files[i].path, host, sizeof host) && !mods_romfs_override(rom_files[i].path, host, sizeof host))
            continue;
        struct stat sb;
        if (stat(host, &sb)) continue;
        if (!ov_meta) {
            ov_meta = malloc(fsz);
            memcpy(ov_meta, fmeta, fsz);
            ov_meta_off = foff;
            ov_meta_size = fsz;
            struct stat rs;
            fstat(fd, &rs);
            ov_end = (rs.st_size + 0xF) & ~0xFull;
        }
        uint64_t rel = ov_end - rom_data_off, size = (uint64_t)sb.st_size;
        memcpy(ov_meta + rom_files[i].meta + 8, &rel, 8);
        memcpy(ov_meta + rom_files[i].meta + 0x10, &size, 8);
        if (ov_n == ov_cap) { ov_cap = ov_cap ? ov_cap * 2 : 256; ov = realloc(ov, sizeof *ov * ov_cap); }
        ov[ov_n].start = ov_end;
        ov[ov_n].size = size;
        ov[ov_n].path = strdup(host);
        ov[ov_n].fd = -1;
        ov[ov_n].mem = NULL;
        ov_n++;
        ov_end = (ov_end + size + 0xF) & ~0xFull;
    }
    if (ov_n) LOG("[fs] mods: %d romfs files replaced\n", ov_n);
    if (ov_meta) { patches[npatches++] = (Patch){ov_meta_off, ov_meta_size, ov_meta}; }
    add_new_files(fd, h, dmeta, dsz, fsz);
    qsort(rom_files, rom_nfiles, sizeof *rom_files, rom_cmp);
    free(dmeta);
    free(fmeta);
    LOG("[fs] romfs index: %d files\n", rom_nfiles);
}

// Virtual RomFS image read: original bytes, with the patched file table and appended replacement files.
static ssize_t romfs_read(int fd, uint8_t *dst, uint64_t size, uint64_t off) {
    if (!ov_n) return pread(fd, dst, size, (off_t)off);
    uint64_t done = 0;
    while (done < size) {
        uint64_t o = off + done, n = size - done;
        if (o >= ov[0].start) {  // appended region
            int i = 0;
            while (i < ov_n && !(o >= ov[i].start && o < ov[i].start + ((ov[i].size + 0xF) & ~0xFull))) i++;
            if (i == ov_n) { memset(dst + done, 0, n); done += n; break; }
            uint64_t in = o - ov[i].start, avail = ((ov[i].size + 0xF) & ~0xFull) - in;
            if (n > avail) n = avail;
            ssize_t r;
            if (ov[i].mem) {  // in-memory blob (the extended file table)
                r = in < ov[i].size ? (ssize_t)(n < ov[i].size - in ? n : ov[i].size - in) : 0;
                memcpy(dst + done, ov[i].mem + in, (size_t)r);
            } else {
                if (ov[i].fd < 0) ov[i].fd = open(ov[i].path, O_RDONLY);
                r = in < ov[i].size ? pread(ov[i].fd, dst + done, n < ov[i].size - in ? n : ov[i].size - in, (off_t)in) : 0;
            }
            if (r < 0) r = 0;
            if ((uint64_t)r < n) memset(dst + done + r, 0, n - r);
        } else {
            if (o + n > ov[0].start) n = ov[0].start - o;
            ssize_t r = pread(fd, dst + done, n, (off_t)o);
            if (r < 0) r = 0;
            if ((uint64_t)r < n) memset(dst + done + r, 0, n - r);
            // overlay the patched tables (file meta, and with added files: header, dir meta, file hash)
            for (int k = 0; k < npatches; k++) {
                uint64_t a = o > patches[k].off ? o : patches[k].off;
                uint64_t b = o + n < patches[k].off + patches[k].size ? o + n : patches[k].off + patches[k].size;
                if (a < b) memcpy(dst + done + (a - o), patches[k].data + (a - patches[k].off), b - a);
            }
        }
        done += n;
    }
    return (ssize_t)done;
}

// Path of the RomFS file containing image offset `off` (NULL if none).
const char *romfs_file_at(uint64_t off) {
    int lo = 0, hi = rom_nfiles - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (off < rom_files[mid].off) hi = mid - 1;
        else if (off >= rom_files[mid].off + rom_files[mid].size) lo = mid + 1;
        else return rom_files[mid].path;
    }
    return NULL;
}

// ------------------------------------------------------------------ IStorage (raw RomFS image)
typedef struct { int fd; uint64_t size; } Storage;

static void storage(Service *s, IpcReq *req, IpcResp *resp) {
    Storage *st = s->state;
    switch (req->cmd) {
    case 0: {  // Read(offset, size) -> B buffer
        uint64_t off = IN(uint64_t, 0), size = IN(uint64_t, 8);
        IpcBuf *b = out_buf(req, 0);
        if (!b) { resp->result = MAKE_RESULT(2, 6001); break; }
        if (size > b->size) size = b->size;
        plat_write_prepare(b->addr, size);  // (pread into pages write tracking made read-only: EFAULT)
        ssize_t n = romfs_read(st->fd, (uint8_t *)(uintptr_t)b->addr, size, off);
        if (n < 0) LOG("[fs] storage read failed at %llx: %s\n", off, strerror(errno));
        {
            const char *path = romfs_file_at(off);
            static const char *last;
            if (path && path != last) {
                last = path;
                if (diag_getenv("LARECOMP_LOG_FILES")) LOG("[fs] read %s\n", path);
            }
        }
        static uint64_t reads, bytes;
        reads++;
        bytes += size;
        if ((reads & (reads - 1)) == 0) LOG("[fs] romfs reads: %llu (%llu KB)\n", reads, bytes >> 10);
        break;
    }
    case 4: OUT(uint64_t, 0) = ov_n ? ov_end : st->size; resp_size(resp, 8); break;  // GetSize
    default: LOG("[fs] IStorage cmd %u (ok)\n", req->cmd);
    }
}

static Service *open_romfs_storage(void) {
    if (romfs_fd < 0) {
        char path[1024];
        snprintf(path, sizeof path, "%s.bin", g_romfs_dir);  // extracted/romfs.bin
        romfs_fd = open(path, O_RDONLY);
        if (romfs_fd < 0) FATAL("cannot open %s (set up the game files on the start screen)\n", path);
        char why[128];
        if (!romfs_image_ok(path, why, sizeof why))  // (else the game's SDK aborts on it with an svcBreak, nothing said)
            FATAL("The game data (romfs.bin) is incomplete or damaged: %s. Set up the game files again (Settings > Files > "
                  "Set up game files), or copy romfs.bin again whole.\n", why);
        rom_index(romfs_fd);
    }
    Storage *st = calloc(1, sizeof *st);
    st->fd = romfs_fd;
    struct stat sb;
    fstat(romfs_fd, &sb);
    st->size = sb.st_size;
    return service_new("IStorage(romfs)", storage, st);
}

// ------------------------------------------------------------------ host-directory filesystem
typedef struct HostFs { char root[1024]; bool read_only; struct HostFs *next; } HostFs;
typedef struct { int fd; char path[1100]; } HostFile;
typedef struct { DIR *d; char path[1024]; int mode; } HostDir;
static HostFs *save_mounts;
static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;
static bool temporary_cleared;
RT_ON_RESET(save_mount_reset) { save_mounts = NULL; temporary_cleared = false; }

static void host_file_close(Service *s) {
    HostFile *f = s->state;
    if (f) { close(f->fd); free(f); s->state = NULL; }
}
static void host_dir_close(Service *s) {
    HostDir *d = s->state;
    if (d) { closedir(d->d); free(d); s->state = NULL; }
}
static void save_mount_close(Service *s) {
    HostFs *fs = s->state;
    if (!fs) return;
    pthread_mutex_lock(&save_lock);
    for (HostFs **p = &save_mounts; *p; p = &(*p)->next)
        if (*p == fs) { *p = fs->next; break; }
    pthread_mutex_unlock(&save_lock);
    free(fs); s->state = NULL;
}

static const char *in_path(IpcReq *req, int i) {
    if (i < req->nx) return (const char *)(uintptr_t)req->x[i].addr;
    if (i < req->na) return (const char *)(uintptr_t)req->a[i].addr;
    return "";
}

static void host_path(HostFs *fs, const char *p, char *out, size_t n) {
    snprintf(out, n, "%s%s%s", fs->root, p[0] == '/' ? "" : "/", p);
}

static uint32_t errno_result(void) {
    if (errno == ENOENT || errno == ENOTDIR) return FS_PATH_NOT_FOUND;
    if (errno == EEXIST) return FS_PATH_EXISTS;
    if (errno == ENOTEMPTY) return FS_NOT_EMPTY;
    return MAKE_RESULT(2, 4000);
}

static void file(Service *s, IpcReq *req, IpcResp *resp) {
    HostFile *f = s->state;
    switch (req->cmd) {
    case 0: {  // Read(option, offset, size) -> B
        int64_t off = IN(int64_t, 8), size = IN(int64_t, 16);
        IpcBuf *b = out_buf(req, 0);
        if ((uint64_t)size > b->size) size = (int64_t)b->size;
        plat_write_prepare(b->addr, size);
        ssize_t n = pread(f->fd, (void *)(uintptr_t)b->addr, (size_t)size, off);
        if(n>0)test_session_file_read(f->path);
        if (strstr(f->path, "savedata")) {
            LOG("[save] read %s off %lld size %lld -> guest %llx\n", f->path, (long long)off, (long long)size, (unsigned long long)b->addr);
        }
        OUT(int64_t, 0) = n < 0 ? 0 : n;
        resp_size(resp, 8);
        break;
    }
    case 1: {  // Write(option, offset, size) <- A
        int64_t off = IN(int64_t, 8), size = IN(int64_t, 16);
        IpcBuf *a = in_buf(req, 0);
        if (strstr(f->path, "savedata"))
            LOG("[save] write %s off %lld size %lld <- guest %llx\n", f->path, (long long)off, (long long)size, (unsigned long long)a->addr);
        if (pwrite(f->fd, (void *)(uintptr_t)a->addr, (size_t)size, off) < 0) resp->result = errno_result();
        break;
    }
    case 2: fsync(f->fd); break;
    case 3: if (ftruncate(f->fd, IN(int64_t, 0))) resp->result = errno_result(); break;
    case 4: {
        struct stat sb;
        fstat(f->fd, &sb);
        OUT(int64_t, 0) = sb.st_size;
        resp_size(resp, 8);
        break;
    }
    default: LOG("[fs] IFile cmd %u (ok)\n", req->cmd);
    }
}

static void directory(Service *s, IpcReq *req, IpcResp *resp) {
    HostDir *d = s->state;
    switch (req->cmd) {
    case 0: {  // Read -> B buffer of 0x310-byte entries
        IpcBuf *b = out_buf(req, 0);
        uint8_t *out = (uint8_t *)(uintptr_t)b->addr;
        int64_t max = (int64_t)(b->size / 0x310), n = 0;
        struct dirent *e;
        while (n < max && (e = readdir(d->d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
#ifdef _WIN32  // no d_type
            char full[2048];
            struct stat es;
            snprintf(full, sizeof full, "%s/%s", d->path, e->d_name);
            bool isdir = !stat(full, &es) && S_ISDIR(es.st_mode);
#else
            bool isdir = e->d_type == DT_DIR;
#endif
            if ((isdir && !(d->mode & 1)) || (!isdir && !(d->mode & 2))) continue;
            uint8_t *ent = out + n * 0x310;
            memset(ent, 0, 0x310);
            strncpy((char *)ent, e->d_name, 0x300);
            ent[0x304] = isdir ? 0 : 1;
            if (!isdir) {
                char p[2048];
                struct stat sb;
                snprintf(p, sizeof p, "%s/%s", d->path, e->d_name);
                if (!stat(p, &sb)) *(int64_t *)(ent + 0x308) = sb.st_size;
            }
            n++;
        }
        OUT(int64_t, 0) = n;
        resp_size(resp, 8);
        break;
    }
    case 1: {  // GetEntryCount
        long pos = telldir(d->d);
        rewinddir(d->d);
        int64_t n = 0;
        struct dirent *e;
        while ((e = readdir(d->d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
        seekdir(d->d, pos);
        OUT(int64_t, 0) = n;
        resp_size(resp, 8);
        break;
    }
    }
}

static int rm_rf(const char *p, bool keep_root) {
    struct stat sb;
    if (lstat(p, &sb)) return -1;
    if (!S_ISDIR(sb.st_mode)) return unlink(p);
    DIR *d = opendir(p);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[2048];
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        if (rm_rf(c, false)) { int error = errno; closedir(d); errno = error; return -1; }
    }
    closedir(d);
    return keep_root ? 0 : rmdir(p);
}

static void filesystem(Service *s, IpcReq *req, IpcResp *resp) {
    HostFs *fs = s->state;
    char p[2048], p2[2048];
    // Guest paths must stay within the mounted filesystem, including cache metadata isolation.
    for (int i = 0; i < 2; i++) {
        const char *path = in_path(req, i);
        for (const char *q = path; *q;) {
            while (*q == '/') q++;
            const char *part = q;
            while (*q && *q != '/') q++;
            if (q - part == 2 && !memcmp(part, "..", 2)) { resp->result = FS_INVALID_ARGUMENT; return; }
        }
    }
    if (fs->read_only && (req->cmd <= 6 || req->cmd == 13 ||
                         (req->cmd == 8 && (IN(uint32_t, 0) & 2)))) {
        resp->result = FS_UNSUPPORTED;
        return;
    }
    host_path(fs, in_path(req, 0), p, sizeof p);
    switch (req->cmd) {
    case 0: {  // CreateFile(option, size)
        int fd = open(p, O_CREAT | O_EXCL | O_RDWR, 0644);
        if (fd < 0) { resp->result = errno_result(); break; }
        if (ftruncate(fd, IN(int64_t, 8))) resp->result = errno_result();
        close(fd);
        break;
    }
    case 1: if (unlink(p)) resp->result = errno_result(); break;
    case 2: if (mkdir(p, 0755)) resp->result = errno_result(); break;
    case 3: if (rmdir(p)) resp->result = errno_result(); break;
    case 4: if (rm_rf(p, false)) resp->result = errno_result(); break;
    case 5: case 6:
        host_path(fs, in_path(req, 1), p2, sizeof p2);
        if (rename(p, p2)) resp->result = errno_result();
        break;
    case 7: {  // GetEntryType
        struct stat sb;
        if (stat(p, &sb)) { resp->result = FS_PATH_NOT_FOUND; break; }
        OUT(uint32_t, 0) = S_ISDIR(sb.st_mode) ? 0 : 1;
        resp_size(resp, 4);
        break;
    }
    case 8: {  // OpenFile(mode)
        uint32_t mode = IN(uint32_t, 0);
        int fd = open(p, (mode & 2) ? O_RDWR : O_RDONLY);
        if (fd < 0) { resp->result = errno_result(); break; }
        HostFile *f = calloc(1, sizeof *f);
        f->fd = fd;
        strlcpy(f->path, p, sizeof f->path);
        Service *opened = service_new("IFile", file, f);
        opened->on_close = host_file_close;
        ipc_return_object(resp, opened);
        break;
    }
    case 9: {  // OpenDirectory(mode)
        DIR *d = opendir(p);
        if (!d) { resp->result = errno_result(); break; }
        HostDir *hd = calloc(1, sizeof *hd);
        hd->d = d;
        hd->mode = (int)IN(uint32_t, 0);
        snprintf(hd->path, sizeof hd->path, "%s", p);
        Service *opened = service_new("IDirectory", directory, hd);
        opened->on_close = host_dir_close;
        ipc_return_object(resp, opened);
        break;
    }
    case 10: break;  // Commit
    case 11: case 12: OUT(int64_t, 0) = 0x40000000; resp_size(resp, 8); break;
    case 13: rm_rf(p, true); break;
    case 14: memset(resp->data, 0, 0x20); resp_size(resp, 0x20); break;
    default: LOG("[fs] IFileSystem cmd %u (ok)\n", req->cmd);
    }
    // (failures logged: newer SDKs abort on a result they don't expect - v1.6.0's SDK 7.3.2 in its save thread)
    if (resp->result) LOG("[fs] IFileSystem cmd %u %s: result %#x\n", req->cmd, p, resp->result);
}

// A copy of folder src at dst (made by this process, so its files are this app's: on Android, files adb copied belong
// to the shell user, and the game couldn't open them for writing - result 2/4000)
static void copy_tree(const char *src, const char *dst) {
    DIR *d = opendir(src);
    if (!d) return;
    mkdir(dst, 0755);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[2048], b[2048];
        snprintf(a, sizeof a, "%s/%s", src, e->d_name);
        snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
        struct stat sb;
        if (stat(a, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) { copy_tree(a, b); continue; }
        int in = open(a, O_RDONLY), out = in < 0 ? -1 : open(b, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        char buf[65536];
        ssize_t n;
        while (out >= 0 && (n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) break;
        if (in >= 0) close(in);
        if (out >= 0) close(out);
    }
    closedir(d);
}

static Service *open_host_fs(const char *sub) {
    HostFs *fs = calloc(1, sizeof *fs);
    if (getenv("LARECOMP_SAVE_DIR")) g_save_dir = getenv("LARECOMP_SAVE_DIR");  // test runs use a scratch copy
    snprintf(fs->root, sizeof fs->root, "%s/%s", g_save_dir, sub);
    {   // LARECOMP_SAVE_SEED=<save folder>: a save that doesn't exist yet starts as a copy of that one's (another game
        // version's data root starting from the player's saves - android/run_v160.sh)
        const char *seed = getenv("LARECOMP_SAVE_SEED");
        struct stat sb;
        if (seed && *seed && stat(fs->root, &sb)) {
            char from[1200];
            snprintf(from, sizeof from, "%s/%s", seed, sub);
            mkdir(g_save_dir, 0755);
            copy_tree(from, fs->root);
            LOG("[fs] save %s started as a copy of %s\n", fs->root, from);
        }
    }
    {   // LARECOMP_SAVE_REPLACE=<save folder>: this save becomes a copy of that one's, the one it was kept beside it as
        // <save>.before-replace[-n] (a test save made the player's own: adb can't write the app's folders on Android)
        // (once a run: the game opens its save again and again)
        static bool replaced;
        const char *from_dir = getenv("LARECOMP_SAVE_REPLACE");
        char from[1200], keep[1300];
        struct stat sb;
        snprintf(from, sizeof from, "%s/%s", from_dir ? from_dir : "", sub);
        if (from_dir && *from_dir && !replaced && !stat(from, &sb) && S_ISDIR(sb.st_mode)) {
            replaced = true;
            snprintf(keep, sizeof keep, "%s.before-replace", fs->root);
            for (int n = 2; !stat(keep, &sb) && n < 100; n++) snprintf(keep, sizeof keep, "%s.before-replace-%d", fs->root, n);
            if (stat(fs->root, &sb) || !rename(fs->root, keep)) {
                copy_tree(from, fs->root);
                LOG("[fs] save %s replaced by a copy of %s (the one before: %s)\n", fs->root, from, keep);
            } else {
                LOG("[fs] save %s not replaced: can't move it aside (errno %d)\n", fs->root, errno);
            }
        }
    }
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "%s", fs->root);
    for (char *q = cmd + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(cmd, 0755); *q = '/'; }
    mkdir(cmd, 0755);
    LOG("[fs] mounted host directory %s\n", fs->root);
    return service_new("IFileSystem", filesystem, fs);
}

// SaveDataAttribute's type is at +0x20 (not the system-save id at +0x18).
// Metadata lives beside files/, outside the filesystem visible to the guest.
typedef struct {
    uint64_t app, uid[2], system;
    uint8_t type, rank;
    uint16_t index;
    uint8_t reserved[0x1c];
} SaveAttribute;
typedef struct {
    char magic[8];
    SaveAttribute attr;
    int64_t size, journal;
    uint8_t space, reserved[7];
} SaveMeta;
enum { SAVE_ACCOUNT = 1, SAVE_DEVICE = 3, SAVE_TEMPORARY = 4, SAVE_CACHE = 5 };
_Static_assert(sizeof(SaveAttribute) == 0x40, "save attribute ABI");
_Static_assert(offsetof(SaveAttribute, type) == 0x20, "save type offset");
_Static_assert(sizeof(SaveMeta) == 0x60, "host metadata format");
typedef struct { uint8_t bytes[0x60]; } SaveInfo;
typedef struct { SaveInfo *entries; size_t count, cursor; } SaveReader;

static void save_root(char *out, size_t cap) {
    const char *base = getenv("LARECOMP_SAVE_DIR");
    snprintf(out, cap, "%s/stores", base && *base ? base : g_save_dir);
}
static bool save_attribute(SaveAttribute *a, const void *data) {
    memcpy(a, data, sizeof *a);
    memset(a->reserved, 0, sizeof a->reserved);
    if (!a->app) a->app = TOTK_TITLE_ID;
    return a->rank <= 1 &&
           (a->type == SAVE_ACCOUNT || a->type == SAVE_DEVICE || a->type == SAVE_TEMPORARY || a->type == SAVE_CACHE);
}
static void save_key(char *out, size_t cap, const SaveMeta *m) {
    const SaveAttribute *a = &m->attr;
    snprintf(out, cap, "%u_%u_%u_%u_%016llx_%016llx%016llx_%016llx", m->space,
             a->type, a->rank, a->index, (unsigned long long)a->app,
             (unsigned long long)a->uid[1], (unsigned long long)a->uid[0], (unsigned long long)a->system);
}
static void save_path(char *out, size_t cap, const SaveMeta *m) {
    char root[1100], key[128];
    save_root(root, sizeof root); save_key(key, sizeof key, m);
    snprintf(out, cap, "%s/%s", root, key);
}
static uint32_t save_load(SaveMeta *m) {
    char path[1400], base[1300]; save_path(base, sizeof base, m);
    snprintf(path, sizeof path, "%s/metadata", base);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno_result();
    SaveMeta loaded;
    ssize_t n = read(fd, &loaded, sizeof loaded);
    close(fd);
    if (n != sizeof loaded || memcmp(loaded.magic, "TKSTORE1", 8) ||
        memcmp(&loaded.attr, &m->attr, sizeof m->attr) || loaded.space != m->space ||
        loaded.size < 0 || loaded.journal < 0) return MAKE_RESULT(2, 4000);
    *m = loaded;
    return 0;
}
static int save_mkdirs(const char *path) {
    char copy[1400];
    if (strlcpy(copy, path, sizeof copy) >= sizeof copy) { errno = ENAMETOOLONG; return -1; }
    for (char *q = copy + 1; ; q++) {
        if (*q && *q != '/') continue;
        char c = *q; *q = 0;
        if (mkdir(copy, 0755) && errno != EEXIST) return -1;
        *q = c;
        if (!c) return 0;
    }
}
static uint32_t save_create(SaveMeta *m) {
    char root[1100], base[1300], stage[1400], path[1500];
    save_root(root, sizeof root); save_path(base, sizeof base, m);
    if (m->size < 0 || m->journal < 0) return MAKE_RESULT(2, 6062);
    if (save_mkdirs(root)) return errno_result();
    struct stat sb;
    if (!lstat(base, &sb)) return FS_PATH_EXISTS;
    if (errno != ENOENT) return errno_result();
    struct statvfs space;
    if (statvfs(root, &space)) return errno_result();
    uint64_t total = (uint64_t)m->size + (uint64_t)m->journal;
    if (total > (uint64_t)space.f_bavail * space.f_frsize) return MAKE_RESULT(2, 30);
    snprintf(stage, sizeof stage, "%s/.creating-XXXXXX", root);
    if (!mkdtemp(stage)) return errno_result();
    snprintf(path, sizeof path, "%s/files", stage);
    uint32_t result = 0;
    if (mkdir(path, 0755)) result = errno_result();
    memcpy(m->magic, "TKSTORE1", 8);
    snprintf(path, sizeof path, "%s/metadata", stage);
    int fd = result ? -1 : open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (!result && fd < 0) result = errno_result();
    if (fd >= 0) {
        if (write(fd, m, sizeof *m) != sizeof *m || fsync(fd)) result = MAKE_RESULT(2, 4000);
        close(fd);
    }
    if (!result && rename(stage, base)) result = errno_result();
    if (result) rm_rf(stage, false);
    else LOG("[fs] created storage type=%u index=%u size=%lld journal=%lld\n",
             m->attr.type, m->attr.index, (long long)m->size, (long long)m->journal);
    return result;
}
uint32_t fs_create_cache_storage(uint16_t index, int64_t size, int64_t journal) {
    SaveMeta m = {.attr = {.app = TOTK_TITLE_ID, .type = SAVE_CACHE, .index = index},
                  .space = 1, .size = size, .journal = journal};
    pthread_mutex_lock(&save_lock);
    uint32_t result = save_create(&m);
    pthread_mutex_unlock(&save_lock);
    return result;
}
static uint64_t save_id(const SaveMeta *m) {
    // Stable host id; the complete attribute, not this hash, selects the storage path.
    uint64_t id = 14695981039346656037ull;
    const uint8_t *p = (const uint8_t *)&m->attr;
    for (size_t i = 0; i < sizeof m->attr; i++) id = (id ^ p[i]) * 1099511628211ull;
    return (id ^ m->space) * 1099511628211ull;
}
static SaveInfo save_info(const SaveMeta *m) {
    SaveInfo info = {0};
    uint64_t id = save_id(m), size = (uint64_t)m->size + (uint64_t)m->journal;
    memcpy(info.bytes, &id, 8);
    info.bytes[8] = m->space; info.bytes[9] = m->attr.type;
    memcpy(info.bytes + 0x10, m->attr.uid, 16);
    memcpy(info.bytes + 0x20, &m->attr.system, 8);
    memcpy(info.bytes + 0x28, &m->attr.app, 8);
    memcpy(info.bytes + 0x30, &size, 8);
    memcpy(info.bytes + 0x38, &m->attr.index, 2);
    info.bytes[0x3a] = m->attr.rank;
    return info;
}
static void save_reader_close(Service *s) {
    SaveReader *r = s->state;
    if (r) { free(r->entries); free(r); s->state = NULL; }
}
static void save_reader(Service *s, IpcReq *req, IpcResp *resp) {
    if (req->cmd) { resp->result = FS_UNSUPPORTED; return; }
    SaveReader *r = s->state;
    IpcBuf *b = out_buf(req, 0);
    if (!b || (b->size && !b->addr)) { resp->result = FS_INVALID_ARGUMENT; return; }
    pthread_mutex_lock(&save_lock);
    size_t n = b->size / sizeof(SaveInfo);
    if (n > r->count - r->cursor) n = r->count - r->cursor;
    if (n) {
        plat_write_prepare(b->addr, n * sizeof(SaveInfo));
        memcpy((void *)(uintptr_t)b->addr, r->entries + r->cursor, n * sizeof(SaveInfo));
        r->cursor += n;
    }
    OUT(int64_t, 0) = (int64_t)n; resp_size(resp, 8);
    pthread_mutex_unlock(&save_lock);
}
static int save_info_compare(const void *a, const void *b) {
    uint16_t x, y;
    memcpy(&x, ((const SaveInfo *)a)->bytes + 0x38, 2);
    memcpy(&y, ((const SaveInfo *)b)->bytes + 0x38, 2);
    return x < y ? -1 : x > y;
}
static uint32_t save_open_reader(IpcResp *resp) {
    char root[1100]; save_root(root, sizeof root);
    DIR *dir = opendir(root);
    if (!dir && errno != ENOENT) return errno_result();
    SaveReader *r = calloc(1, sizeof *r);
    if (!r) { if (dir) closedir(dir); return MAKE_RESULT(2, 4000); }
    struct dirent *e;
    uint32_t result = 0;
    while (dir && (e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        char path[1500]; snprintf(path, sizeof path, "%s/%s/metadata", root, e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) { result = errno_result(); break; }
        SaveMeta m;
        ssize_t n = read(fd, &m, sizeof m); close(fd);
        if (n != sizeof m || memcmp(m.magic, "TKSTORE1", 8)) { result = MAKE_RESULT(2, 4000); break; }
        if (m.attr.type != SAVE_CACHE || m.attr.app != TOTK_TITLE_ID) continue;
        SaveMeta checked = m;
        if ((result = save_load(&checked))) break;
        SaveInfo *entries = realloc(r->entries, (r->count + 1) * sizeof *entries);
        if (!entries) { result = MAKE_RESULT(2, 4000); break; }
        r->entries = entries; r->entries[r->count++] = save_info(&m);
    }
    if (dir) closedir(dir);
    if (result) { free(r->entries); free(r); return result; }
    if (r->count > 1) qsort(r->entries, r->count, sizeof *r->entries, save_info_compare);
    Service *reader = service_new("ISaveDataInfoReader", save_reader, r);
    reader->on_close = save_reader_close;
    ipc_return_object(resp, reader);
    LOG("[fs] opened cache storage list: %zu entries\n", r->count);
    return 0;
}
static bool fsp_save(IpcReq *req, IpcResp *resp) {
    bool create = req->cmd == 22, mount = req->cmd == 51 || req->cmd == 53;
    bool cache = req->cmd == 33 || req->cmd == 34;
    if (!create && !mount && !cache && req->cmd != 62) return false;
    SaveMeta m = {0};
    size_t needed = create ? 0x90 : mount ? 0x48 : cache ? 2 : 0;
    if (req->data_size < needed) { resp->result = FS_INVALID_ARGUMENT; return true; }
    if (create || mount) {
        if (!save_attribute(&m.attr, req->data + (mount ? 8 : 0))) {
            LOG("[fs] unsupported save attribute cmd=%u app=%016llx type=%u rank=%u index=%u\n",
                req->cmd, (unsigned long long)m.attr.app, m.attr.type, m.attr.rank, m.attr.index);
            resp->result = FS_UNSUPPORTED; return true;
        }
        // TotK probes BOTW's saves for horse transfer. No other application's save was imported.
        if (m.attr.app != TOTK_TITLE_ID) {
            resp->result = mount ? FS_TARGET_NOT_FOUND : FS_UNSUPPORTED;
            LOG("[fs] other application save %016llx is unavailable\n", (unsigned long long)m.attr.app);
            return true;
        }
        m.space = IN(uint8_t, mount ? 0 : 0x64);
        if (create) { m.size = IN(int64_t, 0x40); m.journal = IN(int64_t, 0x48); }
    } else if (cache) {
        m.attr.app = TOTK_TITLE_ID; m.attr.type = SAVE_CACHE;
        m.attr.index = IN(uint16_t, 0); m.space = 1;
    }
    pthread_mutex_lock(&save_lock);
    if (req->cmd == 62) resp->result = save_open_reader(resp);
    else if (create) resp->result = save_create(&m);
    else {
        resp->result = save_load(&m);
        // Account/device/temporary storage is provisioned on first use by this standalone app.
        // Cache storage must be explicitly created, or the SDK's cache initialization is skipped.
        if (mount && req->cmd == 51 && m.attr.type != SAVE_CACHE && resp->result == FS_PATH_NOT_FOUND)
            resp->result = save_create(&m);
        if (!resp->result && mount && m.attr.type == SAVE_TEMPORARY && !temporary_cleared) {
            char base[1300], files[1400]; save_path(base, sizeof base, &m);
            snprintf(files, sizeof files, "%s/files", base);
            if (rm_rf(files, true)) resp->result = errno_result();
            else temporary_cleared = true;
        }
        if (!resp->result && mount) {
            char key[128], sub[160]; save_key(key, sizeof key, &m);
            snprintf(sub, sizeof sub, "stores/%s/files", key);
            Service *fs = open_host_fs(sub);
            ((HostFs *)fs->state)->read_only = req->cmd == 53;
            ((HostFs *)fs->state)->next = save_mounts;
            save_mounts = fs->state;
            fs->on_close = save_mount_close;
            ipc_return_object(resp, fs);
        } else if (!resp->result && req->cmd == 34) {
            OUT(int64_t, 0) = m.size; OUT(int64_t, 8) = m.journal; resp_size(resp, 16);
        } else if (!resp->result && req->cmd == 33) {
            char path[1300]; save_path(path, sizeof path, &m);
            char files[1400]; snprintf(files, sizeof files, "%s/files", path);
            for (HostFs *fs = save_mounts; fs; fs = fs->next)
                if (!strcmp(fs->root, files)) { resp->result = FS_TARGET_LOCKED; break; }
            if (!resp->result && rm_rf(path, false)) resp->result = errno_result();
        }
    }
    pthread_mutex_unlock(&save_lock);
    if (mount && resp->result == FS_PATH_NOT_FOUND) resp->result = FS_TARGET_NOT_FOUND;
    if (resp->result) LOG("[fs] save cmd=%u type=%u index=%u result=%#x\n",
                          req->cmd, m.attr.type, m.attr.index, resp->result);
    return true;
}

// ------------------------------------------------------------------ fsp-srv
SERVICE("fsp-srv", fsp_srv) {
    (void)s;
    if (fsp_save(req, resp)) return;
    switch (req->cmd) {
    case 1: break;  // SetCurrentProcess
    case 18: ipc_return_object(resp, open_host_fs("sdcard")); break;
    case 200: ipc_return_object(resp, open_romfs_storage()); break;  // OpenDataStorageByCurrentProcess
    case 202:  // OpenDataStorageByDataId (system data archives)
        LOG("[fs] OpenDataStorageByDataId %llx (not available)\n", IN(uint64_t, 8));
        resp->result = MAKE_RESULT(2, 1001);
        break;
    case 203:  // OpenPatchDataStorageByCurrentProcess: no update installed (BOTW asks at boot) - TargetNotFound
        resp->result = MAKE_RESULT(2, 1002);
        break;
    case 1005: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // GetGlobalAccessLogMode
    case 1003: case 1004: case 1006: break;
    case 1011: memset(resp->data, 0, 0x10); resp_size(resp, 0x10); break;
    default: LOG("[fs] fsp-srv cmd %u (ok)\n", req->cmd);
    }
}
