#include "fatfs_vfs.h"
#include "vfs.h"
#include "fatfs_glue.h"
#include "memory/heap.h"
#include "serial.h"
#include "memory/virtual_memory.h"
#include "sync/kmutex.h"

// Forward-declare SchedulerYield — weak so test builds link without the scheduler.
namespace brook { __attribute__((weak)) void SchedulerYield(); }

extern "C" {
#include "ff.h"
}

namespace brook {

// FatFS is NOT thread-safe — all FatFS calls are serialised by this lock.
static KMutex g_fatLock;
static bool   g_fatLockInit = false;

struct FatFsMountData {
    FATFS fs;
    uint8_t pdrv;
};

static void EnsureLock()
{
    if (!g_fatLockInit) { KMutexInit(&g_fatLock); g_fatLockInit = true; }
}

// ---- String helpers (local to this TU) ----

static bool StrEqLocal(const char* a, const char* b)
{
    while (*a && *b) { if (*a != *b) return false; ++a; ++b; }
    return *a == *b;
}

static void StrCopyLocal(char* dst, const char* src, uint32_t maxLen)
{
    uint32_t i = 0;
    while (i + 1 < maxLen && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

// Build FatFS path: "N:rest" where N = pdrv.
static void BuildFatPath(char* out, uint32_t maxLen,
                         uint8_t pdrv, const char* relPath)
{
    if (maxLen < 4) return;
    out[0] = static_cast<char>('0' + pdrv);
    out[1] = ':';
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

// ---- File cache (for large read-only files) ----

struct CachedFile {
    FIL*     fil;
    uint8_t* data;
    uint64_t size;
    int32_t  refCount;
    volatile bool loading;
    char     path[64];
};

static constexpr uint32_t FILE_CACHE_MAX = 16;
static CachedFile* g_fileCache[FILE_CACHE_MAX] = {};

static CachedFile* FileCacheLookup(const char* fatPath)
{
    for (uint32_t i = 0; i < FILE_CACHE_MAX; ++i) {
        if (g_fileCache[i] && StrEqLocal(g_fileCache[i]->path, fatPath))
            return g_fileCache[i];
    }
    return nullptr;
}

static bool FileCacheInsert(CachedFile* cf)
{
    for (uint32_t i = 0; i < FILE_CACHE_MAX; ++i) {
        if (!g_fileCache[i]) { g_fileCache[i] = cf; return true; }
    }
    return false;
}

static void FileCacheRemove(CachedFile* cf)
{
    for (uint32_t i = 0; i < FILE_CACHE_MAX; ++i) {
        if (g_fileCache[i] == cf) { g_fileCache[i] = nullptr; return; }
    }
}

// Preload file into memory (caller must NOT hold g_fatLock).
static CachedFile* CacheFileUnlocked(FIL* fil, const char* fatPath)
{
    uint64_t size = f_size(fil);
    auto* cf = static_cast<CachedFile*>(kmalloc(sizeof(CachedFile)));
    if (!cf) return nullptr;

    cf->fil      = fil;
    cf->size     = size;
    cf->refCount = 1;
    cf->loading  = false;
    uint32_t pi = 0;
    for (; fatPath[pi] && pi < sizeof(cf->path) - 1; ++pi) cf->path[pi] = fatPath[pi];
    cf->path[pi] = '\0';

    uint64_t numPages = (size + 4095) / 4096;
    cf->data = static_cast<uint8_t*>(
        reinterpret_cast<void*>(VmmAllocPages(numPages, VMM_WRITABLE, MemTag::KernelData).raw()));
    if (!cf->data) {
        SerialPrintf("FATFS_VFS: cache alloc failed (%lu pages)\n", (unsigned long)numPages);
        kfree(cf);
        return nullptr;
    }

    {
        KMutexLock(&g_fatLock);
        f_lseek(fil, 0);
        KMutexUnlock(&g_fatLock);
    }
    uint64_t remaining = size;
    uint64_t off = 0;
    while (remaining > 0) {
        UINT chunk = (remaining > 65536) ? 65536 : static_cast<UINT>(remaining);
        UINT br = 0;
        KMutexLock(&g_fatLock);
        FRESULT res = f_read(fil, cf->data + off, chunk, &br);
        KMutexUnlock(&g_fatLock);
        if (res != FR_OK || br == 0) {
            VmmFreePages(VirtualAddress(reinterpret_cast<uint64_t>(cf->data)), numPages);
            kfree(cf);
            return nullptr;
        }
        off += br;
        remaining -= br;
    }
    DbgPrintf("FATFS_VFS: cached file (%lu bytes)\n", (unsigned long)size);
    return cf;
}

// ---- Vnode operations ----

static int FatFileOpen(Vnode* vn, int flags) { (void)vn; (void)flags; return 0; }

static int FatFileRead(Vnode* vn, void* buf, uint64_t len, uint64_t* offset)
{
    FIL* fil = static_cast<FIL*>(vn->priv);
    KMutexLock(&g_fatLock);
    if (f_tell(fil) != *offset) {
        if (f_lseek(fil, static_cast<FSIZE_t>(*offset)) != FR_OK) {
            KMutexUnlock(&g_fatLock);
            return -1;
        }
    }
    UINT br = 0;
    FRESULT res = f_read(fil, buf, static_cast<UINT>(len), &br);
    KMutexUnlock(&g_fatLock);
    if (res != FR_OK) return -1;
    *offset += br;
    return static_cast<int>(br);
}

static int FatFileWrite(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset)
{
    FIL* fil = static_cast<FIL*>(vn->priv);
    KMutexLock(&g_fatLock);
    if (f_tell(fil) != *offset) {
        if (f_lseek(fil, static_cast<FSIZE_t>(*offset)) != FR_OK) {
            KMutexUnlock(&g_fatLock);
            return -1;
        }
    }
    UINT bw = 0;
    FRESULT res = f_write(fil, buf, static_cast<UINT>(len), &bw);
    KMutexUnlock(&g_fatLock);
    if (res != FR_OK) return -1;
    *offset += bw;
    return static_cast<int>(bw);
}

static int FatDirReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie)
{
    DIR* dir = static_cast<DIR*>(vn->priv);
    (void)cookie;
    KMutexLock(&g_fatLock);
    FILINFO fno;
    FRESULT res = f_readdir(dir, &fno);
    KMutexUnlock(&g_fatLock);
    if (res != FR_OK) return -1;
    if (fno.fname[0] == '\0') return 0;
    StrCopyLocal(out->name, fno.fname, sizeof(out->name));
    out->size  = fno.fsize;
    out->isDir = (fno.fattrib & AM_DIR) != 0;
    return 1;
}

static void FatFileClose(Vnode* vn)
{
    KMutexLock(&g_fatLock);
    if (vn->type == VnodeType::File) {
        FIL* fil = static_cast<FIL*>(vn->priv);
        f_close(fil);
        kfree(fil);
    } else {
        DIR* dir = static_cast<DIR*>(vn->priv);
        f_closedir(dir);
        kfree(dir);
    }
    KMutexUnlock(&g_fatLock);
}

static int FatStat(Vnode* vn, VnodeStat* st)
{
    if (vn->type == VnodeType::File) {
        FIL* fil = static_cast<FIL*>(vn->priv);
        st->size  = f_size(fil);
        st->isDir = false;
    } else {
        st->size  = 0;
        st->isDir = true;
    }
    st->isSymlink = false;
    return 0;
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
    KMutexLock(&g_fatLock);
    int32_t refs = --cf->refCount;
    if (refs <= 0) FileCacheRemove(cf);
    KMutexUnlock(&g_fatLock);
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
    st->isSymlink = false;
    return 0;
}

static const VnodeOps g_cachedFileOps = {
    .open = FatFileOpen, .read = CachedFileRead, .write = nullptr,
    .readdir = nullptr, .close = CachedFileClose, .stat = CachedFileStat,
};

static const VnodeOps g_fatFileOps = {
    .open = FatFileOpen, .read = FatFileRead, .write = FatFileWrite,
    .readdir = nullptr, .close = FatFileClose, .stat = FatStat,
};

static const VnodeOps g_fatDirOps = {
    .open = FatFileOpen, .read = nullptr, .write = nullptr,
    .readdir = FatDirReaddir, .close = FatFileClose, .stat = FatStat,
};

// ---- VfsFsOps callbacks ----

static bool FatFsMount(uint8_t pdrv, void** mountPriv)
{
    EnsureLock();
    auto* data = static_cast<FatFsMountData*>(kmalloc(sizeof(FatFsMountData)));
    if (!data) { SerialPuts("FATFS_VFS: kmalloc failed for FATFS\n"); return false; }
    __builtin_memset(data, 0, sizeof(FatFsMountData));
    data->pdrv = pdrv;

    char fatPath[8];
    fatPath[0] = static_cast<char>('0' + pdrv);
    fatPath[1] = ':'; fatPath[2] = '\0';

    KMutexLock(&g_fatLock);
    FRESULT res = f_mount(&data->fs, fatPath, 1);
    if (res != FR_OK)
        f_mount(nullptr, fatPath, 0);
    KMutexUnlock(&g_fatLock);

    if (res != FR_OK) {
        SerialPrintf("FATFS_VFS: f_mount failed (res=%u) for drive %u\n",
                     static_cast<unsigned>(res), pdrv);
        kfree(data);
        return false;
    }

    *mountPriv = data;
    DbgPrintf("FATFS_VFS: mounted drive %u\n", pdrv);
    return true;
}

static void FatFsUnmount(void* mountPriv)
{
    auto* data = static_cast<FatFsMountData*>(mountPriv);
    if (!data) return;

    char fatPath[8];
    fatPath[0] = static_cast<char>('0' + data->pdrv);
    fatPath[1] = ':'; fatPath[2] = '\0';

    KMutexLock(&g_fatLock);
    f_mount(nullptr, fatPath, 0);
    KMutexUnlock(&g_fatLock);

    kfree(data);
}

static Vnode* FatFsOpen(void* /*mountPriv*/, uint8_t pdrv,
                        const char* relPath, int flags)
{
    char fatPath[256];
    BuildFatPath(fatPath, sizeof(fatPath), pdrv, relPath);

    auto* fil = static_cast<FIL*>(kmalloc(sizeof(FIL)));
    if (!fil) return nullptr;

    BYTE mode = FA_READ;
    if (flags & VFS_O_WRITE)  mode |= FA_WRITE;
    if (flags & VFS_O_CREATE) mode |= (flags & VFS_O_TRUNC) ? FA_CREATE_ALWAYS : FA_OPEN_ALWAYS;
    if (flags & VFS_O_TRUNC)  mode |= FA_CREATE_ALWAYS;

    // Check shared cache first (read-only opens).
    if (!(flags & VFS_O_WRITE)) {
        KMutexLock(&g_fatLock);
        CachedFile* existing = FileCacheLookup(fatPath);
        if (existing) {
            existing->refCount++;
            KMutexUnlock(&g_fatLock);
            kfree(fil);
            while (__atomic_load_n(&existing->loading, __ATOMIC_ACQUIRE)) {
                if (SchedulerYield) SchedulerYield();
            }
            if (!existing->data) {
                KMutexLock(&g_fatLock);
                existing->refCount--;
                KMutexUnlock(&g_fatLock);
                return nullptr;
            }
            auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
            if (!vn) {
                KMutexLock(&g_fatLock);
                existing->refCount--;
                KMutexUnlock(&g_fatLock);
                return nullptr;
            }
            vn->ops = &g_cachedFileOps; vn->type = VnodeType::File;
            vn->priv = existing; vn->refCount = 1;
            return vn;
        }
        KMutexUnlock(&g_fatLock);
    }

    KMutexLock(&g_fatLock);
    FRESULT res = f_open(fil, fatPath, mode);
    if (res != FR_OK) {
        KMutexUnlock(&g_fatLock);
    }
    if (res == FR_OK) {
        auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!vn) { f_close(fil); KMutexUnlock(&g_fatLock); kfree(fil); return nullptr; }

        static constexpr uint64_t CACHE_THRESHOLD = 64 * 1024;
        uint64_t fileSize = f_size(fil);
        if (!(flags & VFS_O_WRITE) && fileSize >= CACHE_THRESHOLD) {
            auto* placeholder = static_cast<CachedFile*>(kmalloc(sizeof(CachedFile)));
            if (placeholder) {
                placeholder->fil = fil; placeholder->data = nullptr;
                placeholder->size = fileSize; placeholder->refCount = 1;
                placeholder->loading = true;
                uint32_t pi = 0;
                for (; fatPath[pi] && pi < sizeof(placeholder->path) - 1; ++pi)
                    placeholder->path[pi] = fatPath[pi];
                placeholder->path[pi] = '\0';
                FileCacheInsert(placeholder);
            }
            KMutexUnlock(&g_fatLock);

            CachedFile* cf = CacheFileUnlocked(fil, fatPath);
            if (cf && placeholder) {
                placeholder->data = cf->data;
                placeholder->size = cf->size;
                __atomic_store_n(&placeholder->loading, false, __ATOMIC_RELEASE);
                cf->data = nullptr;
                kfree(cf);
                vn->ops = &g_cachedFileOps; vn->type = VnodeType::File;
                vn->priv = placeholder; vn->refCount = 1;
                return vn;
            }
            if (placeholder)
                __atomic_store_n(&placeholder->loading, false, __ATOMIC_RELEASE);
            if (cf) {
                KMutexLock(&g_fatLock);
                FileCacheInsert(cf);
                KMutexUnlock(&g_fatLock);
                vn->ops = &g_cachedFileOps; vn->type = VnodeType::File;
                vn->priv = cf; vn->refCount = 1;
                return vn;
            }
        } else {
            KMutexUnlock(&g_fatLock);
        }

        vn->ops = &g_fatFileOps; vn->type = VnodeType::File;
        vn->priv = fil; vn->refCount = 1;

        if (flags & VFS_O_APPEND) {
            KMutexLock(&g_fatLock);
            f_lseek(fil, f_size(fil));
            KMutexUnlock(&g_fatLock);
        }
        return vn;
    }

    kfree(fil);

    // Try as directory.
    auto* dir = static_cast<DIR*>(kmalloc(sizeof(DIR)));
    if (!dir) return nullptr;

    KMutexLock(&g_fatLock);
    res = f_opendir(dir, fatPath);
    KMutexUnlock(&g_fatLock);

    if (res == FR_OK) {
        auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!vn) {
            KMutexLock(&g_fatLock);
            f_closedir(dir);
            KMutexUnlock(&g_fatLock);
            kfree(dir);
            return nullptr;
        }
        vn->ops = &g_fatDirOps; vn->type = VnodeType::Dir;
        vn->priv = dir; vn->refCount = 1;
        return vn;
    }
    kfree(dir);
    return nullptr;
}

static int FatFsStatPath(void* /*mountPriv*/, uint8_t pdrv,
                         const char* relPath, VnodeStat* st)
{
    bool isRoot = (relPath[0] == '/' && relPath[1] == '\0') || relPath[0] == '\0';
    if (isRoot) { st->size = 0; st->isDir = true; st->isSymlink = false; return 0; }

    char fatPath[256];
    BuildFatPath(fatPath, sizeof(fatPath), pdrv, relPath);

    FILINFO fno;
    KMutexLock(&g_fatLock);
    FRESULT res = f_stat(fatPath, &fno);
    KMutexUnlock(&g_fatLock);
    if (res != FR_OK) return -1;

    st->isDir = (fno.fattrib & AM_DIR) != 0;
    st->size  = st->isDir ? 0 : fno.fsize;
    st->isSymlink = false;
    return 0;
}

static int FatFsUnlink(void* /*mountPriv*/, uint8_t pdrv, const char* relPath)
{
    char fatPath[256];
    BuildFatPath(fatPath, sizeof(fatPath), pdrv, relPath);
    KMutexLock(&g_fatLock);
    FRESULT res = f_unlink(fatPath);
    KMutexUnlock(&g_fatLock);
    return (res == FR_OK) ? 0 : -1;
}

static int FatFsMkdir(void* /*mountPriv*/, uint8_t pdrv, const char* relPath)
{
    char fatPath[256];
    BuildFatPath(fatPath, sizeof(fatPath), pdrv, relPath);
    KMutexLock(&g_fatLock);
    FRESULT res = f_mkdir(fatPath);
    KMutexUnlock(&g_fatLock);
    return (res == FR_OK || res == FR_EXIST) ? 0 : -1;
}

static int FatFsRename(void* /*mountPriv*/, uint8_t pdrv,
                       const char* oldRelPath, const char* newRelPath)
{
    char fatOld[256], fatNew[256];
    BuildFatPath(fatOld, sizeof(fatOld), pdrv, oldRelPath);
    BuildFatPath(fatNew, sizeof(fatNew), pdrv, newRelPath);
    KMutexLock(&g_fatLock);
    FRESULT res = f_rename(fatOld, fatNew);
    KMutexUnlock(&g_fatLock);
    return (res == FR_OK) ? 0 : -1;
}

static const VfsFsOps g_fatFsOps = {
    .mount      = FatFsMount,
    .unmount    = FatFsUnmount,
    .open       = FatFsOpen,
    .stat_path  = FatFsStatPath,
    .lstat_path = nullptr,
    .unlink     = FatFsUnlink,
    .mkdir      = FatFsMkdir,
    .rename     = FatFsRename,
    .symlink    = nullptr,
    .readlink   = nullptr,
    .sync       = nullptr,
};

// ---- Public init ----

void FatFsVfsRegister()
{
    EnsureLock();
    VfsRegisterFs("fatfs", &g_fatFsOps);
}

} // namespace brook
