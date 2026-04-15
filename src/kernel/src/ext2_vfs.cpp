// ext2_vfs.cpp — Read-only ext2 filesystem driver for Brook VFS.
//
// Implements: superblock parsing, inode lookup, block mapping (direct/indirect/
// doubly-indirect/triply-indirect), directory traversal, symlink resolution,
// and the VfsFsOps vtable. Write support to follow in a later commit.

#include "ext2_vfs.h"
#include "vfs.h"
#include "device.h"
#include "memory/heap.h"
#include "serial.h"
#include "sync/kmutex.h"

namespace brook {

// ---------------------------------------------------------------------------
// On-disk structures (Linux ext2 format)
// ---------------------------------------------------------------------------

struct Ext2Superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;       // block size = 1024 << this
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;                // 0xEF53
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // EXT2_DYNAMIC_REV fields
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    // ... more fields we don't need for read-only
};

static_assert(sizeof(Ext2Superblock) <= 1024, "Ext2Superblock too large");

static constexpr uint16_t EXT2_SUPER_MAGIC = 0xEF53;
static constexpr uint32_t EXT2_ROOT_INO    = 2;

// Inode type flags (i_mode)
static constexpr uint16_t EXT2_S_IFMT   = 0xF000;
static constexpr uint16_t EXT2_S_IFREG  = 0x8000;
static constexpr uint16_t EXT2_S_IFDIR  = 0x4000;
static constexpr uint16_t EXT2_S_IFLNK  = 0xA000;

struct Ext2BlockGroupDesc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
};

static_assert(sizeof(Ext2BlockGroupDesc) == 32, "Ext2BlockGroupDesc size mismatch");

struct Ext2Inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;         // lower 32 bits of size
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;       // 512-byte block count
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];    // 0-11 direct, 12 indirect, 13 double, 14 triple
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;      // upper 32 bits of size for regular files (ext2 rev1)
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
};

static_assert(sizeof(Ext2Inode) == 128, "Ext2Inode size mismatch");

struct Ext2DirEntry2 {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[]; // variable-length, NOT null-terminated on disk
};

// file_type values (used in readdir)
static constexpr uint8_t EXT2_FT_DIR      = 2;

// ---------------------------------------------------------------------------
// Mount-private state
// ---------------------------------------------------------------------------

struct Ext2Mount {
    Device*  dev;
    uint32_t blockSize;      // bytes per block
    uint32_t blockShift;     // log2(blockSize)
    uint32_t inodeSize;      // bytes per inode on disk
    uint32_t inodesPerGroup;
    uint32_t blocksPerGroup;
    uint32_t groupCount;
    uint32_t firstDataBlock;
    Ext2BlockGroupDesc* bgdt; // block group descriptor table (heap allocated)
};

// Per-open-file state
struct Ext2FilePriv {
    Ext2Mount* mnt;
    uint32_t   inode;
    Ext2Inode  inodeData;
};

// Per-open-dir state
struct Ext2DirPriv {
    Ext2Mount* mnt;
    uint32_t   inode;
    Ext2Inode  inodeData;
    uint32_t   readOffset; // byte offset into directory data
};

static KMutex g_ext2Lock;
static bool   g_ext2LockInit = false;

static void EnsureLock()
{
    if (!g_ext2LockInit) { KMutexInit(&g_ext2Lock); g_ext2LockInit = true; }
}

// ---------------------------------------------------------------------------
// Device I/O helpers
// ---------------------------------------------------------------------------

// Read exactly `len` bytes from device at `byteOffset`.
static bool Ext2DevRead(Ext2Mount* mnt, uint64_t byteOffset, void* buf, uint64_t len)
{
    int r = mnt->dev->ops->read(mnt->dev, byteOffset, buf, len);
    return r == static_cast<int>(len);
}

// Read a single block into buf.
static bool Ext2ReadBlock(Ext2Mount* mnt, uint32_t blockNum, void* buf)
{
    if (blockNum == 0) return false;
    uint64_t off = static_cast<uint64_t>(blockNum) << mnt->blockShift;
    return Ext2DevRead(mnt, off, buf, mnt->blockSize);
}

