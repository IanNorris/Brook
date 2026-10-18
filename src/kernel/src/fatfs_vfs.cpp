#include "fatfs_vfs.h"
#include "vfs.h"
#include "process.h"
#include "fatfs_glue.h"
#include "memory/heap.h"
#include "serial.h"
#include "memory/virtual_memory.h"
#include "sync/kmutex.h"
#include "string.h"

// Forward-declare SchedulerYield — weak so test builds link without the scheduler.
namespace brook { __attribute__((weak)) void SchedulerYield(); }

extern "C" {
#include "ff.h"
}

namespace brook {

// FatFS is NOT thread-safe — all FatFS calls are serialised by this lock.
static KMutex g_fatLock;
static bool   g_fatLockInit = false;

// Linux/musl errno values used to report FatFS failures honestly. The kernel
// has no shared errno header (syscall.cpp defines its own), so the handful we
// need are defined locally here too. Historically the VFS ops returned a bare
// -1 on any FatFS error; on the Linux syscall ABI musl turns that into
// errno = 1 = EPERM ("Operation not permitted"), which is misleading — a
// transient disk read failure during shared-library loading then surfaces as a
// bogus permission error. Mapping FRESULT to a real errno keeps the failure
// honest and diagnosable.
static constexpr int FVFS_EIO    = 5;
static constexpr int FVFS_ENOENT = 2;
static constexpr int FVFS_EACCES = 13;
static constexpr int FVFS_EROFS  = 30;
static constexpr int FVFS_EINVAL = 22;
static constexpr int FVFS_EMFILE = 24;

// Map a (non-OK) FatFS result to a negative errno. FR_OK must not be passed.
static int FrToErrno(FRESULT res)
{
    switch (res) {
        case FR_NO_FILE:
        case FR_NO_PATH:            return -FVFS_ENOENT;
        case FR_DENIED:
        case FR_EXIST:
        case FR_LOCKED:             return -FVFS_EACCES;
        case FR_WRITE_PROTECTED:    return -FVFS_EROFS;
        case FR_INVALID_NAME:
        case FR_INVALID_OBJECT:
        case FR_INVALID_DRIVE:
        case FR_INVALID_PARAMETER:  return -FVFS_EINVAL;
        case FR_TOO_MANY_OPEN_FILES:return -FVFS_EMFILE;
        // FR_DISK_ERR / FR_INT_ERR / FR_NOT_READY / FR_NO_FILESYSTEM /
        // FR_TIMEOUT and anything else are low-level I/O failures.
        default:                    return -FVFS_EIO;
    }
}

// Gate noisy "f_open failed" messages — off by default to avoid blocking
// serial on the ~100+ failed opens the dynamic linker generates per app.
static bool g_fatfsDebug = false;

struct FatOpenFile;
struct FatFsMountData {
    FATFS fs;
    uint8_t pdrv;
    FatOpenFile* writableFiles;
    FatOpenFile* allFiles;
    int deferredCloseError;
};

// FIL stays first so existing vnode/cache users can retain their FIL pointer.
// Writable records are linked under g_fatLock until close has succeeded.
struct FatOpenFile {
    FIL fil;
    FatFsMountData* mount;
    FatOpenFile* next;
    FatOpenFile* allNext;
    bool cacheLoading;
    bool cacheInvalidated;
    bool writable;
    bool closing;
};

static void RemoveWritableFile(FatOpenFile* file)
{
    auto** link = &file->mount->writableFiles;
    while (*link && *link != file) link = &(*link)->next;
    if (*link) *link = file->next;
}

static void RemoveAllFile(FatOpenFile* file)
{
    auto** link = &file->mount->allFiles;
    while (*link && *link != file) link = &(*link)->allNext;
    if (*link) *link = file->allNext;
}

static bool SameFatFile(const FIL* a, const FIL* b)
{
    return a->obj.fs == b->obj.fs && a->dir_sect == b->dir_sect && a->dir_ptr == b->dir_ptr;
}

static FRESULT CheckedFatSync(FIL* fil, bool closing);

// Flush private sector windows before another FIL mutates the same allocation.
static int PrepareFatMutation(FatOpenFile* file, bool flushSelf = true)
{
    for (auto* other = file->mount->allFiles; other; other = other->allNext) {
        if (!SameFatFile(&file->fil, &other->fil)) continue;
        if (other->cacheLoading) return -16;
        if (other->writable && (flushSelf || other != file)) {
            FRESULT r = CheckedFatSync(&other->fil, false);
            if (r != FR_OK) return FrToErrno(r);
        }
    }
    return 0;
}

static void PublishFatMutation(FatOpenFile* file)
{
    for (auto* other = file->mount->allFiles; other; other = other->allNext) {
        if (!SameFatFile(&file->fil, &other->fil)) continue;
        other->cacheInvalidated = true;
        if (other == file) continue;
        other->fil.obj.objsize = file->fil.obj.objsize;
        other->fil.obj.sclust = file->fil.obj.sclust;
        // Caller flushed all sector windows. Force next operation to seek and
        // reload; logical OFD offsets belong to FdEntry, not this FIL cursor.
        other->fil.fptr = 0;
        other->fil.clust = 0;
        other->fil.sect = 0;
    }
}

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
    if (*offset >= f_size(fil)) { KMutexUnlock(&g_fatLock); return 0; }
    if (f_tell(fil) != *offset) {
        FRESULT sres = f_lseek(fil, static_cast<FSIZE_t>(*offset));
        if (sres != FR_OK) {
            KMutexUnlock(&g_fatLock);
            return FrToErrno(sres);
        }
    }
    UINT br = 0;
    FRESULT res = f_read(fil, buf, static_cast<UINT>(len), &br);
    if (res != FR_OK) { KMutexUnlock(&g_fatLock); return FrToErrno(res); }
    *offset += br;
    KMutexUnlock(&g_fatLock);
    return static_cast<int>(br);
}

