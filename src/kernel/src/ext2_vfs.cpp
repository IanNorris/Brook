// ext2_vfs.cpp — Ext2 filesystem driver for Brook VFS (read/write).
//
// Implements: superblock parsing, inode lookup, block mapping (direct/indirect/
// doubly-indirect/triply-indirect), directory traversal, symlink resolution,
// block/inode allocation from bitmaps, file write, mkdir, unlink, rename,
// symlink creation, and the VfsFsOps vtable.

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
    uint32_t totalInodes;    // s_inodes_count
    uint32_t totalBlocks;    // s_blocks_count
    uint64_t bgdtDiskOff;    // byte offset of BGDT on disk
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

void Ext2ForceUnlockForPid(uint32_t pid)
{
    if (g_ext2LockInit)
        KMutexForceUnlock(&g_ext2Lock, pid);
}

// ---------------------------------------------------------------------------
// Device I/O helpers
// ---------------------------------------------------------------------------

// Read exactly `len` bytes from device at `byteOffset`.
static bool Ext2DevRead(Ext2Mount* mnt, uint64_t byteOffset, void* buf, uint64_t len)
{
    int r = mnt->dev->ops->read(mnt->dev, byteOffset, buf, len);
    if (r != static_cast<int>(len))
    {
        SerialPrintf("ext2: DevRead FAIL off=%llu len=%llu got=%d\n", byteOffset, len, r);
        return false;
    }
    return true;
}

// Read a single block into buf.
static bool Ext2ReadBlock(Ext2Mount* mnt, uint32_t blockNum, void* buf)
{
    if (blockNum == 0) return false;
    uint64_t off = static_cast<uint64_t>(blockNum) << mnt->blockShift;
    return Ext2DevRead(mnt, off, buf, mnt->blockSize);
}

// Forward declarations for functions defined later but needed by write helpers
static bool Ext2ReadInode(Ext2Mount* mnt, uint32_t ino, Ext2Inode* out);
static uint64_t Ext2InodeSize(const Ext2Inode* ino);
static uint32_t Ext2BlockMap(Ext2Mount* mnt, const Ext2Inode* ino, uint32_t fileBlock);
static int Ext2ReadInodeData(Ext2Mount* mnt, const Ext2Inode* ino,
                             void* buf, uint64_t len, uint64_t offset);
static uint32_t Ext2DirLookup(Ext2Mount* mnt, const Ext2Inode* dirIno, const char* name);
static uint32_t Ext2ResolvePath(Ext2Mount* mnt, uint32_t startIno,
                                const char* path, int symlinkDepth);

// Write exactly `len` bytes to device at `byteOffset`.
static bool Ext2DevWrite(Ext2Mount* mnt, uint64_t byteOffset, const void* buf, uint64_t len)
{
    int r = mnt->dev->ops->write(mnt->dev, byteOffset, buf, len);
    return r == static_cast<int>(len);
}

// Write a single block from buf.
static bool Ext2WriteBlock(Ext2Mount* mnt, uint32_t blockNum, const void* buf)
{
    if (blockNum == 0) return false;
    // SerialPrintf("ext2: WriteBlock %u\n", blockNum);
    uint64_t off = static_cast<uint64_t>(blockNum) << mnt->blockShift;
    return Ext2DevWrite(mnt, off, buf, mnt->blockSize);
}

// Write an inode back to disk.
static bool Ext2WriteInode(Ext2Mount* mnt, uint32_t ino, const Ext2Inode* data)
{
    if (ino == 0) return false;
    uint32_t group = (ino - 1) / mnt->inodesPerGroup;
    uint32_t index = (ino - 1) % mnt->inodesPerGroup;
    if (group >= mnt->groupCount) return false;

    uint64_t tableOff = static_cast<uint64_t>(mnt->bgdt[group].bg_inode_table)
                        << mnt->blockShift;
    uint64_t inodeOff = tableOff + static_cast<uint64_t>(index) * mnt->inodeSize;
    return Ext2DevWrite(mnt, inodeOff, data, sizeof(Ext2Inode));
}

// Flush the BGDT back to disk.
static bool Ext2WriteBGDT(Ext2Mount* mnt)
{
    uint32_t bgdtSize = mnt->groupCount * sizeof(Ext2BlockGroupDesc);
    return Ext2DevWrite(mnt, mnt->bgdtDiskOff, mnt->bgdt, bgdtSize);
}

// ---------------------------------------------------------------------------
// Block and inode allocation
// ---------------------------------------------------------------------------

// Allocate a free block from the bitmap. Returns block number or 0 on failure.
static uint32_t Ext2AllocBlock(Ext2Mount* mnt)
{
    auto* bitmap = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!bitmap) return 0;

    for (uint32_t g = 0; g < mnt->groupCount; ++g) {
        if (mnt->bgdt[g].bg_free_blocks_count == 0) continue;

        uint32_t bitmapBlock = mnt->bgdt[g].bg_block_bitmap;
        if (!Ext2ReadBlock(mnt, bitmapBlock, bitmap)) continue;

        uint32_t blocksInGroup = mnt->blocksPerGroup;
        // Last group may have fewer blocks
        if (g == mnt->groupCount - 1)
            blocksInGroup = mnt->totalBlocks - g * mnt->blocksPerGroup;

        for (uint32_t bit = 0; bit < blocksInGroup; ++bit) {
            if (!(bitmap[bit / 8] & (1 << (bit % 8)))) {
                // Found free block — mark it used
                bitmap[bit / 8] |= (1 << (bit % 8));
                if (!Ext2WriteBlock(mnt, bitmapBlock, bitmap)) { kfree(bitmap); return 0; }
                mnt->bgdt[g].bg_free_blocks_count--;
                Ext2WriteBGDT(mnt);
                kfree(bitmap);
                return g * mnt->blocksPerGroup + bit + mnt->firstDataBlock;
            }
        }
    }
    kfree(bitmap);
    return 0;
}