// ---------------------------------------------------------------------------
// Inode operations
// ---------------------------------------------------------------------------

static bool Ext2ReadInode(Ext2Mount* mnt, uint32_t ino, Ext2Inode* out)
{
    if (ino == 0) return false;
    uint32_t group = (ino - 1) / mnt->inodesPerGroup;
    uint32_t index = (ino - 1) % mnt->inodesPerGroup;
    if (group >= mnt->groupCount) return false;

    uint64_t tableOff = static_cast<uint64_t>(mnt->bgdt[group].bg_inode_table)
                        << mnt->blockShift;
    uint64_t inodeOff = tableOff + static_cast<uint64_t>(index) * mnt->inodeSize;
    return Ext2DevRead(mnt, inodeOff, out, sizeof(Ext2Inode));
}

static uint64_t Ext2InodeSize(const Ext2Inode* ino)
{
    return ino->i_size; // For files >4GB we'd use i_dir_acl, but read-only ext2 is fine with 32-bit
}

// Resolve a file-relative block index to a disk block number.
// Handles direct, indirect, doubly-indirect, and triply-indirect blocks.
static uint32_t Ext2BlockMap(Ext2Mount* mnt, const Ext2Inode* ino, uint32_t fileBlock)
{
    uint32_t ptrsPerBlock = mnt->blockSize / 4;

    // Direct blocks (0..11)
    if (fileBlock < 12)
        return ino->i_block[fileBlock];

    fileBlock -= 12;

    // Singly indirect (block 12)
    if (fileBlock < ptrsPerBlock) {
        uint32_t indBlock = ino->i_block[12];
        if (!indBlock) return 0;
        uint32_t entry = 0;
        uint64_t off = (static_cast<uint64_t>(indBlock) << mnt->blockShift)
                       + fileBlock * 4;
        if (!Ext2DevRead(mnt, off, &entry, 4)) return 0;
        return entry;
    }
    fileBlock -= ptrsPerBlock;

    // Doubly indirect (block 13)
    if (fileBlock < ptrsPerBlock * ptrsPerBlock) {
        uint32_t dindBlock = ino->i_block[13];
        if (!dindBlock) return 0;
        uint32_t idx1 = fileBlock / ptrsPerBlock;
        uint32_t idx2 = fileBlock % ptrsPerBlock;
        uint32_t indBlock = 0;
        uint64_t off1 = (static_cast<uint64_t>(dindBlock) << mnt->blockShift) + idx1 * 4;
        if (!Ext2DevRead(mnt, off1, &indBlock, 4) || !indBlock) return 0;
        uint32_t entry = 0;
        uint64_t off2 = (static_cast<uint64_t>(indBlock) << mnt->blockShift) + idx2 * 4;
        if (!Ext2DevRead(mnt, off2, &entry, 4)) return 0;
        return entry;
    }
    fileBlock -= ptrsPerBlock * ptrsPerBlock;

    // Triply indirect (block 14)
    uint32_t tindBlock = ino->i_block[14];
    if (!tindBlock) return 0;
    uint32_t idx1 = fileBlock / (ptrsPerBlock * ptrsPerBlock);
    uint32_t rem  = fileBlock % (ptrsPerBlock * ptrsPerBlock);
    uint32_t idx2 = rem / ptrsPerBlock;
    uint32_t idx3 = rem % ptrsPerBlock;
    uint32_t dindB = 0;
    uint64_t o1 = (static_cast<uint64_t>(tindBlock) << mnt->blockShift) + idx1 * 4;
    if (!Ext2DevRead(mnt, o1, &dindB, 4) || !dindB) return 0;
    uint32_t indB = 0;
    uint64_t o2 = (static_cast<uint64_t>(dindB) << mnt->blockShift) + idx2 * 4;
    if (!Ext2DevRead(mnt, o2, &indB, 4) || !indB) return 0;
    uint32_t entry = 0;
    uint64_t o3 = (static_cast<uint64_t>(indB) << mnt->blockShift) + idx3 * 4;
    if (!Ext2DevRead(mnt, o3, &entry, 4)) return 0;
    return entry;
}