static int FatFileWriteAt(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset, bool append)
{
    FIL* fil = static_cast<FIL*>(vn->priv);
    KMutexLock(&g_fatLock);
    auto* file = reinterpret_cast<FatOpenFile*>(fil);
    int ready = PrepareFatMutation(file, false);
    if (ready) { KMutexUnlock(&g_fatLock); return ready; }
    // Alias metadata has been synchronized; EOF and write share g_fatLock.
    uint64_t position = append ? f_size(fil) : *offset;
    if (f_tell(fil) != position) {
        FRESULT sres = f_lseek(fil, static_cast<FSIZE_t>(position));
        if (sres != FR_OK) {
            KMutexUnlock(&g_fatLock);
            return FrToErrno(sres);
        }
    }
    UINT bw = 0;
    FRESULT res = f_write(fil, buf, static_cast<UINT>(len), &bw);
    // Only alias sharing requires an immediate flush. Keep the normal
    // single-writer path buffered; open() flushes pending writers below.
    bool aliases = false;
    for (auto* other = file->mount->allFiles; other; other = other->allNext)
        if (other != file && SameFatFile(fil, &other->fil)) { aliases = true; break; }
    FRESULT sync = aliases ? CheckedFatSync(fil, false) : FR_OK;
    PublishFatMutation(file);
    if (res == FR_OK) res = sync;
    if (res != FR_OK) { KMutexUnlock(&g_fatLock); return FrToErrno(res); }
    if (bw) *offset = position + bw;
    KMutexUnlock(&g_fatLock);
    return static_cast<int>(bw);
}

static int FatFileWrite(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset)
{ return FatFileWriteAt(vn, buf, len, offset, false); }
static int FatFileAppend(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset)
{ return len ? FatFileWriteAt(vn, buf, len, offset, true) : 0; }

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

static FRESULT CheckedFatSync(FIL* fil, bool closing)
{
    // FatFS clears FA_MODIFIED even if sync_fs/CTRL_SYNC fails. Retain its
    // pre-call flags so a retry rewrites dirty data and directory metadata.
    BYTE flags = fil->flag;
    FRESULT result = closing ? f_close(fil) : f_sync(fil);
    if (result != FR_OK) fil->flag |= flags;
    return result;
}

