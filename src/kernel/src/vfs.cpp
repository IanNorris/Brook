#include "vfs.h"
#include "fatfs_glue.h"
#include "memory/heap.h"
#include "serial.h"
#include "memory/virtual_memory.h"
#include "sync/kmutex.h"

extern "C" {
#include "ff.h"
}

namespace brook {

// Global lock protecting all FatFS operations. FatFS is NOT thread-safe —
// concurrent f_read/f_open/f_lseek calls corrupt shared internal state.
// Uses a sleeping mutex so blocked processes yield the CPU instead of spinning.
static KMutex g_vfsLock;

// ---- Mount table ----

static constexpr uint32_t VFS_MAX_MOUNTS = 8;

struct MountEntry {
    char    mountPoint[64]; // e.g. "/" or "/boot"
    FATFS*  fs;             // FatFS work area (heap-allocated at mount time)
    uint8_t pdrv;           // FatFS physical drive number
    bool    used;
};

static MountEntry g_mounts[VFS_MAX_MOUNTS];

// ---- String helpers ----

static uint32_t StrLen(const char* s)
{
    uint32_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

static void StrCopy(char* dst, const char* src, uint32_t maxLen)
{
    uint32_t i = 0;
    while (i + 1 < maxLen && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static bool StrEq(const char* a, const char* b)
{
    while (*a && *b) { if (*a != *b) return false; ++a; ++b; }
    return *a == *b;
}

// Build the FatFS path: "N:rest" where N is pdrv and rest is the path
// relative to the mount point.
static void BuildFatPath(char* out, uint32_t maxLen,
                         uint8_t pdrv, const char* relPath)
{
    if (maxLen < 4) return;
    out[0] = static_cast<char>('0' + pdrv);
    out[1] = ':';
    // relPath starts with '/' already; skip leading '/' if relPath == "/"
    uint32_t i = 2;
    if (!relPath[0] || (relPath[0] == '/' && !relPath[1])) {
        out[i++] = '/';
        out[i] = '\0';
    } else {
        const char* p = relPath;
        while (*p && i + 1 < maxLen) out[i++] = *p++;
        out[i] = '\0';
    }
}

// Find the best (longest-prefix) mount for a given absolute path.
// Returns the mount entry and sets *relPath to the path within the mount.
static MountEntry* FindMount(const char* absPath, const char** relPath)
{
    MountEntry* best      = nullptr;
    uint32_t    bestLen   = 0;

    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        if (!g_mounts[i].used) continue;
        uint32_t mpLen = StrLen(g_mounts[i].mountPoint);
        // Check that absPath starts with mountPoint.
        bool match = true;
        for (uint32_t j = 0; j < mpLen; ++j) {
            if (absPath[j] != g_mounts[i].mountPoint[j]) { match = false; break; }
        }
        if (!match) continue;
        // Root mount "/" matches everything; for others, the next char must be '/' or '\0'.
        if (mpLen > 1 && absPath[mpLen] != '\0' && absPath[mpLen] != '/') continue;
        if (mpLen > bestLen) { bestLen = mpLen; best = &g_mounts[i]; }
    }

    if (best && relPath)
    {
        const char* rem = absPath + bestLen;
        *relPath = (rem[0] == '\0') ? "/" : rem;
    }
    return best;
}

// ---- FatFS vnode ops ----

// Cached file: entire file contents loaded into memory for fast random access.
// Multiple vnodes can share the same CachedFile via refCount.
struct CachedFile {
    FIL*     fil;       // Original FatFS handle (closed when refCount→0)
    uint8_t* data;
    uint64_t size;
    int32_t  refCount;  // Number of vnodes sharing this cache entry
    char     path[64];  // Canonical path for dedup (e.g. "0:/DOOM1.WAD")
};

// Global shared file cache — small fixed table for dedup across openers.
static constexpr uint32_t FILE_CACHE_MAX = 16;
static CachedFile* g_fileCache[FILE_CACHE_MAX] = {};

// Look up an existing cache entry by FatFS path (caller must hold g_vfsLock).
static CachedFile* FileCacheLookup(const char* fatPath)
{
    for (uint32_t i = 0; i < FILE_CACHE_MAX; ++i) {
        if (g_fileCache[i] && StrEq(g_fileCache[i]->path, fatPath))
            return g_fileCache[i];
    }
    return nullptr;
}

// Insert a new cache entry (caller must hold g_vfsLock). Returns false if table full.
static bool FileCacheInsert(CachedFile* cf)
{
    for (uint32_t i = 0; i < FILE_CACHE_MAX; ++i) {
        if (!g_fileCache[i]) {
            g_fileCache[i] = cf;
            return true;
        }
    }
    return false;
}

// Remove a cache entry (caller must hold g_vfsLock).
static void FileCacheRemove(CachedFile* cf)
{
    for (uint32_t i = 0; i < FILE_CACHE_MAX; ++i) {
        if (g_fileCache[i] == cf) {
            g_fileCache[i] = nullptr;
            return;
        }
    }
}

static int FatFileOpen(Vnode* vn, int flags)
{
    (void)vn; (void)flags;
    return 0; // Already open by VfsOpen()
}

// Preload file into memory. Called on first read for files > threshold.
// CacheFile — locks internally per-chunk so other CPUs can interleave.
// Caller must NOT hold g_vfsLock. fatPath is stored for dedup lookups.
static CachedFile* CacheFileUnlocked(FIL* fil, const char* fatPath)
{
    uint64_t size = f_size(fil);
    auto* cf = static_cast<CachedFile*>(kmalloc(sizeof(CachedFile)));
    if (!cf) return nullptr;

    cf->fil      = fil;
    cf->size     = size;
    cf->refCount = 1;
    // Store path for dedup lookups
    uint32_t pi = 0;
    for (; fatPath[pi] && pi < sizeof(cf->path) - 1; ++pi) cf->path[pi] = fatPath[pi];
    cf->path[pi] = '\0';

    // Use VmmAllocPages for large allocations (kmalloc can't do multi-MB).
    uint64_t numPages = (size + 4095) / 4096;
    cf->data = static_cast<uint8_t*>(
        reinterpret_cast<void*>(VmmAllocPages(numPages, VMM_WRITABLE, MemTag::KernelData).raw()));
    if (!cf->data) {
        SerialPrintf("VFS: cache alloc failed (%lu pages)\n", (unsigned long)numPages);
        kfree(cf);
        return nullptr;
    }

    // Read entire file — acquire VFS lock per chunk to allow interleaving.
    {
        KMutexLock(&g_vfsLock);
        f_lseek(fil, 0);
        KMutexUnlock(&g_vfsLock);
    }
    uint64_t remaining = size;
    uint64_t off = 0;
    while (remaining > 0) {
        UINT chunk = (remaining > 32768) ? 32768 : static_cast<UINT>(remaining);
        UINT br = 0;
        KMutexLock(&g_vfsLock);
        FRESULT res = f_read(fil, cf->data + off, chunk, &br);
        KMutexUnlock(&g_vfsLock);
        if (res != FR_OK || br == 0) {
            SerialPrintf("VFS: cache read failed at off=%lu (res=%u)\n",
                         (unsigned long)off, (unsigned)res);
            VmmFreePages(VirtualAddress(reinterpret_cast<uint64_t>(cf->data)), numPages);
            kfree(cf);
            return nullptr;
        }
        off += br;
        remaining -= br;
    }
    DbgPrintf("VFS: cached file (%lu bytes, %lu pages)\n",
                 (unsigned long)size, (unsigned long)numPages);
    return cf;
}

static int CachedFileRead(Vnode* vn, void* buf, uint64_t len, uint64_t* offset)
{
    auto* cf = static_cast<CachedFile*>(vn->priv);
    if (*offset >= cf->size) return 0;
    uint64_t avail = cf->size - *offset;
    if (len > avail) len = avail;
    auto* dst = static_cast<uint8_t*>(buf);
    auto* src = cf->data + *offset;
    for (uint64_t i = 0; i < len; ++i) dst[i] = src[i];
    *offset += len;
    return static_cast<int>(len);
}

static void CachedFileClose(Vnode* vn)
{
    auto* cf = static_cast<CachedFile*>(vn->priv);
    if (!cf) return;

    // Decrement refcount under VFS lock (protects the cache table).
    KMutexLock(&g_vfsLock);
    int32_t refs = --cf->refCount;
    if (refs <= 0)
        FileCacheRemove(cf);
    KMutexUnlock(&g_vfsLock);

    if (refs <= 0) {
        if (cf->fil) f_close(cf->fil);
        if (cf->data) {
            uint64_t numPages = (cf->size + 4095) / 4096;
            VmmFreePages(VirtualAddress(reinterpret_cast<uint64_t>(cf->data)), numPages);
        }
        kfree(cf);
    }
}

static int CachedFileStat(Vnode* vn, VnodeStat* st)
{
    auto* cf = static_cast<CachedFile*>(vn->priv);
    st->size  = cf->size;
    st->isDir = false;
    return 0;
}

static const VnodeOps g_cachedFileOps = {
    .open    = FatFileOpen,
    .read    = CachedFileRead,
    .write   = nullptr,
    .readdir = nullptr,
    .close   = CachedFileClose,
    .stat    = CachedFileStat,
};

static int FatFileRead(Vnode* vn, void* buf, uint64_t len, uint64_t* offset)
{
    FIL* fil = static_cast<FIL*>(vn->priv);

    KMutexLock(&g_vfsLock);

    // Seek if needed.
    if (f_tell(fil) != *offset)
    {
        if (f_lseek(fil, static_cast<FSIZE_t>(*offset)) != FR_OK)
        {
            KMutexUnlock(&g_vfsLock);
            return -1;
        }
    }

    UINT br = 0;
    FRESULT res = f_read(fil, buf, static_cast<UINT>(len), &br);
    KMutexUnlock(&g_vfsLock);

    if (res != FR_OK) return -1;

    *offset += br;
    return static_cast<int>(br);
}

static int FatFileWrite(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset)
{
    FIL* fil = static_cast<FIL*>(vn->priv);

    KMutexLock(&g_vfsLock);
    if (f_tell(fil) != *offset) {
        if (f_lseek(fil, static_cast<FSIZE_t>(*offset)) != FR_OK) {
            KMutexUnlock(&g_vfsLock);
            return -1;
        }
    }
    UINT bw = 0;
    FRESULT res = f_write(fil, buf, static_cast<UINT>(len), &bw);
    KMutexUnlock(&g_vfsLock);

    if (res != FR_OK) return -1;
    *offset += bw;
    return static_cast<int>(bw);
}

static int FatDirReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie)
{
    DIR* dir = static_cast<DIR*>(vn->priv);
    (void)cookie; // FatFS DIR maintains its own iterator state

    KMutexLock(&g_vfsLock);
    FILINFO fno;
    FRESULT res = f_readdir(dir, &fno);
    KMutexUnlock(&g_vfsLock);

    if (res != FR_OK) { SerialPrintf("VFS: readdir failed (res=%u)\n", static_cast<unsigned>(res)); return -1; }
    if (fno.fname[0] == '\0') return 0; // end of directory

    DbgPrintf("VFS: readdir entry: '%s' (%s, %lu bytes)\n",
                 fno.fname, (fno.fattrib & AM_DIR) ? "dir" : "file",
                 static_cast<unsigned long>(fno.fsize));
    StrCopy(out->name, fno.fname, sizeof(out->name));
    out->size  = fno.fsize;
    out->isDir = (fno.fattrib & AM_DIR) != 0;
    return 1;
}

static void FatFileClose(Vnode* vn)
{
    KMutexLock(&g_vfsLock);
    if (vn->type == VnodeType::File)
    {
        FIL* fil = static_cast<FIL*>(vn->priv);
        f_close(fil);
        kfree(fil);
    }
    else
    {
        DIR* dir = static_cast<DIR*>(vn->priv);
        f_closedir(dir);
        kfree(dir);
    }
    KMutexUnlock(&g_vfsLock);
}

static int FatStat(Vnode* vn, VnodeStat* st)
{
    if (vn->type == VnodeType::File)
    {
        FIL* fil = static_cast<FIL*>(vn->priv);
        st->size  = f_size(fil);
        st->isDir = false;
    }
    else
    {
        st->size  = 0;
        st->isDir = true;
    }
    return 0;
}

static const VnodeOps g_fatFileOps = {
    .open    = FatFileOpen,
    .read    = FatFileRead,
    .write   = FatFileWrite,
    .readdir = nullptr,
    .close   = FatFileClose,
    .stat    = FatStat,
};

static const VnodeOps g_fatDirOps = {
    .open    = FatFileOpen,
    .read    = nullptr,
    .write   = nullptr,
    .readdir = FatDirReaddir,
    .close   = FatFileClose,
    .stat    = FatStat,
};

// ---- Public API ----

void VfsInit()
{
    KMutexInit(&g_vfsLock);
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; ++i) g_mounts[i].used = false;
    SerialPuts("VFS: initialised\n");
}

bool VfsMount(const char* mountPoint, const char* fsName, uint8_t pdrv)
{
    if (!mountPoint || !fsName) return false;
    if (!StrEq(fsName, "fatfs"))
    {
        SerialPrintf("VFS: unknown filesystem '%s'\n", fsName);
        return false;
    }

    // Find a free slot.
    MountEntry* slot = nullptr;
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        if (!g_mounts[i].used) { slot = &g_mounts[i]; break; }
    }
    if (!slot) { SerialPuts("VFS: mount table full\n"); return false; }