// Free a block back to the bitmap.
static bool Ext2FreeBlock(Ext2Mount* mnt, uint32_t blockNum)
{
    if (blockNum < mnt->firstDataBlock) return false;
    uint32_t rel = blockNum - mnt->firstDataBlock;
    uint32_t g = rel / mnt->blocksPerGroup;
    uint32_t bit = rel % mnt->blocksPerGroup;
    if (g >= mnt->groupCount) return false;

    auto* bitmap = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!bitmap) return false;
    if (!Ext2ReadBlock(mnt, mnt->bgdt[g].bg_block_bitmap, bitmap)) {
        kfree(bitmap); return false;
    }
    bitmap[bit / 8] &= ~(1 << (bit % 8));
    bool ok = Ext2WriteBlock(mnt, mnt->bgdt[g].bg_block_bitmap, bitmap);
    kfree(bitmap);
    if (ok) { mnt->bgdt[g].bg_free_blocks_count++; Ext2WriteBGDT(mnt); }
    return ok;
}

// Allocate a free inode. Returns inode number or 0 on failure.
// isDir: increment bg_used_dirs_count.
static uint32_t Ext2AllocInode(Ext2Mount* mnt, bool isDir)
{
    auto* bitmap = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!bitmap) return 0;

    for (uint32_t g = 0; g < mnt->groupCount; ++g) {
        if (mnt->bgdt[g].bg_free_inodes_count == 0) continue;

        uint32_t bitmapBlock = mnt->bgdt[g].bg_inode_bitmap;
        if (!Ext2ReadBlock(mnt, bitmapBlock, bitmap)) continue;

        for (uint32_t bit = 0; bit < mnt->inodesPerGroup; ++bit) {
            if (!(bitmap[bit / 8] & (1 << (bit % 8)))) {
                bitmap[bit / 8] |= (1 << (bit % 8));
                if (!Ext2WriteBlock(mnt, bitmapBlock, bitmap)) { kfree(bitmap); return 0; }
                mnt->bgdt[g].bg_free_inodes_count--;
                if (isDir) mnt->bgdt[g].bg_used_dirs_count++;
                Ext2WriteBGDT(mnt);
                kfree(bitmap);
                return g * mnt->inodesPerGroup + bit + 1; // inodes are 1-based
            }
        }
    }
    kfree(bitmap);
    return 0;
}