static void CloseFatFileLocked(FIL* fil)
{
    auto* file = reinterpret_cast<FatOpenFile*>(fil);
    FRESULT result = CheckedFatSync(fil, true);
    if (file->writable && result != FR_OK) {
        // The vnode can disappear, but preserve failed dirty FIL state for
        // a later checked mount sync; never silently discard its buffers.
        file->closing = true;
        file->mount->deferredCloseError = FrToErrno(result);
    } else {
        if (file->writable) RemoveWritableFile(file);
        RemoveAllFile(file);
        kfree(file);
    }
}

static void FatFileClose(Vnode* vn)
{
    KMutexLock(&g_fatLock);
    if (vn->type == VnodeType::File) {
        CloseFatFileLocked(static_cast<FIL*>(vn->priv));
    } else {
        DIR* dir = static_cast<DIR*>(vn->priv);
        f_closedir(dir);
        kfree(dir);
    }
    KMutexUnlock(&g_fatLock);
}

static int FatFileFsync(Vnode* vn)
{
    if (vn->type != VnodeType::File) return -FVFS_EINVAL;
    FIL* fil = static_cast<FIL*>(vn->priv);
    KMutexLock(&g_fatLock);
    // f_sync flushes the file's cached window AND rewrites the directory
    // entry (size, first cluster, timestamps) + FAT, so the on-disk file is
    // valid even if the system loses power before f_close.
    FRESULT res = CheckedFatSync(fil, false);
    int error = res == FR_OK ? FatFsFlushDrive(fil->obj.fs->pdrv) : FrToErrno(res);
    KMutexUnlock(&g_fatLock);
    return error;
}

static int FatDirFsync(Vnode* vn)
{
    auto* dir = static_cast<DIR*>(vn->priv);
    KMutexLock(&g_fatLock);
    int result = FatFsFlushDrive(dir->obj.fs->pdrv);
    KMutexUnlock(&g_fatLock);
    return result;
}

static int FatCachedFsync(Vnode* vn)
{
    auto* cached = static_cast<CachedFile*>(vn->priv);
    KMutexLock(&g_fatLock);
    int result = FatFsFlushDrive(cached->fil->obj.fs->pdrv);
    KMutexUnlock(&g_fatLock);
    return result;
}

// FAT has no persistent inode field. For a file, the physical short-name
// directory entry is its exact identity while that entry remains allocated.
// This survives independent opens, empty files, growth and truncation (unlike
// the first data cluster), and uses no path hash or identity registry.
// Rename may relocate the entry and unlink/recreate may reuse it: those are
// explicit FAT identity limitations, not persistent POSIX inode semantics.
// Keep cacheId == 0: stat identity must not opt FAT into the VFS page cache.
static_assert(FF_FS_EXFAT == 0 && FF_FS_READONLY == 0,
              "FAT stat identity requires FAT12/16/32 directory-entry metadata");
static_assert(FF_MIN_SS == 512 && FF_MAX_SS == 512 && sizeof(LBA_t) <= 4,
              "FAT inode encoding assumes 512-byte sectors and 32-bit LBAs");

static uint64_t FatDeviceIdentity(const FATFS* fs)
{
    // Virtual FAT major, distinct from ext2's 254. FatFS drive numbers are
    // unique in the mounted volume table (FF_MULTI_PARTITION is disabled).
    return (253ULL << 8) | (uint64_t{fs->pdrv} + 1);
}

// Caller holds g_fatLock, including while inspecting the shared window base.
static bool FatFileIdentity(const FIL* fil, VnodeStat* st)
{
    const FATFS* fs = fil->obj.fs;
    if (!fs || !fil->dir_ptr || !fil->dir_sect) return false;
    const uintptr_t entry = reinterpret_cast<uintptr_t>(fil->dir_ptr);
    const uintptr_t window = reinterpret_cast<uintptr_t>(fs->win);
    if (entry < window || entry - window >= FF_MAX_SS || ((entry - window) & 31))
        return false;
    st->ino = uint64_t{fil->dir_sect} * (FF_MAX_SS / 32) + (entry - window) / 32 + 1;
    st->dev = FatDeviceIdentity(fs);
    return true;
}

static bool FatDirectoryIdentity(const DIR* dir, VnodeStat* st)
{
    if (!dir->obj.fs) return false;
    // Directory chains have a stable start cluster; FatFS represents the root
    // by zero. Use a separate namespace from all possible file entry numbers.
    st->ino = (1ULL << 62) | (uint64_t{dir->obj.sclust} + 1);
    st->dev = FatDeviceIdentity(dir->obj.fs);
    return true;
}