    auto* fs = static_cast<FATFS*>(kmalloc(sizeof(FATFS)));
    if (!fs) { SerialPuts("VFS: kmalloc failed for FATFS\n"); return false; }

    char fatPath[8];
    fatPath[0] = static_cast<char>('0' + pdrv);
    fatPath[1] = ':'; fatPath[2] = '\0';

    KMutexLock(&g_vfsLock);
    FRESULT res = f_mount(fs, fatPath, 1);
    KMutexUnlock(&g_vfsLock);

    if (res != FR_OK)
    {
        SerialPrintf("VFS: f_mount failed (res=%u) for drive %u\n",
                     static_cast<unsigned>(res), pdrv);
        kfree(fs);
        return false;
    }

    slot->fs   = fs;
    slot->pdrv = pdrv;
    slot->used = true;
    StrCopy(slot->mountPoint, mountPoint, sizeof(slot->mountPoint));

    // Diagnostic: show physical page backing the FATFS.win buffer
    uint64_t winVirt = reinterpret_cast<uint64_t>(&fs->win[0]);
    [[maybe_unused]] uint64_t winPhys = VmmVirtToPhys(KernelPageTable, VirtualAddress(winVirt)).raw();
    DbgPrintf("VFS: FATFS.win virt=0x%lx phys=0x%lx (sizeof FATFS=%lu)\n",
                 winVirt, winPhys, static_cast<unsigned long>(sizeof(FATFS)));