// Free an inode back to the bitmap.
static bool Ext2FreeInode(Ext2Mount* mnt, uint32_t ino, bool isDir)
{
    if (ino == 0) return false;
    uint32_t g = (ino - 1) / mnt->inodesPerGroup;
    uint32_t bit = (ino - 1) % mnt->inodesPerGroup;
    if (g >= mnt->groupCount) return false;

    auto* bitmap = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!bitmap) return false;
    if (!Ext2ReadBlock(mnt, mnt->bgdt[g].bg_inode_bitmap, bitmap)) {
        kfree(bitmap); return false;
    }
    bitmap[bit / 8] &= ~(1 << (bit % 8));
    bool ok = Ext2WriteBlock(mnt, mnt->bgdt[g].bg_inode_bitmap, bitmap);
    kfree(bitmap);
    if (ok) {
        mnt->bgdt[g].bg_free_inodes_count++;
        if (isDir) mnt->bgdt[g].bg_used_dirs_count--;
        Ext2WriteBGDT(mnt);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Write helpers: inode data write, block assignment
// ---------------------------------------------------------------------------

// Ensure inode has a block mapped at `fileBlock`. Allocates if needed.
// Returns disk block number or 0 on failure.
static uint32_t Ext2EnsureBlock(Ext2Mount* mnt, Ext2Inode* ino,
                                uint32_t inoNum, uint32_t fileBlock)
{
    uint32_t ptrsPerBlock = mnt->blockSize / 4;

    // Direct blocks (0..11)
    if (fileBlock < 12) {
        if (ino->i_block[fileBlock]) return ino->i_block[fileBlock];
        uint32_t nb = Ext2AllocBlock(mnt);
        if (!nb) return 0;
        ino->i_block[fileBlock] = nb;
        Ext2WriteInode(mnt, inoNum, ino);
        return nb;
    }
    fileBlock -= 12;

    // Singly indirect
    if (fileBlock < ptrsPerBlock) {
        if (!ino->i_block[12]) {
            uint32_t nb = Ext2AllocBlock(mnt);
            if (!nb) return 0;
            // Zero out new indirect block
            auto* zb = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
            if (!zb) return 0;
            for (uint32_t i = 0; i < mnt->blockSize; ++i) zb[i] = 0;
            Ext2WriteBlock(mnt, nb, zb);
            kfree(zb);
            ino->i_block[12] = nb;
            Ext2WriteInode(mnt, inoNum, ino);
        }
        uint32_t entry = 0;
        uint64_t off = (static_cast<uint64_t>(ino->i_block[12]) << mnt->blockShift)
                       + fileBlock * 4;
        Ext2DevRead(mnt, off, &entry, 4);
        if (entry) return entry;
        uint32_t nb = Ext2AllocBlock(mnt);
        if (!nb) return 0;
        Ext2DevWrite(mnt, off, &nb, 4);
        return nb;
    }
    fileBlock -= ptrsPerBlock;

    // Doubly indirect
    if (fileBlock < ptrsPerBlock * ptrsPerBlock) {
        if (!ino->i_block[13]) {
            uint32_t nb = Ext2AllocBlock(mnt);
            if (!nb) return 0;
            auto* zb = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
            if (!zb) return 0;
            for (uint32_t i = 0; i < mnt->blockSize; ++i) zb[i] = 0;
            Ext2WriteBlock(mnt, nb, zb);
            kfree(zb);
            ino->i_block[13] = nb;
            Ext2WriteInode(mnt, inoNum, ino);
        }
        uint32_t idx1 = fileBlock / ptrsPerBlock;
        uint32_t idx2 = fileBlock % ptrsPerBlock;

        uint64_t off1 = (static_cast<uint64_t>(ino->i_block[13]) << mnt->blockShift) + idx1 * 4;
        uint32_t indBlock = 0;
        Ext2DevRead(mnt, off1, &indBlock, 4);
        if (!indBlock) {
            indBlock = Ext2AllocBlock(mnt);
            if (!indBlock) return 0;
            auto* zb = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
            if (!zb) return 0;
            for (uint32_t i = 0; i < mnt->blockSize; ++i) zb[i] = 0;
            Ext2WriteBlock(mnt, indBlock, zb);
            kfree(zb);
            Ext2DevWrite(mnt, off1, &indBlock, 4);
        }

        uint64_t off2 = (static_cast<uint64_t>(indBlock) << mnt->blockShift) + idx2 * 4;
        uint32_t entry = 0;
        Ext2DevRead(mnt, off2, &entry, 4);
        if (entry) return entry;
        uint32_t nb = Ext2AllocBlock(mnt);
        if (!nb) return 0;
        Ext2DevWrite(mnt, off2, &nb, 4);
        return nb;
    }

    // Triply indirect not implemented for writes yet
    return 0;
}

// Write `len` bytes to inode data at `offset`. Returns bytes written.
static int Ext2WriteInodeData(Ext2Mount* mnt, Ext2Inode* ino, uint32_t inoNum,
                              const void* buf, uint64_t len, uint64_t offset)
{
    auto* src = static_cast<const uint8_t*>(buf);
    uint64_t bytesWritten = 0;

    auto* blockBuf = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!blockBuf) return -1;

    while (bytesWritten < len) {
        uint32_t fileBlock = static_cast<uint32_t>((offset + bytesWritten) >> mnt->blockShift);
        uint32_t blockOff  = static_cast<uint32_t>((offset + bytesWritten) & (mnt->blockSize - 1));
        uint32_t diskBlock = Ext2EnsureBlock(mnt, ino, inoNum, fileBlock);
        if (!diskBlock) break;

        // If partial block write, read-modify-write
        if (blockOff != 0 || (len - bytesWritten) < mnt->blockSize) {
            Ext2ReadBlock(mnt, diskBlock, blockBuf);
        }

        uint32_t avail = mnt->blockSize - blockOff;
        uint64_t toCopy = len - bytesWritten;
        if (toCopy > avail) toCopy = avail;

        for (uint64_t i = 0; i < toCopy; ++i)
            blockBuf[blockOff + i] = src[bytesWritten + i];

        if (!Ext2WriteBlock(mnt, diskBlock, blockBuf)) break;
        bytesWritten += toCopy;
    }

    kfree(blockBuf);

    // Update inode size if we extended the file
    uint64_t newEnd = offset + bytesWritten;
    if (newEnd > ino->i_size) {
        ino->i_size = static_cast<uint32_t>(newEnd);
        ino->i_blocks = static_cast<uint32_t>(
            ((newEnd + mnt->blockSize - 1) >> mnt->blockShift) * (mnt->blockSize / 512));
        Ext2WriteInode(mnt, inoNum, ino);
    }

    return static_cast<int>(bytesWritten);
}

// Free all data blocks of an inode (direct + indirect).
static void Ext2FreeInodeBlocks(Ext2Mount* mnt, Ext2Inode* ino)
{
    uint32_t ptrsPerBlock = mnt->blockSize / 4;

    // Direct blocks
    for (int i = 0; i < 12; ++i) {
        if (ino->i_block[i]) { Ext2FreeBlock(mnt, ino->i_block[i]); ino->i_block[i] = 0; }
    }

    // Singly indirect
    if (ino->i_block[12]) {
        auto* ind = static_cast<uint32_t*>(kmalloc(mnt->blockSize));
        if (ind) {
            Ext2ReadBlock(mnt, ino->i_block[12], ind);
            for (uint32_t i = 0; i < ptrsPerBlock; ++i)
                if (ind[i]) Ext2FreeBlock(mnt, ind[i]);
            kfree(ind);
        }
        Ext2FreeBlock(mnt, ino->i_block[12]);
        ino->i_block[12] = 0;
    }

    // Doubly indirect
    if (ino->i_block[13]) {
        auto* dind = static_cast<uint32_t*>(kmalloc(mnt->blockSize));
        if (dind) {
            Ext2ReadBlock(mnt, ino->i_block[13], dind);
            for (uint32_t i = 0; i < ptrsPerBlock; ++i) {
                if (dind[i]) {
                    auto* ind = static_cast<uint32_t*>(kmalloc(mnt->blockSize));
                    if (ind) {
                        Ext2ReadBlock(mnt, dind[i], ind);
                        for (uint32_t j = 0; j < ptrsPerBlock; ++j)
                            if (ind[j]) Ext2FreeBlock(mnt, ind[j]);
                        kfree(ind);
                    }
                    Ext2FreeBlock(mnt, dind[i]);
                }
            }
            kfree(dind);
        }
        Ext2FreeBlock(mnt, ino->i_block[13]);
        ino->i_block[13] = 0;
    }

    // Triply indirect — not freed for now (very large files rare in hobby OS)
    ino->i_blocks = 0;
    ino->i_size = 0;
}

// ---------------------------------------------------------------------------
// Directory manipulation
// ---------------------------------------------------------------------------

