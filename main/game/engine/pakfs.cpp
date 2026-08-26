#include "pakfs.hpp"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <strings.h>   // strcasecmp/strncasecmp (paths compare like FAT: case-insensitive)
#include <cstdio>
#include <cstdlib>

static const char* TAG = "PAKFS";

// --- on-disk format ---------------------------------------------------------------
// Written by tools/modpack.py; spec in docs/modpacks.md. The TOC entry is mirrored 1:1
// in RAM so the whole table loads with a single fread and no per-entry fixup.

static const char     PAK_MAGIC[8]  = { 'V','P','E','T','P','A','K','1' };
static const uint32_t PAK_VERSION   = 1;
static const int      PAK_PATH_MAX  = 104;     // incl. NUL; matches the tool
// Sanity cap, not a design limit: 16k entries is a ~1.9 MB TOC, far beyond any real mod
// library, so anything claiming more is a corrupt or hostile header.
static const uint32_t PAK_MAX_ENTRIES = 16384;

struct PakHeader {
    char     magic[8];
    uint32_t version;
    uint32_t count;
    uint32_t entrySize;    // sizeof(PakEntry); lets the format grow without lying headers
    uint32_t reserved;
};
static_assert(sizeof(PakHeader) == 24, "pak header layout drifted from the tool");

struct PakEntry {
    char     path[PAK_PATH_MAX];   // '/'-separated, no leading slash, NUL-terminated
    uint32_t offset;               // absolute offset of the blob within the .pak
    uint32_t size;
    uint32_t crc32;                // checked by the tool at pack/verify time, not here
};
static_assert(sizeof(PakEntry) == 116, "pak TOC entry layout drifted from the tool");

// --- mounted-pack state -------------------------------------------------------------

// Per-pack open-file slots. The game task opens at most a couple of files at once (one
// scan read + maybe a sprite decode), so a small fixed table is plenty.
static const int PAK_MAX_FDS = 4;

struct PakFd {
    int32_t  toc;    // TOC index of the open entry
    uint32_t pos;    // read cursor within the entry
    bool     used;
};

struct Pak {
    char      mount[8];      // "/pak0"
    char      src[192];      // underlying .pak path, for logs (fits mount_all's path buf)
    FILE*     f;             // the archive, held open for the whole session
    PakEntry* toc;           // PSRAM; sorted case-insensitively by path
    int       count;
    SemaphoreHandle_t mtx;   // guards f (shared seek position) and the fd table
    PakFd     fds[PAK_MAX_FDS];
};

static Pak s_paks[PAKFS_MAX_PAKS];
static int s_count = 0;

// --- lookups --------------------------------------------------------------------------

static const char* strip_slash(const char* p)
{
    while (*p == '/') p++;
    return p;
}