static int FatStat(Vnode* vn, VnodeStat* st)
{
    KMutexLock(&g_fatLock);
    bool valid;
    if (vn->type == VnodeType::File) {
        const FIL* fil = static_cast<FIL*>(vn->priv);
        st->size = f_size(fil);
        st->isDir = false;
        valid = FatFileIdentity(fil, st);
    } else {
        st->size = 0;
        st->isDir = true;
        valid = FatDirectoryIdentity(static_cast<DIR*>(vn->priv), st);
    }
    st->isSymlink = false;
    KMutexUnlock(&g_fatLock);
    return valid ? 0 : -FVFS_EIO;
}

static int CachedFileRead(Vnode* vn, void* buf, uint64_t len, uint64_t* offset)
{
    auto* cf = static_cast<CachedFile*>(vn->priv);
    KMutexLock(&g_fatLock);
    auto* file = reinterpret_cast<FatOpenFile*>(cf->fil);
    if (file->cacheInvalidated) {
        if (*offset >= f_size(cf->fil)) { KMutexUnlock(&g_fatLock); return 0; }
        FRESULT r = f_lseek(cf->fil, static_cast<FSIZE_t>(*offset));
        UINT count = 0;
        if (r == FR_OK) r = f_read(cf->fil, buf, static_cast<UINT>(len), &count);
        if (r == FR_OK) *offset += count;
        KMutexUnlock(&g_fatLock);
        return r == FR_OK ? static_cast<int>(count) : FrToErrno(r);
    }
    if (*offset >= cf->size) { KMutexUnlock(&g_fatLock); return 0; }
    if (len > cf->size - *offset) len = cf->size - *offset;
    memcpy(buf, cf->data + *offset, len);
    *offset += len;
    KMutexUnlock(&g_fatLock);
    return static_cast<int>(len);
}