// Add a directory entry to a directory inode.
static bool Ext2DirAdd(Ext2Mount* mnt, uint32_t dirIno, Ext2Inode* dirData,
                       uint32_t childIno, const char* name, uint8_t fileType)
{
    uint32_t nameLen = 0;
    for (const char* p = name; *p; ++p) ++nameLen;
    if (nameLen == 0 || nameLen > 255) return false;

    // SerialPrintf("ext2: DirAdd '%s' (ino %u) into dir ino %u\n", name, childIno, dirIno);

    // Required size for new entry (8 bytes header + name, 4-byte aligned)
    uint32_t neededLen = ((8 + nameLen + 3) / 4) * 4;

    uint64_t dirSize = Ext2InodeSize(dirData);
    auto* blockBuf = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!blockBuf) return false;

    // Scan existing entries for space (look for slack in last entry's rec_len)
    uint64_t off = 0;
    while (off < dirSize) {
        uint32_t fileBlock = static_cast<uint32_t>(off >> mnt->blockShift);
        uint32_t diskBlock = Ext2BlockMap(mnt, dirData, fileBlock);
        if (!diskBlock || !Ext2ReadBlock(mnt, diskBlock, blockBuf)) break;

        uint32_t pos = 0;
        while (pos < mnt->blockSize) {
            auto* de = reinterpret_cast<Ext2DirEntry2*>(blockBuf + pos);
            if (de->rec_len == 0) break;

            // Calculate actual size of this entry
            uint32_t actualLen = ((8 + de->name_len + 3) / 4) * 4;
            if (de->inode == 0) actualLen = 0; // deleted entry uses no real space

            uint32_t slack = de->rec_len - actualLen;
            if (slack >= neededLen) {
                // Split this entry
                if (de->inode != 0) {
                    de->rec_len = static_cast<uint16_t>(actualLen);
                    pos += actualLen;
                }
                auto* newDe = reinterpret_cast<Ext2DirEntry2*>(blockBuf + pos);
                newDe->inode = childIno;
                newDe->rec_len = static_cast<uint16_t>(
                    de->inode != 0 ? slack : de->rec_len);
                newDe->name_len = static_cast<uint8_t>(nameLen);
                newDe->file_type = fileType;
                for (uint32_t i = 0; i < nameLen; ++i) newDe->name[i] = name[i];

                Ext2WriteBlock(mnt, diskBlock, blockBuf);
                kfree(blockBuf);
                return true;
            }

            pos += de->rec_len;
        }
        off += mnt->blockSize;
    }

    // No space found — allocate a new block for the directory
    uint32_t newBlock = Ext2EnsureBlock(mnt, dirData, dirIno,
                                        static_cast<uint32_t>(dirSize >> mnt->blockShift));
    if (!newBlock) { kfree(blockBuf); return false; }

    // Fill the new block with our entry
    for (uint32_t i = 0; i < mnt->blockSize; ++i) blockBuf[i] = 0;
    auto* newDe = reinterpret_cast<Ext2DirEntry2*>(blockBuf);
    newDe->inode = childIno;
    newDe->rec_len = static_cast<uint16_t>(mnt->blockSize); // spans entire block
    newDe->name_len = static_cast<uint8_t>(nameLen);
    newDe->file_type = fileType;
    for (uint32_t i = 0; i < nameLen; ++i) newDe->name[i] = name[i];

    Ext2WriteBlock(mnt, newBlock, blockBuf);
    kfree(blockBuf);

    // Update directory size
    dirData->i_size = static_cast<uint32_t>(dirSize + mnt->blockSize);
    dirData->i_blocks = static_cast<uint32_t>(
        ((dirData->i_size + mnt->blockSize - 1) >> mnt->blockShift) * (mnt->blockSize / 512));
    Ext2WriteInode(mnt, dirIno, dirData);
    return true;
}

// Remove a directory entry by name. Returns the removed inode number or 0.
static uint32_t Ext2DirRemove(Ext2Mount* mnt, Ext2Inode* dirData,
                              uint32_t dirIno, const char* name)
{
    uint32_t nameLen = 0;
    for (const char* p = name; *p; ++p) ++nameLen;

    uint64_t dirSize = Ext2InodeSize(dirData);
    auto* blockBuf = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!blockBuf) return 0;

    uint64_t off = 0;
    while (off < dirSize) {
        uint32_t fileBlock = static_cast<uint32_t>(off >> mnt->blockShift);
        uint32_t diskBlock = Ext2BlockMap(mnt, dirData, fileBlock);
        if (!diskBlock || !Ext2ReadBlock(mnt, diskBlock, blockBuf)) break;

        uint32_t pos = 0;
        Ext2DirEntry2* prevDe = nullptr;
        while (pos < mnt->blockSize) {
            auto* de = reinterpret_cast<Ext2DirEntry2*>(blockBuf + pos);
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len == nameLen) {
                bool match = true;
                for (uint32_t i = 0; i < nameLen; ++i)
                    if (de->name[i] != name[i]) { match = false; break; }
                if (match) {
                    uint32_t removedIno = de->inode;
                    if (prevDe) {
                        // Merge with previous entry
                        prevDe->rec_len += de->rec_len;
                    } else {
                        // First entry in block — zero the inode
                        de->inode = 0;
                    }
                    Ext2WriteBlock(mnt, diskBlock, blockBuf);
                    kfree(blockBuf);
                    return removedIno;
                }
            }
            prevDe = de;
            pos += de->rec_len;
        }
        off += mnt->blockSize;
    }
    kfree(blockBuf);
    return 0;
}