// First TOC index whose path sorts >= key (the TOC's order IS strcasecmp order; the
// mount-time validation rejects a pak where it isn't, so this stays a true lower bound).
static int lower_bound_(const Pak& p, const char* key)
{
    int lo = 0, hi = p.count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (strcasecmp(p.toc[mid].path, key) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static int find_file(const Pak& p, const char* rel)
{
    int i = lower_bound_(p, rel);
    return (i < p.count && strcasecmp(p.toc[i].path, rel) == 0) ? i : -1;
}

// Directories are implicit (the TOC stores only files): "creatures/" exists iff some
// entry starts with it. Case-folded extensions of a prefix sort contiguously right at
// its lower bound, so one probe decides.
static bool dir_exists(const Pak& p, const char* relSlash)
{
    if (!relSlash[0]) return true;   // the pak root
    int i = lower_bound_(p, relSlash);
    return i < p.count && strncasecmp(p.toc[i].path, relSlash, strlen(relSlash)) == 0;
}

// --- file ops ---------------------------------------------------------------------------

static int pak_open(void* ctx, const char* path, int flags, int)
{
    Pak& p = *(Pak*)ctx;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) {
        errno = EROFS;
        return -1;
    }
    int t = find_file(p, strip_slash(path));
    if (t < 0) { errno = ENOENT; return -1; }

    xSemaphoreTake(p.mtx, portMAX_DELAY);
    int fd = -1;
    for (int i = 0; i < PAK_MAX_FDS; i++)
        if (!p.fds[i].used) { p.fds[i] = { t, 0, true }; fd = i; break; }
    xSemaphoreGive(p.mtx);
    if (fd < 0) errno = ENFILE;
    return fd;
}

static int pak_close(void* ctx, int fd)
{
    Pak& p = *(Pak*)ctx;
    if (fd < 0 || fd >= PAK_MAX_FDS || !p.fds[fd].used) { errno = EBADF; return -1; }
    p.fds[fd].used = false;
    return 0;
}

// Core read: `pos` is caller-owned so read (advancing) and pread (not) share it.
static ssize_t read_at(Pak& p, const PakEntry& e, uint32_t pos, void* dst, size_t size)
{
    if (pos >= e.size || size == 0) return 0;
    size_t n = e.size - pos;
    if (n > size) n = size;

    xSemaphoreTake(p.mtx, portMAX_DELAY);
    ssize_t got = -1;
    if (fseek(p.f, (long)(e.offset + pos), SEEK_SET) == 0)
        got = (ssize_t)fread(dst, 1, n, p.f);
    xSemaphoreGive(p.mtx);

    if (got < 0) errno = EIO;
    return got;
}

static ssize_t pak_read(void* ctx, int fd, void* dst, size_t size)
{
    Pak& p = *(Pak*)ctx;
    if (fd < 0 || fd >= PAK_MAX_FDS || !p.fds[fd].used) { errno = EBADF; return -1; }
    PakFd& h = p.fds[fd];
    ssize_t got = read_at(p, p.toc[h.toc], h.pos, dst, size);
    if (got > 0) h.pos += (uint32_t)got;
    return got;
}

static ssize_t pak_pread(void* ctx, int fd, void* dst, size_t size, off_t offset)
{
    Pak& p = *(Pak*)ctx;
    if (fd < 0 || fd >= PAK_MAX_FDS || !p.fds[fd].used) { errno = EBADF; return -1; }
    if (offset < 0) { errno = EINVAL; return -1; }
    return read_at(p, p.toc[p.fds[fd].toc], (uint32_t)offset, dst, size);
}

static off_t pak_lseek(void* ctx, int fd, off_t off, int mode)
{
    Pak& p = *(Pak*)ctx;
    if (fd < 0 || fd >= PAK_MAX_FDS || !p.fds[fd].used) { errno = EBADF; return -1; }
    const PakEntry& e = p.toc[p.fds[fd].toc];
    int64_t base;
    switch (mode) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = p.fds[fd].pos; break;
        case SEEK_END: base = e.size; break;
        default: errno = EINVAL; return -1;
    }
    int64_t np = base + off;
    if (np < 0 || np > 0xFFFFFFFFll) { errno = EINVAL; return -1; }
    p.fds[fd].pos = (uint32_t)np;   // may sit past EOF (POSIX); reads there return 0
    return (off_t)np;
}

static int pak_fstat(void* ctx, int fd, struct stat* st)
{
    Pak& p = *(Pak*)ctx;
    if (fd < 0 || fd >= PAK_MAX_FDS || !p.fds[fd].used) { errno = EBADF; return -1; }
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFREG | 0444;
    st->st_size = p.toc[p.fds[fd].toc].size;
    return 0;
}

static ssize_t pak_write(void*, int, const void*, size_t)
{
    errno = EROFS;
    return -1;
}

// --- dir ops ------------------------------------------------------------------------------

static int pak_stat(void* ctx, const char* path, struct stat* st)
{
    Pak& p = *(Pak*)ctx;
    const char* rel = strip_slash(path);
    memset(st, 0, sizeof *st);
    if (!rel[0]) { st->st_mode = S_IFDIR | 0555; return 0; }

    int t = find_file(p, rel);
    if (t >= 0) {
        st->st_mode = S_IFREG | 0444;
        st->st_size = p.toc[t].size;
        return 0;
    }
    char d[PAK_PATH_MAX + 1];
    size_t n = strlen(rel);
    if (n + 2 <= sizeof d) {
        memcpy(d, rel, n);
        d[n] = '/';
        d[n + 1] = '\0';
        if (dir_exists(p, d)) { st->st_mode = S_IFDIR | 0555; return 0; }
    }
    errno = ENOENT;
    return -1;
}