static void CachedFileClose(Vnode* vn)
{
    auto* cf = static_cast<CachedFile*>(vn->priv);
    if (!cf) return;
    KMutexLock(&g_fatLock);
    int32_t refs = --cf->refCount;
    if (refs <= 0) { FileCacheRemove(cf); CloseFatFileLocked(cf->fil); }
    KMutexUnlock(&g_fatLock);
    if (refs <= 0) {
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
    KMutexLock(&g_fatLock);
    st->size = f_size(cf->fil);
    st->isDir = false;
    st->isSymlink = false;
    const bool valid = FatFileIdentity(cf->fil, st);
    KMutexUnlock(&g_fatLock);
    return valid ? 0 : -FVFS_EIO;
}

static int FatFileTruncate(Vnode* vn, uint64_t size)
{
    if (size > 0xffffffffULL) return -27;
    auto* file = static_cast<FatOpenFile*>(vn->priv);
    uint8_t* zeros = static_cast<uint8_t*>(kmalloc(4096));
    if (!zeros) return -12;
    memset(zeros, 0, 4096);
    KMutexLock(&g_fatLock);
    int result = PrepareFatMutation(file);
    FIL* fil = &file->fil;
    if (!result) {
        FRESULT r = f_lseek(fil, static_cast<FSIZE_t>(size < f_size(fil) ? size : f_size(fil)));
        if (r == FR_OK && size < f_size(fil)) r = f_truncate(fil);
        while (r == FR_OK && f_size(fil) < size) {
            UINT count = 0;
            UINT chunk = size - f_size(fil) > 4096 ? 4096 : size - f_size(fil);
            r = f_write(fil, zeros, chunk, &count);
            if (r == FR_OK && count != chunk) { result = -28; break; }
        }
        FRESULT sync = CheckedFatSync(fil, false);
        PublishFatMutation(file);
        if (!result && r != FR_OK) result = FrToErrno(r);
        if (!result && sync != FR_OK) result = FrToErrno(sync);
    }
    KMutexUnlock(&g_fatLock);
    kfree(zeros);
    return result;
}

static int FatSpaceFor(FATFS* expected, VfsSpace* out)
{
    if (!expected) return -FVFS_EIO;
    char drive[] = {static_cast<char>('0' + expected->pdrv), ':', 0};
    FATFS* fs = nullptr;
    DWORD freeClusters = 0;
    FRESULT result = f_getfree(drive, &freeClusters, &fs);
    if (result != FR_OK) return FrToErrno(result);
    if (fs != expected || fs->n_fatent < 2 || freeClusters > fs->n_fatent - 2)
        return -FVFS_EIO;
    const uint64_t clusterBytes = uint64_t{fs->csize} * 512;
    *out = {0x4d44, clusterBytes, fs->n_fatent - 2, freeClusters, freeClusters,
            0, 0, FatDeviceIdentity(fs), 255, clusterBytes, 0};
    return 0;
}
static int FatSpace(Vnode* vn, VfsSpace* out)
{
    KMutexLock(&g_fatLock);
    FATFS* fs = vn->type == VnodeType::Dir ? static_cast<DIR*>(vn->priv)->obj.fs
        : static_cast<FIL*>(vn->priv)->obj.fs;
    int result = FatSpaceFor(fs, out);
    KMutexUnlock(&g_fatLock);
    return result;
}
static int FatCachedSpace(Vnode* vn, VfsSpace* out)
{
    KMutexLock(&g_fatLock);
    int result = FatSpaceFor(static_cast<CachedFile*>(vn->priv)->fil->obj.fs, out);
    KMutexUnlock(&g_fatLock);
    return result;
}

static const VnodeOps g_cachedFileOps = {
    .open = FatFileOpen, .read = CachedFileRead, .write = nullptr,
    .readdir = nullptr, .close = CachedFileClose, .stat = CachedFileStat,
    .fsync = FatCachedFsync,
    .space = FatCachedSpace,
};

static const VnodeOps g_fatFileOps = {
    .open = FatFileOpen, .read = FatFileRead, .write = FatFileWrite,
    .readdir = nullptr, .close = FatFileClose, .stat = FatStat,
    .fsync = FatFileFsync,
    .truncate = FatFileTruncate,
    .append = FatFileAppend,
    .space = FatSpace,
};

static const VnodeOps g_fatDirOps = {
    .open = FatFileOpen, .read = nullptr, .write = nullptr,
    .readdir = FatDirReaddir, .close = FatFileClose, .stat = FatStat,
    .fsync = FatDirFsync,
    .space = FatSpace,
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

static int FatFsSyncLocked(FatFsMountData* data)
{
    int result = data->deferredCloseError;
    data->deferredCloseError = 0;
    auto** link = &data->writableFiles;
    while (*link) {
        auto* file = *link;
        FRESULT error = CheckedFatSync(&file->fil, file->closing);
        if (error != FR_OK && !result) result = FrToErrno(error);
        if (file->closing && error == FR_OK) {
            *link = file->next;
            RemoveAllFile(file);
            kfree(file);
        } else {
            link = &file->next;
        }
    }
    int error = FatFsFlushDrive(data->pdrv);
    return result ? result : error;
}

static int FatFsSync(void* mountPriv)
{
    auto* data = static_cast<FatFsMountData*>(mountPriv);
    if (!data) return -FVFS_EINVAL;
    KMutexLock(&g_fatLock);
    int result = FatFsSyncLocked(data);
    KMutexUnlock(&g_fatLock);
    return result;
}

static int FatFsUnmount(void* mountPriv)
{
    auto* data = static_cast<FatFsMountData*>(mountPriv);
    if (!data) return -FVFS_EINVAL;

    char fatPath[8];
    fatPath[0] = static_cast<char>('0' + data->pdrv);
    fatPath[1] = ':'; fatPath[2] = '\0';

    KMutexLock(&g_fatLock);
    int result = FatFsSyncLocked(data);
    if (!result && data->allFiles) result = -16; // EBUSY: live FIL, including mapped readers
    if (!result) {
        FRESULT error = f_mount(nullptr, fatPath, 0);
        if (error != FR_OK) result = FrToErrno(error);
    }
    KMutexUnlock(&g_fatLock);
    if (result) return result;
    kfree(data);
    return 0;
}

static Vnode* FatFsOpen(void* mountPriv, uint8_t pdrv,
                        const char* relPath, int flags)
{
    char fatPath[256];
    BuildFatPath(fatPath, sizeof(fatPath), pdrv, relPath);

    auto* file = static_cast<FatOpenFile*>(kmalloc(sizeof(FatOpenFile)));
    if (!file) return nullptr;
    __builtin_memset(file, 0, sizeof(*file));
    file->mount = static_cast<FatFsMountData*>(mountPriv);
    auto* fil = &file->fil;

    BYTE mode = FA_READ;
    if (flags & VFS_O_WRITE)  mode |= FA_WRITE;
    if (flags & VFS_O_CREATE) mode |= (flags & VFS_O_TRUNC) ? FA_CREATE_ALWAYS : FA_OPEN_ALWAYS;
    if (flags & VFS_O_TRUNC)  mode |= FA_CREATE_ALWAYS;

    // Check shared cache first (read-only opens).
    if (!(flags & VFS_O_METADATA) && !(mode & (FA_WRITE | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS))) {
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
                if (--existing->refCount == 0) kfree(existing);
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
            vn->directoryPath = nullptr;
            vn->ops = &g_cachedFileOps; vn->type = VnodeType::File;
            vn->priv = existing; vn->refCount = 1; vn->cacheId = 0;
            return vn;
        }
        KMutexUnlock(&g_fatLock);
    }

    KMutexLock(&g_fatLock);
    // FatFS keeps size/first-cluster metadata in each FIL. Publish pending
    // writers before opening another FIL, then all mutations update aliases.
    for (auto* writer = file->mount->writableFiles; writer; writer = writer->next) {
        FRESULT sync = CheckedFatSync(&writer->fil, false);
        if (sync != FR_OK) { KMutexUnlock(&g_fatLock); kfree(file); return nullptr; }
    }
    FRESULT res = f_open(fil, fatPath, mode);
    if (res != FR_OK) {
        if (g_fatfsDebug)
            SerialPrintf("FATFS: f_open('%s', 0x%x) failed: %d\n", fatPath, mode, res);
        KMutexUnlock(&g_fatLock);
    }
    if (res == FR_OK) {
        file->allNext = file->mount->allFiles;
        file->mount->allFiles = file;
        if (mode & (FA_WRITE | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS)) {
            file->writable = true;
            file->next = file->mount->writableFiles;
            file->mount->writableFiles = file;
        }
        auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!vn) { CloseFatFileLocked(fil); KMutexUnlock(&g_fatLock); return nullptr; }

        static constexpr uint64_t CACHE_THRESHOLD = 64 * 1024;
        uint64_t fileSize = f_size(fil);
        if (!(flags & VFS_O_METADATA) && !file->writable && fileSize >= CACHE_THRESHOLD) {
            file->cacheLoading = true;
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
            KMutexLock(&g_fatLock);
            file->cacheLoading = false;
            KMutexUnlock(&g_fatLock);
            if (cf && placeholder) {
                placeholder->data = cf->data;
                placeholder->size = cf->size;
                __atomic_store_n(&placeholder->loading, false, __ATOMIC_RELEASE);
                cf->data = nullptr;
                kfree(cf);
                vn->directoryPath = nullptr;
                vn->ops = &g_cachedFileOps; vn->type = VnodeType::File;
                vn->priv = placeholder; vn->refCount = 1; vn->cacheId = 0;
                return vn;
            }
            if (placeholder) {
                KMutexLock(&g_fatLock);
                FileCacheRemove(placeholder);
                __atomic_store_n(&placeholder->loading, false, __ATOMIC_RELEASE);
                if (--placeholder->refCount == 0) kfree(placeholder);
                KMutexUnlock(&g_fatLock);
            }
            if (cf) {
                KMutexLock(&g_fatLock);
                FileCacheInsert(cf);
                KMutexUnlock(&g_fatLock);
                vn->directoryPath = nullptr;
                vn->ops = &g_cachedFileOps; vn->type = VnodeType::File;
                vn->priv = cf; vn->refCount = 1; vn->cacheId = 0;
                return vn;
            }
        } else {
            KMutexUnlock(&g_fatLock);
        }

        vn->directoryPath = nullptr;
        vn->ops = &g_fatFileOps; vn->type = VnodeType::File;
        vn->priv = fil; vn->refCount = 1; vn->cacheId = 0;

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
        vn->directoryPath = nullptr;
        vn->ops = &g_fatDirOps; vn->type = VnodeType::Dir;
        vn->priv = dir; vn->refCount = 1; vn->cacheId = 0;
        return vn;
    }
    kfree(dir);
    return nullptr;
}