    DbgPrintf("VFS: mounted fatfs drive %u at '%s'\n", pdrv, mountPoint);
    return true;
}

bool VfsUnmount(const char* mountPoint)
{
    if (!mountPoint) return false;
    for (uint32_t i = 0; i < VFS_MAX_MOUNTS; ++i)
    {
        if (!g_mounts[i].used) continue;
        if (!StrEq(g_mounts[i].mountPoint, mountPoint)) continue;

        char fatPath[8];
        fatPath[0] = static_cast<char>('0' + g_mounts[i].pdrv);
        fatPath[1] = ':'; fatPath[2] = '\0';

        KMutexLock(&g_vfsLock);
        f_unmount(fatPath);
        KMutexUnlock(&g_vfsLock);

        kfree(g_mounts[i].fs);
        g_mounts[i].fs   = nullptr;
        g_mounts[i].used = false;
        SerialPrintf("VFS: unmounted '%s'\n", mountPoint);
        return true;
    }
    return false;
}

Vnode* VfsOpen(const char* path, int flags)
{
    if (!path || !path[0]) return nullptr;

    const char* relPath = nullptr;
    MountEntry* mount   = FindMount(path, &relPath);
    if (!mount)
    {
        SerialPrintf("VFS: no mount for '%s'\n", path);
        return nullptr;
    }

    char fatPath[256];
    BuildFatPath(fatPath, sizeof(fatPath), mount->pdrv, relPath);

    // Try as a file first.
    auto* fil = static_cast<FIL*>(kmalloc(sizeof(FIL)));
    if (!fil) return nullptr;

    BYTE mode = FA_READ;
    if (flags & VFS_O_WRITE)  mode |= FA_WRITE;
    if (flags & VFS_O_CREATE) mode |= (flags & VFS_O_TRUNC) ? FA_CREATE_ALWAYS : FA_OPEN_ALWAYS;
    if (flags & VFS_O_TRUNC)  mode |= FA_CREATE_ALWAYS;
    DbgPrintf("VFS: f_open('%s', mode=0x%x)\n", fatPath, mode);

    // Check shared cache first (read-only opens only).
    if (!(flags & VFS_O_WRITE)) {
        KMutexLock(&g_vfsLock);
        CachedFile* existing = FileCacheLookup(fatPath);
        if (existing) {
            existing->refCount++;
            KMutexUnlock(&g_vfsLock);
            kfree(fil); // Don't need FatFS handle — sharing cached data

            auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
            if (!vn) {
                KMutexLock(&g_vfsLock);
                existing->refCount--;
                KMutexUnlock(&g_vfsLock);
                return nullptr;
            }
            vn->ops  = &g_cachedFileOps;
            vn->type = VnodeType::File;
            vn->priv = existing;
            DbgPrintf("VFS: sharing cached '%s' (refCount=%d)\n",
                         fatPath, existing->refCount);
            return vn;
        }
        KMutexUnlock(&g_vfsLock);
    }

    KMutexLock(&g_vfsLock);
    FRESULT res = f_open(fil, fatPath, mode);
    if (res != FR_OK)
    {
        KMutexUnlock(&g_vfsLock);
        DbgPrintf("VFS: f_open('%s') result: %d\n", fatPath, (int)res);
    }
    if (res == FR_OK)
    {
        auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!vn) { f_close(fil); KMutexUnlock(&g_vfsLock); kfree(fil); return nullptr; }

        // Cache large read-only files entirely in memory for fast random access.
        static constexpr uint64_t CACHE_THRESHOLD = 64 * 1024; // 64 KB
        uint64_t fileSize = f_size(fil);
        if (!(flags & VFS_O_WRITE) && fileSize >= CACHE_THRESHOLD)
        {
            KMutexUnlock(&g_vfsLock);
            CachedFile* cf = CacheFileUnlocked(fil, fatPath);
            if (cf) {
                // Insert into shared cache table.
                KMutexLock(&g_vfsLock);
                FileCacheInsert(cf); // Best-effort; dedup still works if table full
                KMutexUnlock(&g_vfsLock);

                vn->ops  = &g_cachedFileOps;
                vn->type = VnodeType::File;
                vn->priv = cf;
                return vn;
            }
            // Cache failed — fall through to uncached path
        }
        else
        {
            KMutexUnlock(&g_vfsLock);
        }

        vn->ops  = &g_fatFileOps;
        vn->type = VnodeType::File;
        vn->priv = fil;

        // Seek to end for append mode.
        if (flags & VFS_O_APPEND)
        {
            KMutexLock(&g_vfsLock);
            f_lseek(fil, f_size(fil));
            KMutexUnlock(&g_vfsLock);
        }

        return vn;
    }
    if (res != FR_NO_FILE && res != FR_NO_PATH)
        DbgPrintf("VFS: f_open('%s') failed: %d\n", fatPath, (int)res);
    kfree(fil);

    // Try as a directory.
    auto* dir = static_cast<DIR*>(kmalloc(sizeof(DIR)));
    if (!dir) return nullptr;

    KMutexLock(&g_vfsLock);
    res = f_opendir(dir, fatPath);
    KMutexUnlock(&g_vfsLock);

    if (res == FR_OK)
    {
        DbgPrintf("VFS: opened dir '%s' (fatpath='%s')\n", path, fatPath);
        auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!vn) {
            KMutexLock(&g_vfsLock);
            f_closedir(dir);
            KMutexUnlock(&g_vfsLock);
            kfree(dir);
            return nullptr;
        }
        vn->ops  = &g_fatDirOps;
        vn->type = VnodeType::Dir;
        vn->priv = dir;
        return vn;
    }
    kfree(dir);

    SerialPrintf("VFS: cannot open '%s' (fatpath='%s')\n", path, fatPath);
    return nullptr;
}