struct PakDir {
    DIR     base;                   // the VFS layer stamps dd_vfs_idx in here
    int     cursor;                 // next TOC index to consider
    size_t  plen;                   // strlen(prefix)
    char    prefix[PAK_PATH_MAX];   // "" (root) or "creatures/agumon/" (trailing slash)
    struct dirent ent;              // storage for the non-reentrant readdir
};

static DIR* pak_opendir(void* ctx, const char* name)
{
    Pak& p = *(Pak*)ctx;
    const char* rel = strip_slash(name);
    size_t n = strlen(rel);
    while (n && rel[n - 1] == '/') n--;                 // tolerate a trailing slash
    if (n + 2 > (size_t)PAK_PATH_MAX) { errno = ENAMETOOLONG; return nullptr; }

    PakDir* d = (PakDir*)calloc(1, sizeof(PakDir));
    if (!d) { errno = ENOMEM; return nullptr; }
    memcpy(d->prefix, rel, n);
    if (n) d->prefix[n++] = '/';
    d->prefix[n] = '\0';
    d->plen = n;

    if (!dir_exists(p, d->prefix)) { free(d); errno = ENOENT; return nullptr; }
    d->cursor = n ? lower_bound_(p, d->prefix) : 0;
    return (DIR*)d;
}

static int pak_readdir_r(void* ctx, DIR* pdir, struct dirent* ent, struct dirent** out)
{
    Pak& p = *(Pak*)ctx;
    PakDir* d = (PakDir*)pdir;
    *out = nullptr;

    while (d->cursor < p.count) {
        const char* path = p.toc[d->cursor].path;
        if (strncasecmp(path, d->prefix, d->plen) != 0) break;   // left the subtree
        const char* seg = path + d->plen;
        size_t sl = strcspn(seg, "/");

        // A subdirectory spans a contiguous run of sorted entries; emit its name on the
        // run's first row only. Comparing against the PREVIOUS row (instead of caching
        // "last emitted") keeps this stateless, so telldir/seekdir stay trivially correct.
        if (d->cursor > 0) {
            const char* pv = p.toc[d->cursor - 1].path;
            if (strncasecmp(pv, d->prefix, d->plen) == 0) {
                const char* ps = pv + d->plen;
                if (strcspn(ps, "/") == sl && strncasecmp(ps, seg, sl) == 0) {
                    d->cursor++;
                    continue;
                }
            }
        }

        memset(ent, 0, sizeof *ent);
        ent->d_ino  = d->cursor;
        ent->d_type = (seg[sl] == '/') ? DT_DIR : DT_REG;
        size_t cp = (sl < sizeof(ent->d_name) - 1) ? sl : sizeof(ent->d_name) - 1;
        memcpy(ent->d_name, seg, cp);
        ent->d_name[cp] = '\0';
        d->cursor++;
        *out = ent;
        return 0;
    }
    return 0;   // end of directory: *out stays null
}

static struct dirent* pak_readdir(void* ctx, DIR* pdir)
{
    PakDir* d = (PakDir*)pdir;
    struct dirent* out = nullptr;
    pak_readdir_r(ctx, pdir, &d->ent, &out);
    return out;
}

static long pak_telldir(void*, DIR* pdir) { return ((PakDir*)pdir)->cursor; }
static void pak_seekdir(void*, DIR* pdir, long off) { ((PakDir*)pdir)->cursor = (int)off; }
static int  pak_closedir(void*, DIR* pdir) { free(pdir); return 0; }

static int pak_access(void* ctx, const char* path, int amode)
{
    if (amode & W_OK) { errno = EACCES; return -1; }
    struct stat st;
    return pak_stat(ctx, path, &st);
}

// --- registration ---------------------------------------------------------------------------