// Read `len` bytes from inode data at `offset`. Returns bytes read.
static int Ext2ReadInodeData(Ext2Mount* mnt, const Ext2Inode* ino,
                             void* buf, uint64_t len, uint64_t offset)
{
    uint64_t fileSize = Ext2InodeSize(ino);
    if (offset >= fileSize) return 0;
    if (offset + len > fileSize) len = fileSize - offset;
    if (len == 0) return 0;

    auto* dst = static_cast<uint8_t*>(buf);
    uint64_t bytesRead = 0;

    // Temporary block buffer
    auto* blockBuf = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!blockBuf) return -1;

    while (bytesRead < len) {
        uint32_t fileBlock = static_cast<uint32_t>((offset + bytesRead) >> mnt->blockShift);
        uint32_t blockOff  = static_cast<uint32_t>((offset + bytesRead) & (mnt->blockSize - 1));
        uint32_t diskBlock = Ext2BlockMap(mnt, ino, fileBlock);
        if (!diskBlock) break; // sparse hole or error

        if (!Ext2ReadBlock(mnt, diskBlock, blockBuf)) break;

        uint32_t avail = mnt->blockSize - blockOff;
        uint64_t toCopy = len - bytesRead;
        if (toCopy > avail) toCopy = avail;

        for (uint64_t i = 0; i < toCopy; ++i)
            dst[bytesRead + i] = blockBuf[blockOff + i];
        bytesRead += toCopy;
    }

    kfree(blockBuf);
    return static_cast<int>(bytesRead);
}

// ---------------------------------------------------------------------------
// Path resolution (with symlink following)
// ---------------------------------------------------------------------------

// Look up a name in a directory inode. Returns the inode number or 0 on failure.
static uint32_t Ext2DirLookup(Ext2Mount* mnt, const Ext2Inode* dirIno, const char* name)
{
    uint64_t dirSize = Ext2InodeSize(dirIno);
    if (dirSize == 0) return 0;

    uint32_t nameLen = 0;
    for (const char* p = name; *p; ++p) ++nameLen;

    auto* buf = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!buf) return 0;

    uint64_t off = 0;
    while (off < dirSize) {
        uint32_t fileBlock = static_cast<uint32_t>(off >> mnt->blockShift);
        uint32_t diskBlock = Ext2BlockMap(mnt, dirIno, fileBlock);
        if (!diskBlock || !Ext2ReadBlock(mnt, diskBlock, buf)) break;

        uint32_t pos = 0;
        while (pos < mnt->blockSize && off + pos < dirSize) {
            auto* de = reinterpret_cast<Ext2DirEntry2*>(buf + pos);
            if (de->rec_len == 0) break; // corrupt
            if (de->inode != 0 && de->name_len == nameLen) {
                bool match = true;
                for (uint32_t i = 0; i < nameLen; ++i) {
                    if (de->name[i] != name[i]) { match = false; break; }
                }
                if (match) {
                    uint32_t result = de->inode;
                    kfree(buf);
                    return result;
                }
            }
            pos += de->rec_len;
        }
        off += mnt->blockSize;
    }
    kfree(buf);
    return 0;
}

// Read symlink target. Returns heap-allocated string or nullptr.
static char* Ext2ReadSymlink(Ext2Mount* mnt, const Ext2Inode* ino)
{
    uint64_t size = Ext2InodeSize(ino);
    if (size == 0 || size > 4096) return nullptr;

    auto* target = static_cast<char*>(kmalloc(size + 1));
    if (!target) return nullptr;

    // Fast symlinks: if size <= 60 and no data blocks allocated, target is in i_block[]
    if (size <= 60 && ino->i_blocks == 0) {
        auto* src = reinterpret_cast<const char*>(ino->i_block);
        for (uint64_t i = 0; i < size; ++i) target[i] = src[i];
        target[size] = '\0';
        return target;
    }

    // Slow symlink: target stored in data blocks
    int r = Ext2ReadInodeData(mnt, ino, target, size, 0);
    if (r <= 0) { kfree(target); return nullptr; }
    target[r] = '\0';
    return target;
}