static int FatFsStatPath(void* /*mountPriv*/, uint8_t pdrv,
                         const char* relPath, VnodeStat* st)
{
    // Guard: st must be a canonical kernel-half pointer.
    // BRO-051 showed callee-saved reg corruption setting st to 0x46.
    uint64_t stVal = reinterpret_cast<uint64_t>(st);
    if (stVal < 0xFFFF800000000000ULL)
    {
        SerialPrintf("FATFS: FatFsStatPath st=%p (corrupt!) pdrv=%u path='%s' "
                     "caller=%p\n",
                     st, pdrv, relPath ? relPath : "(null)",
                     __builtin_return_address(0));
        return -1;
    }

    bool isRoot = (relPath[0] == '/' && relPath[1] == '\0') || relPath[0] == '\0';

    char fatPath[256];
    BuildFatPath(fatPath, sizeof(fatPath), pdrv, relPath);

    FILINFO fno = {};
    VnodeStat identity = {};
    KMutexLock(&g_fatLock);
    FRESULT res = isRoot ? FR_OK : f_stat(fatPath, &fno);
    bool valid = false;
    if (res == FR_OK) {
        if (isRoot || (fno.fattrib & AM_DIR)) {
            DIR dir = {};
            res = f_opendir(&dir, fatPath);
            if (res == FR_OK) {
                valid = FatDirectoryIdentity(&dir, &identity);
                res = f_closedir(&dir);
            }
        } else {
            // f_stat's FILINFO omits the entry location. A read-only open
            // exposes that exact location without changing file contents;
            // keep stat/open/close under the same namespace lock.
            FIL fil = {};
            res = f_open(&fil, fatPath, FA_READ);
            if (res == FR_OK) {
                valid = FatFileIdentity(&fil, &identity);
                res = f_close(&fil);
            }
        }
    }
    KMutexUnlock(&g_fatLock);
    if (res != FR_OK) return FrToErrno(res);
    if (!valid) return -FVFS_EIO;

    // Recheck st after lock round-trip — catches SMP register corruption.
    stVal = reinterpret_cast<uint64_t>(st);
    if (stVal < 0xFFFF800000000000ULL)
    {
        SerialPrintf("FATFS: FatFsStatPath st=%p CORRUPTED after f_stat! "
                     "pdrv=%u path='%s' fattrib=0x%x caller=%p\n",
                     st, pdrv, relPath ? relPath : "(null)",
                     fno.fattrib, __builtin_return_address(0));
        return -1;
    }

    st->isDir = isRoot || (fno.fattrib & AM_DIR) != 0;
    st->ino = identity.ino;
    st->dev = identity.dev;
    st->size  = st->isDir ? 0 : fno.fsize;
    st->isSymlink = false;
    return 0;
}