// The ops tables name only the ops a read-only FS has (the VFS layer null-checks the
// rest), but which members esp_vfs_fs_ops_t even HAS depends on kconfig (termios/select),
// so "initialize everything" isn't portable -- silence the aggregate warning instead.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

static const esp_vfs_dir_ops_t s_dir_ops = {
    .stat_p      = pak_stat,
    .opendir_p   = pak_opendir,
    .readdir_p   = pak_readdir,
    .readdir_r_p = pak_readdir_r,
    .telldir_p   = pak_telldir,
    .seekdir_p   = pak_seekdir,
    .closedir_p  = pak_closedir,
    .access_p    = pak_access,
};

static const esp_vfs_fs_ops_t s_fs_ops = {
    .write_p = pak_write,   // explicit EROFS beats the layer's generic ENOSYS
    .lseek_p = pak_lseek,
    .read_p  = pak_read,
    .pread_p = pak_pread,
    .open_p  = pak_open,
    .close_p = pak_close,
    .fstat_p = pak_fstat,
    .dir     = &s_dir_ops,
};

#pragma GCC diagnostic pop

static bool mount_one(const char* srcPath)
{
    // Idempotent per source. The deep-sleep timer-wake path mounts the packs headlessly to
    // read the roster, and when a wake trigger fires it falls through to App::init, which
    // mounts the same two sources again. Without this, base.pak would come back as a SECOND,
    // stronger /pakN -- inverting the override order it is mounted first to establish -- and
    // burn a slot out of PAKFS_MAX_PAKS while it was at it.
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_paks[i].src, srcPath) == 0) {
            ESP_LOGD(TAG, "%s already mounted at %s", srcPath, s_paks[i].mount);
            return false;
        }

    // The one choke point that writes s_paks[s_count], so the bound is enforced here rather
    // than trusted to callers -- pakfs_mount_all is invoked once per pack source per boot.
    if (s_count >= PAKFS_MAX_PAKS) {
        ESP_LOGW(TAG, "pak limit (%d) reached; not mounting %s", PAKFS_MAX_PAKS, srcPath);
        return false;
    }
    const int64_t t0 = esp_timer_get_time();
    FILE* f = fopen(srcPath, "rb");
    if (!f) { ESP_LOGW(TAG, "can't open %s", srcPath); return false; }

    PakHeader h;
    if (fread(&h, 1, sizeof h, f) != sizeof h || memcmp(h.magic, PAK_MAGIC, 8) != 0) {
        ESP_LOGW(TAG, "%s: not a mod pack (bad magic); skipped", srcPath);
        fclose(f);
        return false;
    }
    if (h.version != PAK_VERSION || h.entrySize != sizeof(PakEntry) ||
        h.count == 0 || h.count > PAK_MAX_ENTRIES) {
        ESP_LOGW(TAG, "%s: unsupported pak (version %u, entry %u, count %u); skipped",
                 srcPath, (unsigned)h.version, (unsigned)h.entrySize, (unsigned)h.count);
        fclose(f);
        return false;
    }

    fseek(f, 0, SEEK_END);
    const long fileSize = ftell(f);
    const size_t tocBytes = (size_t)h.count * sizeof(PakEntry);
    if (fileSize < (long)(sizeof h + tocBytes)) {
        ESP_LOGW(TAG, "%s: truncated (no room for the TOC); skipped", srcPath);
        fclose(f);
        return false;
    }

    PakEntry* toc = (PakEntry*)heap_caps_malloc(tocBytes, MALLOC_CAP_SPIRAM);
    if (!toc) toc = (PakEntry*)malloc(tocBytes);
    if (!toc) {
        ESP_LOGW(TAG, "%s: TOC alloc (%u bytes) failed; skipped", srcPath, (unsigned)tocBytes);
        fclose(f);
        return false;
    }
    fseek(f, sizeof h, SEEK_SET);
    if (fread(toc, 1, tocBytes, f) != tocBytes) {
        ESP_LOGW(TAG, "%s: TOC read failed; skipped", srcPath);
        free(toc);
        fclose(f);
        return false;
    }

    // Validate once at mount so every later lookup can trust the table blindly: paths are
    // relative and NUL-terminated, blobs live inside the file, and the order is STRICTLY
    // ascending case-insensitively -- which is both what makes the binary search valid and
    // a guarantee that no two entries collide the way FAT would consider equal.
    const uint32_t dataStart = sizeof h + (uint32_t)tocBytes;
    for (uint32_t i = 0; i < h.count; i++) {
        PakEntry& e = toc[i];
        e.path[PAK_PATH_MAX - 1] = '\0';
        if (!e.path[0] || e.path[0] == '/' ||
            e.offset < dataStart || (uint64_t)e.offset + e.size > (uint64_t)fileSize ||
            (i > 0 && strcasecmp(toc[i - 1].path, e.path) >= 0)) {
            ESP_LOGW(TAG, "%s: corrupt TOC at entry %u ('%s'); pak rejected",
                     srcPath, (unsigned)i, e.path);
            free(toc);
            fclose(f);
            return false;
        }
    }

    Pak& p = s_paks[s_count];
    snprintf(p.mount, sizeof p.mount, "/pak%d", s_count);
    strncpy(p.src, srcPath, sizeof p.src - 1);
    p.src[sizeof p.src - 1] = '\0';
    p.f     = f;                       // held open for the session: one FAT open, ever
    p.toc   = toc;
    p.count = (int)h.count;
    p.mtx   = xSemaphoreCreateMutex();
    memset(p.fds, 0, sizeof p.fds);

    esp_err_t err = esp_vfs_register_fs(p.mount, &s_fs_ops,
                                        ESP_VFS_FLAG_STATIC | ESP_VFS_FLAG_CONTEXT_PTR, &p);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: vfs register failed (%s)", srcPath, esp_err_to_name(err));
        vSemaphoreDelete(p.mtx);
        free(toc);
        fclose(f);
        return false;
    }
    s_count++;
    ESP_LOGI(TAG, "%s -> %s: %u files, %ld KB (TOC %u KB) in %d ms",
             srcPath, p.mount, (unsigned)h.count, fileSize / 1024,
             (unsigned)(tocBytes / 1024), (int)((esp_timer_get_time() - t0) / 1000));
    return true;
}