// Resolve a path to an inode number, following symlinks (up to depth limit).
// path must be relative (no leading slash); resolution starts from startIno.
static uint32_t Ext2ResolvePath(Ext2Mount* mnt, uint32_t startIno,
                                const char* path, int symlinkDepth);

static uint32_t Ext2ResolvePathInternal(Ext2Mount* mnt, uint32_t startIno,
                                        const char* path, int symlinkDepth)
{
    if (symlinkDepth > 8) return 0; // symlink loop protection

    uint32_t curIno = startIno;

    // Skip leading slashes (absolute path restarts from root)
    while (*path == '/') { ++path; curIno = EXT2_ROOT_INO; }
    if (*path == '\0') return curIno;

    // Extract next path component
    char component[256];
    uint32_t ci = 0;
    while (*path && *path != '/' && ci < sizeof(component) - 1)
        component[ci++] = *path++;
    component[ci] = '\0';
    while (*path == '/') ++path; // skip trailing slashes

    // Look up component in current directory
    Ext2Inode dirIno;
    if (!Ext2ReadInode(mnt, curIno, &dirIno)) return 0;
    if ((dirIno.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return 0;

    uint32_t childIno = Ext2DirLookup(mnt, &dirIno, component);
    if (!childIno) return 0;

    // Check if child is a symlink — follow it
    Ext2Inode childData;
    if (!Ext2ReadInode(mnt, childIno, &childData)) return 0;

    if ((childData.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
        char* target = Ext2ReadSymlink(mnt, &childData);
        if (!target) return 0;

        // Resolve symlink target
        uint32_t base = curIno;
        if (target[0] == '/') base = EXT2_ROOT_INO;

        // Build combined path: target + remaining path
        uint32_t targetLen = 0;
        for (const char* p = target; *p; ++p) ++targetLen;
        uint32_t remainLen = 0;
        for (const char* p = path; *p; ++p) ++remainLen;

        char* combined = nullptr;
        if (remainLen > 0) {
            combined = static_cast<char*>(kmalloc(targetLen + 1 + remainLen + 1));
            if (!combined) { kfree(target); return 0; }
            uint32_t wi = 0;
            for (uint32_t i = 0; i < targetLen; ++i) combined[wi++] = target[i];
            combined[wi++] = '/';
            for (uint32_t i = 0; i < remainLen; ++i) combined[wi++] = path[i];
            combined[wi] = '\0';
            kfree(target);
            uint32_t result = Ext2ResolvePathInternal(mnt, base, combined, symlinkDepth + 1);
            kfree(combined);
            return result;
        } else {
            uint32_t result = Ext2ResolvePathInternal(mnt, base, target, symlinkDepth + 1);
            kfree(target);
            return result;
        }
    }

    // Not a symlink — if more path remains, recurse
    if (*path != '\0')
        return Ext2ResolvePathInternal(mnt, childIno, path, symlinkDepth);

    return childIno;
}

static uint32_t Ext2ResolvePath(Ext2Mount* mnt, uint32_t startIno,
                                const char* path, int symlinkDepth)
{
    return Ext2ResolvePathInternal(mnt, startIno, path, symlinkDepth);
}

// ---------------------------------------------------------------------------
// Vnode operations
// ---------------------------------------------------------------------------

static int Ext2FileOpen(Vnode* vn, int flags) { (void)vn; (void)flags; return 0; }

static int Ext2FileRead(Vnode* vn, void* buf, uint64_t len, uint64_t* offset)
{
    auto* fp = static_cast<Ext2FilePriv*>(vn->priv);
    KMutexLock(&g_ext2Lock);
    int r = Ext2ReadInodeData(fp->mnt, &fp->inodeData, buf, len, *offset);
    KMutexUnlock(&g_ext2Lock);
    if (r > 0) *offset += r;
    return r;
}

static void Ext2FileClose(Vnode* vn)
{
    auto* fp = static_cast<Ext2FilePriv*>(vn->priv);
    kfree(fp);
}

static int Ext2FileStat(Vnode* vn, VnodeStat* st)
{
    auto* fp = static_cast<Ext2FilePriv*>(vn->priv);
    st->size  = Ext2InodeSize(&fp->inodeData);
    st->isDir = false;
    return 0;
}

static int Ext2DirReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie)
{
    auto* dp = static_cast<Ext2DirPriv*>(vn->priv);
    uint64_t dirSize = Ext2InodeSize(&dp->inodeData);
    (void)cookie;

    KMutexLock(&g_ext2Lock);

    while (dp->readOffset < dirSize) {
        // Read directory entry at current offset
        uint8_t entBuf[264]; // max dir entry: 8 + 255 name
        int r = Ext2ReadInodeData(dp->mnt, &dp->inodeData, entBuf,
                                  sizeof(entBuf), dp->readOffset);
        if (r < 8) { KMutexUnlock(&g_ext2Lock); return 0; } // end or error

        auto* de = reinterpret_cast<Ext2DirEntry2*>(entBuf);
        if (de->rec_len == 0) { KMutexUnlock(&g_ext2Lock); return 0; }

        dp->readOffset += de->rec_len;

        if (de->inode == 0) continue; // deleted entry
        // Skip . and ..
        if (de->name_len == 1 && de->name[0] == '.') continue;
        if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') continue;

        // Fill output
        uint32_t copyLen = de->name_len;
        if (copyLen >= sizeof(out->name)) copyLen = sizeof(out->name) - 1;
        for (uint32_t i = 0; i < copyLen; ++i) out->name[i] = de->name[i];
        out->name[copyLen] = '\0';

        out->isDir = (de->file_type == EXT2_FT_DIR);

        // Get file size from inode
        if (de->file_type == EXT2_FT_DIR) {
            out->size = 0;
        } else {
            Ext2Inode childIno;
            if (Ext2ReadInode(dp->mnt, de->inode, &childIno))
                out->size = Ext2InodeSize(&childIno);
            else
                out->size = 0;
        }

        KMutexUnlock(&g_ext2Lock);
        return 1;
    }

    KMutexUnlock(&g_ext2Lock);
    return 0;
}

static void Ext2DirClose(Vnode* vn)
{
    auto* dp = static_cast<Ext2DirPriv*>(vn->priv);
    kfree(dp);
}

static int Ext2DirStat(Vnode* vn, VnodeStat* st)
{
    (void)vn;
    st->size  = 0;
    st->isDir = true;
    return 0;
}

static const VnodeOps g_ext2FileOps = {
    .open    = Ext2FileOpen,
    .read    = Ext2FileRead,
    .write   = nullptr,    // read-only for now
    .readdir = nullptr,
    .close   = Ext2FileClose,
    .stat    = Ext2FileStat,
};

static const VnodeOps g_ext2DirOps = {
    .open    = Ext2FileOpen,
    .read    = nullptr,
    .write   = nullptr,
    .readdir = Ext2DirReaddir,
    .close   = Ext2DirClose,
    .stat    = Ext2DirStat,
};

// ---------------------------------------------------------------------------
// VfsFsOps callbacks
// ---------------------------------------------------------------------------

// Device binding table (similar to FatFS pdrv concept)
static constexpr uint8_t EXT2_MAX_MOUNTS = 4;
static Device* g_ext2Devices[EXT2_MAX_MOUNTS] = {};

static bool Ext2FsMount(uint8_t pdrv, void** mountPriv)
{
    EnsureLock();
    if (pdrv >= EXT2_MAX_MOUNTS || !g_ext2Devices[pdrv]) {
        SerialPrintf("ext2: no device bound for pdrv %u\n", pdrv);
        return false;
    }

    Device* dev = g_ext2Devices[pdrv];

    // Read superblock at byte offset 1024
    Ext2Superblock sb;
    KMutexLock(&g_ext2Lock);
    int r = dev->ops->read(dev, 1024, &sb, sizeof(sb));
    KMutexUnlock(&g_ext2Lock);
    if (r != sizeof(sb)) {
        SerialPrintf("ext2: failed to read superblock from %s\n", dev->name);
        return false;
    }

    if (sb.s_magic != EXT2_SUPER_MAGIC) {
        SerialPrintf("ext2: bad magic 0x%x (expected 0xEF53) on %s\n",
                     sb.s_magic, dev->name);
        return false;
    }

    uint32_t blockSize = 1024u << sb.s_log_block_size;
    uint32_t blockShift = 10 + sb.s_log_block_size;

    // Compute group count
    uint32_t groupCount = (sb.s_blocks_count + sb.s_blocks_per_group - 1)
                          / sb.s_blocks_per_group;

    // Inode size: rev 0 = 128, rev 1+ = s_inode_size
    uint32_t inodeSize = (sb.s_rev_level >= 1) ? sb.s_inode_size : 128;

    SerialPrintf("ext2: %s — %u blocks, %u inodes, %u block size, %u groups\n",
                 dev->name, sb.s_blocks_count, sb.s_inodes_count, blockSize, groupCount);

    // Read block group descriptor table
    // Starts at the block after the superblock (block 1 for 1K blocks, or byte offset blockSize for 2K+)
    uint64_t bgdtOff;
    if (blockSize == 1024)
        bgdtOff = 2048; // block 2 for 1K block sizes
    else
        bgdtOff = blockSize; // block 1 for 2K+ block sizes

    uint32_t bgdtSize = groupCount * sizeof(Ext2BlockGroupDesc);
    auto* bgdt = static_cast<Ext2BlockGroupDesc*>(kmalloc(bgdtSize));
    if (!bgdt) { SerialPuts("ext2: BGDT alloc failed\n"); return false; }

    KMutexLock(&g_ext2Lock);
    r = dev->ops->read(dev, bgdtOff, bgdt, bgdtSize);
    KMutexUnlock(&g_ext2Lock);
    if (r != static_cast<int>(bgdtSize)) {
        SerialPuts("ext2: failed to read BGDT\n");
        kfree(bgdt);
        return false;
    }

    auto* mnt = static_cast<Ext2Mount*>(kmalloc(sizeof(Ext2Mount)));
    if (!mnt) { kfree(bgdt); return false; }

    mnt->dev            = dev;
    mnt->blockSize      = blockSize;
    mnt->blockShift     = blockShift;
    mnt->inodeSize      = inodeSize;
    mnt->inodesPerGroup = sb.s_inodes_per_group;
    mnt->blocksPerGroup = sb.s_blocks_per_group;
    mnt->groupCount     = groupCount;
    mnt->firstDataBlock = sb.s_first_data_block;
    mnt->bgdt           = bgdt;

    *mountPriv = mnt;
    DbgPrintf("ext2: mounted successfully\n");
    return true;
}

static void Ext2FsUnmount(void* mountPriv)
{
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);
    if (!mnt) return;
    if (mnt->bgdt) kfree(mnt->bgdt);
    kfree(mnt);
}

static Vnode* Ext2FsOpen(void* mountPriv, uint8_t pdrv,
                         const char* relPath, int flags)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);
    if (flags & VFS_O_WRITE) return nullptr; // read-only

    KMutexLock(&g_ext2Lock);

    // Handle root directory
    bool isRoot = (!relPath[0] || (relPath[0] == '/' && !relPath[1]));
    uint32_t ino;
    if (isRoot) {
        ino = EXT2_ROOT_INO;
    } else {
        // Strip leading slash for resolve
        const char* p = relPath;
        while (*p == '/') ++p;
        ino = Ext2ResolvePath(mnt, EXT2_ROOT_INO, p, 0);
    }

    if (!ino) { KMutexUnlock(&g_ext2Lock); return nullptr; }

    Ext2Inode inodeData;
    if (!Ext2ReadInode(mnt, ino, &inodeData)) { KMutexUnlock(&g_ext2Lock); return nullptr; }

    KMutexUnlock(&g_ext2Lock);

    uint16_t mode = inodeData.i_mode & EXT2_S_IFMT;

    if (mode == EXT2_S_IFDIR) {
        auto* dp = static_cast<Ext2DirPriv*>(kmalloc(sizeof(Ext2DirPriv)));
        if (!dp) return nullptr;
        dp->mnt = mnt;
        dp->inode = ino;
        dp->inodeData = inodeData;
        dp->readOffset = 0;

        auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!vn) { kfree(dp); return nullptr; }
        vn->ops = &g_ext2DirOps;
        vn->type = VnodeType::Dir;
        vn->priv = dp;
        vn->refCount = 1;
        return vn;
    }

    if (mode == EXT2_S_IFREG) {
        auto* fp = static_cast<Ext2FilePriv*>(kmalloc(sizeof(Ext2FilePriv)));
        if (!fp) return nullptr;
        fp->mnt = mnt;
        fp->inode = ino;
        fp->inodeData = inodeData;

        auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!vn) { kfree(fp); return nullptr; }
        vn->ops = &g_ext2FileOps;
        vn->type = VnodeType::File;
        vn->priv = fp;
        vn->refCount = 1;
        return vn;
    }

    // Symlinks should have been resolved during path resolution
    return nullptr;
}