// FatFS does not preserve unlinked or renamed open FILs (FF_FS_LOCK=0).
// Reject moving their directory slots, including handles not mapped yet.
static bool FatPathOpenLocked(FatFsMountData* mount, const char* path)
{
    FILINFO info;
    if (f_stat(path, &info) != FR_OK) return false;
    if (info.fattrib & AM_DIR) {
        if (!mount->allFiles) return false;
        // Open FILs prevent moving their directory slots. An empty directory
        // has no such slots: unrelated fonts/logs must not block its removal.
        // Nonempty directories retain the conservative guard until descendant
        // handle tracking is implemented. Keep errors conservative as well.
        DIR directory{};
        if (f_opendir(&directory, path) != FR_OK) return true;
        bool busy = false;
        for (;;) {
            FILINFO child{};
            if (f_readdir(&directory, &child) != FR_OK) { busy = true; break; }
            if (!child.fname[0]) break;
            bool dot = child.fname[0] == '.' && !child.fname[1];
            bool dotdot = child.fname[0] == '.' && child.fname[1] == '.' && !child.fname[2];
            if (!dot && !dotdot) { busy = true; break; }
        }
        if (f_closedir(&directory) != FR_OK) busy = true;
        return busy;
    }
    FIL lookup;
    if (f_open(&lookup, path, FA_READ) != FR_OK) return true;
    bool busy = false;
    for (auto* file = mount->allFiles; file; file = file->allNext)
        if (SameFatFile(&lookup, &file->fil)) { busy = true; break; }
    f_close(&lookup);
    return busy;
}