int pakfs_mount_all(const char* dir)
{
    DIR* d = opendir(dir);
    if (!d) { ESP_LOGI(TAG, "no mod-pack dir at %s", dir); return 0; }

    // Collect names first, then sort: FAT's readdir order is insertion order, and the
    // override rule ("later pak wins") must not depend on the order files were copied.
    char names[PAKFS_MAX_PAKS][64];
    int  n = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcasecmp(ent->d_name + len - 4, ".pak") != 0) continue;
        if (len >= sizeof names[0]) {
            ESP_LOGW(TAG, "pak name too long, skipped: %s", ent->d_name);
            continue;
        }
        // Against the GLOBAL total, not this call's: pakfs_mount_all runs more than once per
        // boot (base.pak from the data partition, then the card's mods), so a per-call limit
        // would let the second call walk off the end of s_paks[]. mount_one re-checks.
        if (s_count + n >= PAKFS_MAX_PAKS) {
            ESP_LOGW(TAG, "pak limit (%d) reached; ignored: %s/%s",
                     PAKFS_MAX_PAKS, dir, ent->d_name);
            continue;
        }
        strcpy(names[n++], ent->d_name);
    }
    closedir(d);

    for (int i = 1; i < n; i++)              // insertion sort; n <= 4
        for (int j = i; j > 0 && strcasecmp(names[j - 1], names[j]) > 0; j--) {
            char tmp[sizeof names[0]];
            strcpy(tmp, names[j]);
            strcpy(names[j], names[j - 1]);
            strcpy(names[j - 1], tmp);
        }

    int mounted = 0;
    for (int i = 0; i < n; i++) {
        char path[192];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (mount_one(path)) mounted++;
    }
    return mounted;
}

int pakfs_count() { return s_count; }

const char* pakfs_root(int i)   { return (i >= 0 && i < s_count) ? s_paks[i].mount : nullptr; }
const char* pakfs_source(int i) { return (i >= 0 && i < s_count) ? s_paks[i].src   : nullptr; }