static int Ext2FsStatPath(void* mountPriv, uint8_t pdrv,
                          const char* relPath, VnodeStat* st)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);

    bool isRoot = (!relPath[0] || (relPath[0] == '/' && !relPath[1]));

    KMutexLock(&g_ext2Lock);

    uint32_t ino;
    if (isRoot) {
        ino = EXT2_ROOT_INO;
    } else {
        const char* p = relPath;
        while (*p == '/') ++p;
        ino = Ext2ResolvePath(mnt, EXT2_ROOT_INO, p, 0);
    }

    if (!ino) { KMutexUnlock(&g_ext2Lock); return -1; }

    Ext2Inode inodeData;
    if (!Ext2ReadInode(mnt, ino, &inodeData)) { KMutexUnlock(&g_ext2Lock); return -1; }

    KMutexUnlock(&g_ext2Lock);

    uint16_t mode = inodeData.i_mode & EXT2_S_IFMT;
    st->isDir = (mode == EXT2_S_IFDIR);
    st->size  = st->isDir ? 0 : Ext2InodeSize(&inodeData);
    return 0;
}

// Read-only stubs
static int Ext2FsUnlink(void*, uint8_t, const char*) { return -1; }
static int Ext2FsMkdir(void*, uint8_t, const char*) { return -1; }
static int Ext2FsRename(void*, uint8_t, const char*, const char*) { return -1; }

static const VfsFsOps g_ext2FsOps = {
    .mount     = Ext2FsMount,
    .unmount   = Ext2FsUnmount,
    .open      = Ext2FsOpen,
    .stat_path = Ext2FsStatPath,
    .unlink    = Ext2FsUnlink,
    .mkdir     = Ext2FsMkdir,
    .rename    = Ext2FsRename,
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Ext2VfsRegister()
{
    EnsureLock();
    VfsRegisterFs("ext2", &g_ext2FsOps);
}

} // namespace brook

// Bind a block device to an ext2 pdrv slot.
extern "C" bool Ext2BindDevice(uint8_t pdrv, brook::Device* dev)
{
    if (pdrv >= brook::EXT2_MAX_MOUNTS || !dev) return false;
    brook::g_ext2Devices[pdrv] = dev;
    brook::SerialPrintf("ext2: drive %u → '%s'\n", pdrv, dev->name);
    return true;
}