// Resolve parent directory and extract final component from a path.
// Returns parent inode number. Writes component name into `nameOut`.
static uint32_t Ext2ResolveParent(Ext2Mount* mnt, const char* relPath,
                                  char* nameOut, uint32_t nameOutSize)
{
    // Find last slash
    const char* p = relPath;
    while (*p == '/') ++p; // skip leading slashes
    const char* lastSlash = nullptr;
    for (const char* q = p; *q; ++q)
        if (*q == '/') lastSlash = q;

    if (!lastSlash) {
        // No slash — parent is root, name is entire path
        uint32_t i = 0;
        while (p[i] && i < nameOutSize - 1) { nameOut[i] = p[i]; ++i; }
        nameOut[i] = '\0';
        return EXT2_ROOT_INO;
    }

    // Copy parent path
    uint32_t parentLen = static_cast<uint32_t>(lastSlash - p);
    char parentPath[256];
    if (parentLen >= sizeof(parentPath)) return 0;
    for (uint32_t i = 0; i < parentLen; ++i) parentPath[i] = p[i];
    parentPath[parentLen] = '\0';

    // Copy name (after last slash)
    const char* name = lastSlash + 1;
    while (*name == '/') ++name;
    uint32_t ni = 0;
    while (name[ni] && ni < nameOutSize - 1) { nameOut[ni] = name[ni]; ++ni; }
    nameOut[ni] = '\0';

    return Ext2ResolvePath(mnt, EXT2_ROOT_INO, parentPath, 0);
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
        if (!dindBlock) {
            SerialPrintf("ext2: dind i_block[13]=0 for fileBlock=%u\n", fileBlock + 12 + ptrsPerBlock);
            return 0;
        }
        uint32_t idx1 = fileBlock / ptrsPerBlock;
        uint32_t idx2 = fileBlock % ptrsPerBlock;
        uint32_t indBlock = 0;
        uint64_t off1 = (static_cast<uint64_t>(dindBlock) << mnt->blockShift) + idx1 * 4;
        if (!Ext2DevRead(mnt, off1, &indBlock, 4) || !indBlock) {
            SerialPrintf("ext2: dind L1 fail idx1=%u indBlock=%u dindBlock=%u\n", idx1, indBlock, dindBlock);
            return 0;
        }
        uint32_t entry = 0;
        uint64_t off2 = (static_cast<uint64_t>(indBlock) << mnt->blockShift) + idx2 * 4;
        if (!Ext2DevRead(mnt, off2, &entry, 4)) {
            SerialPrintf("ext2: dind L2 fail idx2=%u indBlock=%u\n", idx2, indBlock);
            return 0;
        }
        if (!entry) {
            // sparse hole — block not allocated
        }
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
    if (offset >= fileSize) {
        return 0;
    }
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
        if (!diskBlock) {
            break; // sparse hole or error
        }

        if (!Ext2ReadBlock(mnt, diskBlock, blockBuf)) {
            SerialPrintf("ext2: ReadBlock failed for diskBlock=%u (fileBlock=%u)\n",
                diskBlock, fileBlock);
            break;
        }

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
    if (!Ext2ReadInode(mnt, curIno, &dirIno)) {
        SerialPrintf("ext2: ReadInode %u FAILED for component '%s'\n", curIno, component);
        return 0;
    }
    if ((dirIno.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        SerialPrintf("ext2: ino %u not a dir (mode 0x%x) for component '%s'\n",
                     curIno, dirIno.i_mode, component);
        return 0;
    }

    uint32_t childIno = Ext2DirLookup(mnt, &dirIno, component);
    if (!childIno) {
        // SerialPrintf("ext2: DirLookup '%s' in ino %u → MISS\n", component, curIno);
        return 0;
    }

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

static int Ext2FileWrite(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset)
{
    auto* fp = static_cast<Ext2FilePriv*>(vn->priv);
    KMutexLock(&g_ext2Lock);
    int r = Ext2WriteInodeData(fp->mnt, &fp->inodeData, fp->inode, buf, len, *offset);
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
    .write   = Ext2FileWrite,
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
static constexpr uint8_t EXT2_MAX_MOUNTS = 8;
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
    mnt->totalInodes    = sb.s_inodes_count;
    mnt->totalBlocks    = sb.s_blocks_count;
    mnt->bgdtDiskOff    = bgdtOff;
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

    KMutexLock(&g_ext2Lock);

    // Handle root directory
    bool isRoot = (!relPath[0] || (relPath[0] == '/' && !relPath[1]));
    uint32_t ino = 0;
    Ext2Inode inodeData;

    if (isRoot) {
        ino = EXT2_ROOT_INO;
    } else {
        const char* p = relPath;
        while (*p == '/') ++p;
        ino = Ext2ResolvePath(mnt, EXT2_ROOT_INO, p, 0);
    }

    // File not found — create if requested
    if (!ino && (flags & VFS_O_CREATE)) {
        SerialPrintf("ext2: CREATE file '%s'\n", relPath);
        char name[256];
        uint32_t parentIno = Ext2ResolveParent(mnt, relPath, name, sizeof(name));
        if (!parentIno || !name[0]) { KMutexUnlock(&g_ext2Lock); return nullptr; }

        Ext2Inode parentData;
        if (!Ext2ReadInode(mnt, parentIno, &parentData)) { KMutexUnlock(&g_ext2Lock); return nullptr; }
        if ((parentData.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) { KMutexUnlock(&g_ext2Lock); return nullptr; }

        // Allocate new inode
        ino = Ext2AllocInode(mnt, false);
        if (!ino) { KMutexUnlock(&g_ext2Lock); return nullptr; }

        // Initialize inode
        for (uint32_t i = 0; i < sizeof(inodeData); ++i)
            reinterpret_cast<uint8_t*>(&inodeData)[i] = 0;
        inodeData.i_mode = EXT2_S_IFREG | 0644;
        inodeData.i_links_count = 1;
        Ext2WriteInode(mnt, ino, &inodeData);

        // Add directory entry
        if (!Ext2DirAdd(mnt, parentIno, &parentData, ino, name, 1 /*EXT2_FT_REG_FILE*/)) {
            Ext2FreeInode(mnt, ino, false);
            KMutexUnlock(&g_ext2Lock);
            return nullptr;
        }
    }

    if (!ino) {
        SerialPrintf("ext2: open '%s' → not found\n", relPath);
        KMutexUnlock(&g_ext2Lock);
        return nullptr;
    }

    if (!Ext2ReadInode(mnt, ino, &inodeData)) { KMutexUnlock(&g_ext2Lock); return nullptr; }

    // Truncate if requested
    if ((flags & VFS_O_TRUNC) && (inodeData.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG) {
        Ext2FreeInodeBlocks(mnt, &inodeData);
        Ext2WriteInode(mnt, ino, &inodeData);
    }

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

    if (!ino) {
        SerialPrintf("ext2: stat '%s' → not found\n", relPath);
        KMutexUnlock(&g_ext2Lock);
        return -1;
    }

    Ext2Inode inodeData;
    if (!Ext2ReadInode(mnt, ino, &inodeData)) {
        SerialPrintf("ext2: stat '%s' ino=%u → read failed\n", relPath, ino);
        KMutexUnlock(&g_ext2Lock);
        return -1;
    }

    KMutexUnlock(&g_ext2Lock);

    uint16_t mode = inodeData.i_mode & EXT2_S_IFMT;
    st->isDir = (mode == EXT2_S_IFDIR);
    st->isSymlink = false; // stat follows symlinks, so result is never a symlink
    st->size  = st->isDir ? 0 : Ext2InodeSize(&inodeData);
    return 0;
}

// lstat: like stat but does NOT follow the final symlink
static int Ext2FsLstatPath(void* mountPriv, uint8_t pdrv,
                            const char* relPath, VnodeStat* st)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);

    bool isRoot = (!relPath[0] || (relPath[0] == '/' && !relPath[1]));

    KMutexLock(&g_ext2Lock);

    if (isRoot) {
        Ext2Inode inodeData;
        if (!Ext2ReadInode(mnt, EXT2_ROOT_INO, &inodeData)) {
            KMutexUnlock(&g_ext2Lock);
            return -1;
        }
        KMutexUnlock(&g_ext2Lock);
        st->isDir = true;
        st->isSymlink = false;
        st->size = 0;
        return 0;
    }

    // Resolve parent directory (following symlinks in the path), then
    // look up the final component directly without following it.
    char name[256];
    uint32_t parentIno = Ext2ResolveParent(mnt, relPath, name, sizeof(name));
    if (!parentIno || !name[0]) { KMutexUnlock(&g_ext2Lock); return -1; }

    Ext2Inode parentData;
    if (!Ext2ReadInode(mnt, parentIno, &parentData)) { KMutexUnlock(&g_ext2Lock); return -1; }

    uint32_t ino = Ext2DirLookup(mnt, &parentData, name);
    if (!ino) { KMutexUnlock(&g_ext2Lock); return -1; }

    Ext2Inode inodeData;
    if (!Ext2ReadInode(mnt, ino, &inodeData)) { KMutexUnlock(&g_ext2Lock); return -1; }

    KMutexUnlock(&g_ext2Lock);

    uint16_t mode = inodeData.i_mode & EXT2_S_IFMT;
    st->isDir     = (mode == EXT2_S_IFDIR);
    st->isSymlink = (mode == EXT2_S_IFLNK);
    st->size      = Ext2InodeSize(&inodeData);
    return 0;
}

// ---------------------------------------------------------------------------
// VfsFsOps write operations
// ---------------------------------------------------------------------------

static int Ext2FsUnlink(void* mountPriv, uint8_t pdrv, const char* relPath)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);

    KMutexLock(&g_ext2Lock);

    char name[256];
    uint32_t parentIno = Ext2ResolveParent(mnt, relPath, name, sizeof(name));
    if (!parentIno || !name[0]) { KMutexUnlock(&g_ext2Lock); return -1; }

    Ext2Inode parentData;
    if (!Ext2ReadInode(mnt, parentIno, &parentData)) { KMutexUnlock(&g_ext2Lock); return -1; }

    uint32_t removedIno = Ext2DirRemove(mnt, &parentData, parentIno, name);
    if (!removedIno) { KMutexUnlock(&g_ext2Lock); return -1; }

    // Decrement link count, free if zero
    Ext2Inode removedData;
    if (Ext2ReadInode(mnt, removedIno, &removedData)) {
        removedData.i_links_count--;
        if (removedData.i_links_count == 0) {
            bool isDir = (removedData.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
            Ext2FreeInodeBlocks(mnt, &removedData);
            Ext2WriteInode(mnt, removedIno, &removedData);
            Ext2FreeInode(mnt, removedIno, isDir);
        } else {
            Ext2WriteInode(mnt, removedIno, &removedData);
        }
    }

    KMutexUnlock(&g_ext2Lock);
    return 0;
}

static int Ext2FsMkdir(void* mountPriv, uint8_t pdrv, const char* relPath)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);

    SerialPrintf("ext2: MKDIR '%s'\n", relPath);

    KMutexLock(&g_ext2Lock);

    char name[256];
    uint32_t parentIno = Ext2ResolveParent(mnt, relPath, name, sizeof(name));
    if (!parentIno || !name[0]) { KMutexUnlock(&g_ext2Lock); return -1; }

    Ext2Inode parentData;
    if (!Ext2ReadInode(mnt, parentIno, &parentData)) { KMutexUnlock(&g_ext2Lock); return -1; }

    // Check if already exists
    if (Ext2DirLookup(mnt, &parentData, name)) {
        KMutexUnlock(&g_ext2Lock);
        return 0; // Already exists, not an error (like FAT driver)
    }

    // Allocate inode
    uint32_t newIno = Ext2AllocInode(mnt, true);
    if (!newIno) { KMutexUnlock(&g_ext2Lock); return -1; }

    // Initialize directory inode
    Ext2Inode newData;
    for (uint32_t i = 0; i < sizeof(newData); ++i)
        reinterpret_cast<uint8_t*>(&newData)[i] = 0;
    newData.i_mode = EXT2_S_IFDIR | 0755;
    newData.i_links_count = 2; // . and parent's entry

    // Allocate first data block for . and .. entries
    uint32_t dataBlock = Ext2AllocBlock(mnt);
    if (!dataBlock) {
        Ext2FreeInode(mnt, newIno, true);
        KMutexUnlock(&g_ext2Lock);
        return -1;
    }

    newData.i_block[0] = dataBlock;
    newData.i_size = mnt->blockSize;
    newData.i_blocks = mnt->blockSize / 512;

    // Build . and .. entries
    auto* blockBuf = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!blockBuf) {
        Ext2FreeBlock(mnt, dataBlock);
        Ext2FreeInode(mnt, newIno, true);
        KMutexUnlock(&g_ext2Lock);
        return -1;
    }
    for (uint32_t i = 0; i < mnt->blockSize; ++i) blockBuf[i] = 0;

    // . entry
    auto* dot = reinterpret_cast<Ext2DirEntry2*>(blockBuf);
    dot->inode = newIno;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = 2; // EXT2_FT_DIR
    dot->name[0] = '.';

    // .. entry (fills rest of block)
    auto* dotdot = reinterpret_cast<Ext2DirEntry2*>(blockBuf + 12);
    dotdot->inode = parentIno;
    dotdot->rec_len = static_cast<uint16_t>(mnt->blockSize - 12);
    dotdot->name_len = 2;
    dotdot->file_type = 2;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    Ext2WriteBlock(mnt, dataBlock, blockBuf);
    kfree(blockBuf);

    Ext2WriteInode(mnt, newIno, &newData);

    // Add entry to parent
    if (!Ext2DirAdd(mnt, parentIno, &parentData, newIno, name, 2 /*EXT2_FT_DIR*/)) {
        // Rollback
        Ext2FreeBlock(mnt, dataBlock);
        Ext2FreeInode(mnt, newIno, true);
        KMutexUnlock(&g_ext2Lock);
        return -1;
    }

    // Increment parent link count (for ..)
    parentData.i_links_count++;
    Ext2WriteInode(mnt, parentIno, &parentData);

    KMutexUnlock(&g_ext2Lock);
    return 0;
}