int VfsRead(Vnode* vn, void* buf, uint64_t len, uint64_t* offset)
{
    if (!vn || !vn->ops->read) return -1;
    return vn->ops->read(vn, buf, len, offset);
}

int VfsWrite(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset)
{
    if (!vn || !vn->ops->write) return -1;
    return vn->ops->write(vn, buf, len, offset);
}

int VfsReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie)
{
    if (!vn || !vn->ops->readdir) return -1;
    return vn->ops->readdir(vn, out, cookie);
}

int VfsStat(Vnode* vn, VnodeStat* st)
{
    if (!vn || !vn->ops->stat) return -1;
    return vn->ops->stat(vn, st);
}

int VfsStatPath(const char* path, VnodeStat* st)
{
    if (!path || !path[0] || !st) return -1;

    const char* relPath = nullptr;
    MountEntry* mount   = FindMount(path, &relPath);
    if (!mount) return -1;

    char fatPath[256];
    BuildFatPath(fatPath, sizeof(fatPath), mount->pdrv, relPath);

    // Check for root directory
    bool isRoot = (relPath[0] == '/' && relPath[1] == '\0') || relPath[0] == '\0';
    if (isRoot) {
        st->size  = 0;
        st->isDir = true;
        return 0;
    }

    FILINFO fno;
    KMutexLock(&g_vfsLock);
    FRESULT res = f_stat(fatPath, &fno);
    KMutexUnlock(&g_vfsLock);

    if (res != FR_OK) return -1;

    st->isDir = (fno.fattrib & AM_DIR) != 0;
    st->size  = st->isDir ? 0 : fno.fsize;
    return 0;
}

int VfsSync(Vnode* vn)
{
    if (!vn || vn->type != VnodeType::File) return -1;
    // Only works for direct FatFS files (not cached).
    if (vn->ops != &g_fatFileOps) return 0;
    FIL* fil = static_cast<FIL*>(vn->priv);
    KMutexLock(&g_vfsLock);
    FRESULT res = f_sync(fil);
    KMutexUnlock(&g_vfsLock);
    return (res == FR_OK) ? 0 : -1;
}

void VfsClose(Vnode* vn)
{
    if (!vn) return;
    if (vn->ops && vn->ops->close) vn->ops->close(vn);
    kfree(vn);
}

} // namespace brook
