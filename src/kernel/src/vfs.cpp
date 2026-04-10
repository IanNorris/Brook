#include "vfs.h"
#include "fatfs_glue.h"
#include "heap.h"
#include "serial.h"

extern "C" {
#include "ff.h"
}

namespace brook {

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

static int FatFileOpen(Vnode* vn, int flags)
{
    (void)vn; (void)flags;
    return 0; // Already open by VfsOpen()
}

static int FatFileRead(Vnode* vn, void* buf, uint64_t len, uint64_t* offset)
{
    FIL* fil = static_cast<FIL*>(vn->priv);

    // Seek if needed.
    if (f_tell(fil) != *offset)
    {
        if (f_lseek(fil, static_cast<FSIZE_t>(*offset)) != FR_OK)
            return -1;
    }

    UINT br = 0;
    FRESULT res = f_read(fil, buf, static_cast<UINT>(len), &br);
    if (res != FR_OK) return -1;
    *offset += br;
    return static_cast<int>(br);
}

static int FatFileWrite(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset)
{
    FIL* fil = static_cast<FIL*>(vn->priv);
    if (f_tell(fil) != *offset) {
        if (f_lseek(fil, static_cast<FSIZE_t>(*offset)) != FR_OK) return -1;
    }
    UINT bw = 0;
    FRESULT res = f_write(fil, buf, static_cast<UINT>(len), &bw);
    if (res != FR_OK) return -1;
    *offset += bw;
    return static_cast<int>(bw);
}

static int FatDirReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie)
{
    DIR* dir = static_cast<DIR*>(vn->priv);
    (void)cookie; // FatFS DIR maintains its own iterator state

    FILINFO fno;
    FRESULT res = f_readdir(dir, &fno);
    if (res != FR_OK) { SerialPrintf("VFS: readdir failed (res=%u)\n", static_cast<unsigned>(res)); return -1; }
    if (fno.fname[0] == '\0') return 0; // end of directory

    SerialPrintf("VFS: readdir entry: '%s' (%s, %lu bytes)\n",
                 fno.fname, (fno.fattrib & AM_DIR) ? "dir" : "file",
                 static_cast<unsigned long>(fno.fsize));
    StrCopy(out->name, fno.fname, sizeof(out->name));
    out->size  = fno.fsize;
    out->isDir = (fno.fattrib & AM_DIR) != 0;
    return 1;
}

static void FatFileClose(Vnode* vn)
{
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

    FRESULT res = f_mount(fs, fatPath, 1);
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

    SerialPrintf("VFS: mounted fatfs drive %u at '%s'\n", pdrv, mountPoint);
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
        f_unmount(fatPath);
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

    BYTE mode = (flags & 1) ? FA_READ | FA_WRITE : FA_READ;
    FRESULT res = f_open(fil, fatPath, mode);
    if (res == FR_OK)
    {
        auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!vn) { f_close(fil); kfree(fil); return nullptr; }
        vn->ops  = &g_fatFileOps;
        vn->type = VnodeType::File;
        vn->priv = fil;
        return vn;
    }
    kfree(fil);

    // Try as a directory.
    auto* dir = static_cast<DIR*>(kmalloc(sizeof(DIR)));
    if (!dir) return nullptr;

    res = f_opendir(dir, fatPath);
    if (res == FR_OK)
    {
        SerialPrintf("VFS: opened dir '%s' (fatpath='%s')\n", path, fatPath);
        auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!vn) { f_closedir(dir); kfree(dir); return nullptr; }
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

void VfsClose(Vnode* vn)
{
    if (!vn) return;
    if (vn->ops && vn->ops->close) vn->ops->close(vn);
    kfree(vn);
}

} // namespace brook