static int Ext2FsRename(void* mountPriv, uint8_t pdrv,
                        const char* oldRelPath, const char* newRelPath)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);

    KMutexLock(&g_ext2Lock);

    // Resolve old path
    char oldName[256];
    uint32_t oldParentIno = Ext2ResolveParent(mnt, oldRelPath, oldName, sizeof(oldName));
    if (!oldParentIno) { KMutexUnlock(&g_ext2Lock); return -1; }

    Ext2Inode oldParentData;
    if (!Ext2ReadInode(mnt, oldParentIno, &oldParentData)) { KMutexUnlock(&g_ext2Lock); return -1; }

    // Look up the inode being moved
    uint32_t targetIno = Ext2DirLookup(mnt, &oldParentData, oldName);
    if (!targetIno) { KMutexUnlock(&g_ext2Lock); return -1; }

    Ext2Inode targetData;
    if (!Ext2ReadInode(mnt, targetIno, &targetData)) { KMutexUnlock(&g_ext2Lock); return -1; }

    // Determine file type for new dir entry
    uint8_t ft = 1; // EXT2_FT_REG_FILE
    uint16_t mode = targetData.i_mode & EXT2_S_IFMT;
    if (mode == EXT2_S_IFDIR) ft = 2;
    else if (mode == EXT2_S_IFLNK) ft = 7;

    // Resolve new path
    char newName[256];
    uint32_t newParentIno = Ext2ResolveParent(mnt, newRelPath, newName, sizeof(newName));
    if (!newParentIno) { KMutexUnlock(&g_ext2Lock); return -1; }

    Ext2Inode newParentData;
    if (!Ext2ReadInode(mnt, newParentIno, &newParentData)) { KMutexUnlock(&g_ext2Lock); return -1; }

    // Remove from old directory
    Ext2DirRemove(mnt, &oldParentData, oldParentIno, oldName);

    // Add to new directory
    if (!Ext2DirAdd(mnt, newParentIno, &newParentData, targetIno, newName, ft)) {
        // Try to re-add to old location on failure
        Ext2DirAdd(mnt, oldParentIno, &oldParentData, targetIno, oldName, ft);
        KMutexUnlock(&g_ext2Lock);
        return -1;
    }

    // If moving a directory, update .. entry
    if (mode == EXT2_S_IFDIR && oldParentIno != newParentIno) {
        // Read first block of moved directory, update .. inode
        auto* buf = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
        if (buf && targetData.i_block[0]) {
            Ext2ReadBlock(mnt, targetData.i_block[0], buf);
            // .. is the second entry (after .)
            auto* dot = reinterpret_cast<Ext2DirEntry2*>(buf);
            auto* dotdot = reinterpret_cast<Ext2DirEntry2*>(buf + dot->rec_len);
            dotdot->inode = newParentIno;
            Ext2WriteBlock(mnt, targetData.i_block[0], buf);
            kfree(buf);

            // Update link counts
            oldParentData.i_links_count--;
            Ext2WriteInode(mnt, oldParentIno, &oldParentData);
            newParentData.i_links_count++;
            Ext2WriteInode(mnt, newParentIno, &newParentData);
        } else if (buf) {
            kfree(buf);
        }
    }

    KMutexUnlock(&g_ext2Lock);
    return 0;
}