static int FatFsUnlink(void* mountPriv, uint8_t pdrv, const char* relPath)
{
    char fatPath[256];
    BuildFatPath(fatPath, sizeof(fatPath), pdrv, relPath);
    KMutexLock(&g_fatLock);
    if (FatPathOpenLocked(static_cast<FatFsMountData*>(mountPriv), fatPath)) {
        KMutexUnlock(&g_fatLock); return -16;
    }
    FRESULT res = f_unlink(fatPath);
    KMutexUnlock(&g_fatLock);
    return res == FR_OK ? 0 : FrToErrno(res);
}

static int FatFsMkdir(void* /*mountPriv*/, uint8_t pdrv, const char* relPath, uint16_t /*mode*/)
{
    char fatPath[256];
    BuildFatPath(fatPath, sizeof(fatPath), pdrv, relPath);
    KMutexLock(&g_fatLock);
    FRESULT res = f_mkdir(fatPath);
    KMutexUnlock(&g_fatLock);
    return res == FR_OK ? 0 : (res == FR_EXIST ? -17 : FrToErrno(res));
}

static int FatFsRename(void* mountPriv, uint8_t pdrv,
                       const char* oldRelPath, const char* newRelPath)
{
    char fatOld[256], fatNew[256];
    BuildFatPath(fatOld, sizeof(fatOld), pdrv, oldRelPath);
    BuildFatPath(fatNew, sizeof(fatNew), pdrv, newRelPath);
    KMutexLock(&g_fatLock);
    auto* mount = static_cast<FatFsMountData*>(mountPriv);
    if (FatPathOpenLocked(mount, fatOld) || FatPathOpenLocked(mount, fatNew)) {
        KMutexUnlock(&g_fatLock); return -16;
    }
    // POSIX rename atomically replaces destination; FatFS f_rename fails if
    // destination exists.  Delete the target first (ignore errors — it may
    // not exist).
    f_unlink(fatNew);
    FRESULT res = f_rename(fatOld, fatNew);
    KMutexUnlock(&g_fatLock);
    return res == FR_OK ? 0 : FrToErrno(res);
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
    .sync       = FatFsSync,
};

// ---- Public init ----

void FatFsVfsRegister()
{
    EnsureLock();
    VfsRegisterFs("fatfs", &g_fatFsOps);
}

} // namespace brook

extern "C" void FatFsDumpLockState()
{
    brook::SerialPrintf("  fatLock: locked=%u owner=%u waitHead=%p\n",
                        brook::g_fatLock.locked, brook::g_fatLock.ownerPid,
                        brook::g_fatLock.waitHead);
    // BRO-196: walk the FAT-lock FIFO wait queue (pids blocked acquiring it).
    int n = 0;
    for (brook::Process* w = brook::g_fatLock.waitHead; w && n < 64; w = w->syncNext, ++n) {
        if (w->magic != brook::PROCESS_MAGIC) { brook::SerialPrintf("    waiter[CORRUPT]\n"); break; }
        brook::SerialPrintf("    fatLock waiter pid=%u state=%d syscall=%lu pendWake=%d\n",
                            w->pid, (int)w->state, w->currentSyscallNum,
                            __atomic_load_n(&w->pendingWakeup, __ATOMIC_RELAXED));
    }
}

// BRO-196 diagnostic: current owner pid of g_fatLock (0 = unheld). Lets the
// generic syscall layer detect a thread doing a blocking op while holding the
// global FAT lock (a lock leak across a blocking wait).
extern "C" uint32_t FatFsLockOwner()
{
    return brook::g_fatLock.locked ? brook::g_fatLock.ownerPid : 0;
}