// ---------------------------------------------------------------------------
// symlink: create a symbolic link
// ---------------------------------------------------------------------------

static int Ext2FsSymlink(void* mountPriv, uint8_t pdrv,
                         const char* target, const char* relPath)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);

    uint32_t targetLen = 0;
    for (const char* p = target; *p; ++p) ++targetLen;
    if (targetLen == 0 || targetLen > 4096) return -1;

    KMutexLock(&g_ext2Lock);

    char name[256];
    uint32_t parentIno = Ext2ResolveParent(mnt, relPath, name, sizeof(name));
    if (!parentIno || !name[0]) { KMutexUnlock(&g_ext2Lock); return -1; }

    Ext2Inode parentData;
    if (!Ext2ReadInode(mnt, parentIno, &parentData)) { KMutexUnlock(&g_ext2Lock); return -1; }

    // Check if already exists
    if (Ext2DirLookup(mnt, &parentData, name)) {
        KMutexUnlock(&g_ext2Lock);
        return -17; // -EEXIST
    }

    uint32_t newIno = Ext2AllocInode(mnt, false);
    if (!newIno) { KMutexUnlock(&g_ext2Lock); return -1; }

    Ext2Inode newData;
    for (uint32_t i = 0; i < sizeof(newData); ++i)
        reinterpret_cast<uint8_t*>(&newData)[i] = 0;
    newData.i_mode = EXT2_S_IFLNK | 0777;
    newData.i_links_count = 1;

    // Fast symlink: store target directly in i_block[] if it fits (≤60 bytes)
    if (targetLen <= 60) {
        auto* dst = reinterpret_cast<char*>(newData.i_block);
        for (uint32_t i = 0; i < targetLen; ++i) dst[i] = target[i];
        newData.i_size = targetLen;
        newData.i_blocks = 0;
    } else {
        // Slow symlink: allocate a data block
        uint32_t blk = Ext2AllocBlock(mnt);
        if (!blk) {
            Ext2FreeInode(mnt, newIno, false);
            KMutexUnlock(&g_ext2Lock);
            return -1;
        }
        auto* buf = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
        if (!buf) {
            Ext2FreeBlock(mnt, blk);
            Ext2FreeInode(mnt, newIno, false);
            KMutexUnlock(&g_ext2Lock);
            return -1;
        }
        for (uint32_t i = 0; i < mnt->blockSize; ++i) buf[i] = 0;
        for (uint32_t i = 0; i < targetLen; ++i) buf[i] = static_cast<uint8_t>(target[i]);
        Ext2WriteBlock(mnt, blk, buf);
        kfree(buf);

        newData.i_block[0] = blk;
        newData.i_size = targetLen;
        newData.i_blocks = mnt->blockSize / 512;
    }

    Ext2WriteInode(mnt, newIno, &newData);

    // Add directory entry with type=7 (EXT2_FT_SYMLINK)
    if (!Ext2DirAdd(mnt, parentIno, &parentData, newIno, name, 7)) {
        Ext2FreeInodeBlocks(mnt, &newData);
        Ext2FreeInode(mnt, newIno, false);
        KMutexUnlock(&g_ext2Lock);
        return -1;
    }

    KMutexUnlock(&g_ext2Lock);
    return 0;
}

// ---------------------------------------------------------------------------
// readlink: read a symbolic link target
// ---------------------------------------------------------------------------

static int Ext2FsReadlink(void* mountPriv, uint8_t pdrv,
                          const char* relPath, char* buf, uint64_t bufsiz)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);

    KMutexLock(&g_ext2Lock);

    // Resolve path WITHOUT following the final symlink component.
    // We split the path into parent + name and resolve the parent,
    // then look up the name directly in the parent directory.
    char name[256];
    uint32_t parentIno = Ext2ResolveParent(mnt, relPath, name, sizeof(name));
    if (!parentIno || !name[0]) { KMutexUnlock(&g_ext2Lock); return -1; }

    Ext2Inode parentData;
    if (!Ext2ReadInode(mnt, parentIno, &parentData)) { KMutexUnlock(&g_ext2Lock); return -1; }

    uint32_t ino = Ext2DirLookup(mnt, &parentData, name);
    if (!ino) { KMutexUnlock(&g_ext2Lock); return -22; } // -EINVAL

    Ext2Inode inodeData;
    if (!Ext2ReadInode(mnt, ino, &inodeData)) { KMutexUnlock(&g_ext2Lock); return -1; }

    if ((inodeData.i_mode & EXT2_S_IFMT) != EXT2_S_IFLNK) {
        KMutexUnlock(&g_ext2Lock);
        return -22; // -EINVAL: not a symlink
    }

    char* target = Ext2ReadSymlink(mnt, &inodeData);
    if (!target) { KMutexUnlock(&g_ext2Lock); return -1; }

    uint32_t targetLen = 0;
    for (const char* p = target; *p; ++p) ++targetLen;

    uint32_t copyLen = targetLen;
    if (copyLen > bufsiz) copyLen = static_cast<uint32_t>(bufsiz);
    for (uint32_t i = 0; i < copyLen; ++i) buf[i] = target[i];

    kfree(target);
    KMutexUnlock(&g_ext2Lock);
    return static_cast<int>(copyLen);
}

static const VfsFsOps g_ext2FsOps = {
    .mount      = Ext2FsMount,
    .unmount    = Ext2FsUnmount,
    .open       = Ext2FsOpen,
    .stat_path  = Ext2FsStatPath,
    .lstat_path = Ext2FsLstatPath,
    .unlink     = Ext2FsUnlink,
    .mkdir      = Ext2FsMkdir,
    .rename     = Ext2FsRename,
    .symlink    = Ext2FsSymlink,
    .readlink   = Ext2FsReadlink,
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
