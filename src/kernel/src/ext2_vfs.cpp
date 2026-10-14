// ext2_vfs.cpp — Ext2 filesystem driver for Brook VFS (read/write).
//
// Implements: superblock parsing, inode lookup, block mapping (direct/indirect/
// doubly-indirect/triply-indirect), directory traversal, symlink resolution,
// block/inode allocation from bitmaps, file write, mkdir, unlink, rename,
// symlink creation, and the VfsFsOps vtable.

#include "ext2_vfs.h"
#include "vfs.h"
#include "device.h"
#include "process.h"
#include "memory/heap.h"
#include "serial.h"
#include "string.h"
#include "sync/krwlock.h"
#include "spinlock.h"
#include "rtc.h"

namespace brook {

// Verbose tracing (per-file kernel debug prints).  Default off because each
// SerialPrintf busy-waits the UART; in nix-install vlc, the not-found print
// alone costs ~35s of UART time while holding g_ext2Lock.  Set true at
// runtime to re-enable.
bool g_kdebugTrace = false;

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
static constexpr uint32_t EXT2_MAX_TIMESTAMP = 0xffffffffu;

static uint32_t Ext2Now()
{
    uint64_t now = RtcNow();
    return now > EXT2_MAX_TIMESTAMP ? EXT2_MAX_TIMESTAMP : static_cast<uint32_t>(now);
}

static void Ext2Touch(Ext2Inode* inode, bool created = false)
{
    inode->i_mtime = inode->i_ctime = Ext2Now();
    if (created) inode->i_atime = inode->i_mtime;
}

struct Ext2DirEntry2 {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[]; // variable-length, NOT null-terminated on disk
};

// file_type values (used in readdir)
static constexpr uint8_t EXT2_FT_DIR      = 2;

// Validate a directory entry from untrusted disk data.
// `pos` is the byte offset within the block, `blockSize` is the block size.
// Returns false if the entry is corrupt (rec_len misaligned, too small, or
// name_len exceeds rec_len).
static inline bool Ext2DirEntryValid(const Ext2DirEntry2* de, uint32_t pos,
                                     uint32_t blockSize)
{
    if (de->rec_len < 8)              return false; // minimum: header only
    if ((de->rec_len & 3) != 0)       return false; // must be 4-byte aligned
    if (pos + de->rec_len > blockSize) return false; // must not exceed block
    if (de->inode != 0 && de->name_len > de->rec_len - 8)
        return false; // name must fit in entry
    return true;
}

// ---------------------------------------------------------------------------
// Mount-private state
// ---------------------------------------------------------------------------

struct Ext2Mount {
    Device*  dev;
    uint32_t cacheNamespace; // unique for this mount's lifetime, including remounts
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

    // --- Metadata write-back cache ---
    // Without this, every file create performs ~4–6 sector writes for
    // bitmap and BGDT updates even though they're all rewrites of the
    // same blocks.  Cache dirty-tracks the BGDT and per-group bitmaps
    // and defers writes until Ext2Sync (called on unmount, periodic
    // safety flush, or sys_sync).  Crash window is bounded by
    // Ext2FsSync being called every OPS_PER_AUTO_SYNC metadata ops.
    bool      bgdtDirty;
    uint8_t** blockBitmapCache;   // [groupCount] — lazy-alloced
    bool*     blockBitmapDirty;   // [groupCount]
    uint8_t** inodeBitmapCache;   // [groupCount]
    bool*     inodeBitmapDirty;   // [groupCount]
    uint32_t  pendingMetaOps;     // ops since last auto-sync

    // --- Indirect-block read cache ---
    // BlockMap() is called once per file-block during sequential reads,
    // and for every fileBlock past the first 12 it has to read
    // indirect blocks from disk.  4 cached slots avoid thrashing when
    // doubly/triply-indirect lookups alternate between L1 and L2 blocks.
    static constexpr uint32_t IND_CACHE_SLOTS = 4;
    SpinLock indCacheLock;
    uint32_t indCacheBlockNum[IND_CACHE_SLOTS];  // 0 = empty
    uint8_t* indCacheData[IND_CACHE_SLOTS];      // blockSize bytes each; lazy-alloced
    uint32_t indCacheLru;                         // next slot to evict (simple round-robin)

    // --- Bitmap allocation hints ---
    // Without these, Ext2AllocBlock/Ext2AllocInode rescan each bitmap from
    // bit 0 every call.  Writing a 1MB file (256 blocks) on a partially
    // filled FS becomes O(N * blocksUsedSoFar).  Hints track "first bit
    // we haven't seen used yet" per group, are reset to <= freed_bit on
    // Ext2FreeBlock/FreeInode, and clamped on every miss so they remain
    // valid even if disk state lies.
    uint32_t* nextFreeBlockHint;  // [groupCount], lazy-init to 0
    uint32_t* nextFreeInodeHint;  // [groupCount], lazy-init to 0

    // --- Inode read cache ---
    // Direct-mapped (1024 slots).  Without this, every Ext2ReadInode
    // hits the disk for 256 bytes -- and path resolution does one
    // ReadInode per component, so opening "/store/<hash>/lib/foo.so"
    // is 5+ disk reads even on cache-warm code.  nix-install opens
    // hundreds of files per invocation, almost all repeat lookups
    // through the same /store/<hash>/... prefixes.  Slot is keyed on
    // ino number (not hash) so collisions just mean replacement.
    // Entry.ino==0 means empty.  Protected by inodeCacheLock on SMP
    // (g_ext2Lock only guarantees mutual exclusion with writers, not
    // between concurrent readers which both call Ext2InodeCachePut).
    SpinLock inodeCacheLock;
    void* inodeCache;             // Ext2InodeCacheEntry[1024]; lazy-alloced

    // --- Directory entry name cache ---
    // Path resolution does Ext2DirLookup once per component, which
    // reads the dir's data block(s) and linear-scans entries.  /store
    // can have thousands of entries, so this dominates lookup cost.
    // Direct-mapped 1024-slot cache of (parentIno, name) -> childIno.
    // Names longer than EXT2_DIRENT_CACHE_NAMELEN are not cached.
    // Protected by direntCacheLock on SMP (same reason as inodeCacheLock).
    SpinLock direntCacheLock;
    void* direntCache;            // Ext2DirentCacheEntry[1024]; lazy-alloced

    // --- Data block read cache ---
    // Without this, every read() syscall hits disk even for blocks that
    // were just read by another process (e.g. every GTK app re-reads
    // libgtk-4.so).  Direct-mapped 8192 slots keyed on disk block
    // number.  Each slot stores a full block (blockSize bytes).
    // Invalidated on Ext2WriteBlock.  ~32 MB for 4KB blocks.
    static constexpr uint32_t BLOCK_CACHE_SLOTS = 8192;
    SpinLock blockCacheLock;
    uint32_t* blockCacheBlockNum;  // [BLOCK_CACHE_SLOTS]; 0 = empty
    uint8_t** blockCacheData;      // [BLOCK_CACHE_SLOTS]; lazy-alloced per slot

    // --- Access control ---
    // When true, Unix permission checks are skipped for this mount.
    // Used for /nix (packages must remain world-readable/executable).
    bool skipPermChecks;
};
static constexpr uint32_t EXT2_INODE_CACHE_SIZE = 1024;
static constexpr uint32_t EXT2_DIRENT_CACHE_SIZE = 1024;
static constexpr uint32_t EXT2_DIRENT_CACHE_NAMELEN = 95;
struct Ext2InodeCacheEntry {
    uint32_t ino;
    Ext2Inode data;
};
struct Ext2DirentCacheEntry {
    uint32_t parentIno;   // 0 = empty
    uint32_t childIno;
    uint8_t  nameLen;
    char     name[EXT2_DIRENT_CACHE_NAMELEN];
};

static constexpr uint32_t EXT2_OPS_PER_AUTO_SYNC = 256;

struct Ext2OpenInode {
    Ext2Mount* mnt;
    uint32_t inode;
    uint32_t holders;
    bool orphan;
    Ext2OpenInode* next;
};

// Final vnode close can run in the timer's reaper. This lock protects only
// bookkeeping; it never nests filesystem I/O, allocation or the ext2 rwlock.
static IrqSpinLock g_ext2OpenLock;
static Ext2OpenInode* g_ext2OpenInodes;

// Called with the filesystem lock held, before publishing a vnode.
static Ext2OpenInode* Ext2HoldInode(Ext2Mount* mnt, uint32_t inode)
{
    auto* candidate = static_cast<Ext2OpenInode*>(kmalloc(sizeof(Ext2OpenInode)));
    if (!candidate) return nullptr;
    *candidate = {mnt, inode, 1, false, nullptr};
    uint64_t flags = IrqSpinLockAcquire(&g_ext2OpenLock);
    for (auto* p = g_ext2OpenInodes; p; p = p->next) {
        if (p->mnt == mnt && p->inode == inode) {
            ++p->holders;
            IrqSpinLockRelease(&g_ext2OpenLock, flags);
            kfree(candidate);
            return p;
        }
    }
    candidate->next = g_ext2OpenInodes;
    g_ext2OpenInodes = candidate;
    IrqSpinLockRelease(&g_ext2OpenLock, flags);
    return candidate;
}

static void Ext2ReleaseInode(Ext2OpenInode* inode)
{
    if (!inode) return;
    bool dispose = false;
    uint64_t flags = IrqSpinLockAcquire(&g_ext2OpenLock);
    if (--inode->holders == 0 && !inode->orphan) {
        auto** link = &g_ext2OpenInodes;
        while (*link && *link != inode) link = &(*link)->next;
        if (*link) *link = inode->next;
        dispose = true;
    }
    // Zero-holder orphans remain queued until a filesystem writer/sync can
    // reclaim them. Closing from an IRQ never acquires a sleeping lock.
    IrqSpinLockRelease(&g_ext2OpenLock, flags);
    if (dispose) kfree(inode);
}

static bool Ext2MarkOpenOrphan(Ext2Mount* mnt, uint32_t inode)
{
    uint64_t flags = IrqSpinLockAcquire(&g_ext2OpenLock);
    for (auto* p = g_ext2OpenInodes; p; p = p->next) {
        if (p->mnt == mnt && p->inode == inode) {
            p->orphan = true;
            IrqSpinLockRelease(&g_ext2OpenLock, flags);
            return true;
        }
    }
    IrqSpinLockRelease(&g_ext2OpenLock, flags);
    return false;
}

// Per-open-file state
struct Ext2FilePriv {
    Ext2Mount* mnt;
    uint32_t   inode;
    Ext2Inode  inodeData;
    Ext2OpenInode* hold;
};

// Per-open-dir state
struct Ext2DirPriv {
    Ext2Mount* mnt;
    uint32_t   inode;
    Ext2Inode  inodeData;
    uint32_t   readOffset; // byte offset into directory data
    Ext2OpenInode* hold;
};

static KRwLock g_ext2Lock;
static bool    g_ext2LockInit = false;

static void EnsureLock()
{
    if (!g_ext2LockInit) { KRwLockInit(&g_ext2Lock); g_ext2LockInit = true; }
}

void Ext2ForceUnlockForPid(uint32_t pid)
{
    // RwLock doesn't support force-unlock per PID; skip for now.
    // The old KMutex force-unlock was a safety net for process cleanup.
    (void)pid;
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

// Forward declarations for block cache helpers (defined after dirent cache).
static bool Ext2BlockCacheLookup(Ext2Mount* mnt, uint32_t blockNum, void* buf);
static void Ext2BlockCachePut(Ext2Mount* mnt, uint32_t blockNum, const void* buf);

// Read a single block into buf. Checks the data block cache first.
static bool Ext2ReadBlock(Ext2Mount* mnt, uint32_t blockNum, void* buf)
{
    if (blockNum == 0) return false;
    // Fast path: serve from cache
    if (Ext2BlockCacheLookup(mnt, blockNum, buf)) return true;
    // Cache miss: read from disk
    uint64_t off = static_cast<uint64_t>(blockNum) << mnt->blockShift;
    if (!Ext2DevRead(mnt, off, buf, mnt->blockSize)) return false;
    // Populate cache with the freshly read block
    Ext2BlockCachePut(mnt, blockNum, buf);
    return true;
}

// Forward declarations for functions defined later but needed by write helpers
static bool Ext2ReadInode(Ext2Mount* mnt, uint32_t ino, Ext2Inode* out);
static uint64_t Ext2InodeSize(const Ext2Inode* ino);
static uint32_t Ext2BlockMap(Ext2Mount* mnt, const Ext2Inode* ino, uint32_t fileBlock);
static int Ext2ReadInodeData(Ext2Mount* mnt, const Ext2Inode* ino,
                             void* buf, uint64_t len, uint64_t offset);
static uint32_t Ext2DirLookup(Ext2Mount* mnt, uint32_t parentIno, const Ext2Inode* dirIno, const char* name);
static uint32_t Ext2ResolvePath(Ext2Mount* mnt, uint32_t startIno,
                                const char* path, int symlinkDepth);

// Write exactly `len` bytes to device at `byteOffset`.
static bool Ext2DevWrite(Ext2Mount* mnt, uint64_t byteOffset, const void* buf, uint64_t len)
{
    int r = mnt->dev->ops->write(mnt->dev, byteOffset, buf, len);
    return r == static_cast<int>(len);
}

// --- Inode read cache helpers ---
static inline Ext2InodeCacheEntry* Ext2InodeCacheSlot(Ext2Mount* mnt, uint32_t ino)
{
    if (!mnt->inodeCache) return nullptr;
    auto* table = static_cast<Ext2InodeCacheEntry*>(mnt->inodeCache);
    return &table[(ino - 1) & (EXT2_INODE_CACHE_SIZE - 1)];
}
static bool Ext2InodeCacheLookup(Ext2Mount* mnt, uint32_t ino, Ext2Inode* out)
{
    auto* slot = Ext2InodeCacheSlot(mnt, ino);
    if (!slot) return false;
    SpinLockAcquire(&mnt->inodeCacheLock);
    if (slot->ino != ino) { SpinLockRelease(&mnt->inodeCacheLock); return false; }
    *out = slot->data;
    SpinLockRelease(&mnt->inodeCacheLock);
    return true;
}
static void Ext2InodeCachePut(Ext2Mount* mnt, uint32_t ino, const Ext2Inode* data)
{
    auto* slot = Ext2InodeCacheSlot(mnt, ino);
    if (!slot) return;
    SpinLockAcquire(&mnt->inodeCacheLock);
    slot->ino = ino;
    slot->data = *data;
    SpinLockRelease(&mnt->inodeCacheLock);
}
static void Ext2InodeCacheInvalidate(Ext2Mount* mnt, uint32_t ino)
{
    auto* slot = Ext2InodeCacheSlot(mnt, ino);
    if (!slot) return;
    SpinLockAcquire(&mnt->inodeCacheLock);
    if (slot->ino == ino) slot->ino = 0;
    SpinLockRelease(&mnt->inodeCacheLock);
}

// --- Dirent cache helpers ---
static inline uint32_t Ext2DirentCacheHash(uint32_t parentIno, const char* name, uint32_t len)
{
    uint32_t h = parentIno * 2654435761u;
    for (uint32_t i = 0; i < len; ++i) h = h * 31u + static_cast<uint8_t>(name[i]);
    return h & (EXT2_DIRENT_CACHE_SIZE - 1);
}
static bool Ext2DirentCacheLookup(Ext2Mount* mnt, uint32_t parentIno,
                                  const char* name, uint32_t nameLen,
                                  uint32_t* outChild)
{
    if (!mnt->direntCache || nameLen == 0 || nameLen > EXT2_DIRENT_CACHE_NAMELEN) return false;
    auto* table = static_cast<Ext2DirentCacheEntry*>(mnt->direntCache);
    SpinLockAcquire(&mnt->direntCacheLock);
    auto& slot = table[Ext2DirentCacheHash(parentIno, name, nameLen)];
    if (slot.parentIno != parentIno || slot.nameLen != nameLen) {
        SpinLockRelease(&mnt->direntCacheLock);
        return false;
    }
    for (uint32_t i = 0; i < nameLen; ++i) {
        if (slot.name[i] != name[i]) {
            SpinLockRelease(&mnt->direntCacheLock);
            return false;
        }
    }
    *outChild = slot.childIno;
    SpinLockRelease(&mnt->direntCacheLock);
    return true;
}
static void Ext2DirentCachePut(Ext2Mount* mnt, uint32_t parentIno,
                               const char* name, uint32_t nameLen, uint32_t childIno)
{
    if (!mnt->direntCache || nameLen == 0 || nameLen > EXT2_DIRENT_CACHE_NAMELEN) return;
    auto* table = static_cast<Ext2DirentCacheEntry*>(mnt->direntCache);
    SpinLockAcquire(&mnt->direntCacheLock);
    auto& slot = table[Ext2DirentCacheHash(parentIno, name, nameLen)];
    slot.parentIno = parentIno;
    slot.childIno  = childIno;
    slot.nameLen   = static_cast<uint8_t>(nameLen);
    for (uint32_t i = 0; i < nameLen; ++i) slot.name[i] = name[i];
    SpinLockRelease(&mnt->direntCacheLock);
}
// Invalidate every cache entry referencing parentIno.  Called after add/remove.
static void Ext2DirentCacheInvalidateParent(Ext2Mount* mnt, uint32_t parentIno)
{
    if (!mnt->direntCache) return;
    auto* table = static_cast<Ext2DirentCacheEntry*>(mnt->direntCache);
    SpinLockAcquire(&mnt->direntCacheLock);
    for (uint32_t i = 0; i < EXT2_DIRENT_CACHE_SIZE; ++i)
        if (table[i].parentIno == parentIno) table[i].parentIno = 0;
    SpinLockRelease(&mnt->direntCacheLock);
}

// --- Data block read cache helpers ---
// Direct-mapped cache: slot = blockNum % BLOCK_CACHE_SLOTS.
// On collision, the new block replaces the old one (simple, no chaining).

// Cache hit/miss counters (atomic, lock-free) for diagnostics.
volatile uint64_t g_blockCacheHits;
volatile uint64_t g_blockCacheMisses;

static inline uint32_t Ext2BlockCacheSlot(uint32_t blockNum)
{
    return blockNum & (Ext2Mount::BLOCK_CACHE_SLOTS - 1);
}

// Look up a cached data block. Returns true and copies into `buf` on hit.
static bool Ext2BlockCacheLookup(Ext2Mount* mnt, uint32_t blockNum, void* buf)
{
    if (!mnt->blockCacheBlockNum) return false;
    uint32_t slot = Ext2BlockCacheSlot(blockNum);
    SpinLockAcquire(&mnt->blockCacheLock);
    if (mnt->blockCacheBlockNum[slot] == blockNum && mnt->blockCacheData[slot]) {
        memcpy(buf, mnt->blockCacheData[slot], mnt->blockSize);
        SpinLockRelease(&mnt->blockCacheLock);
        __atomic_fetch_add(&g_blockCacheHits, 1, __ATOMIC_RELAXED);
        return true;
    }
    SpinLockRelease(&mnt->blockCacheLock);
    __atomic_fetch_add(&g_blockCacheMisses, 1, __ATOMIC_RELAXED);
    return false;
}

// Insert a data block into the cache. Replaces any existing entry in the slot.
static void Ext2BlockCachePut(Ext2Mount* mnt, uint32_t blockNum, const void* buf)
{
    if (!mnt->blockCacheBlockNum) return;
    uint32_t slot = Ext2BlockCacheSlot(blockNum);
    SpinLockAcquire(&mnt->blockCacheLock);
    if (!mnt->blockCacheData[slot]) {
        mnt->blockCacheData[slot] = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
        if (!mnt->blockCacheData[slot]) {
            SpinLockRelease(&mnt->blockCacheLock);
            return;
        }
    }
    mnt->blockCacheBlockNum[slot] = blockNum;
    memcpy(mnt->blockCacheData[slot], buf, mnt->blockSize);
    SpinLockRelease(&mnt->blockCacheLock);
}

// Invalidate a cached data block (e.g. after a write).
static void Ext2BlockCacheInvalidate(Ext2Mount* mnt, uint32_t blockNum)
{
    if (!mnt->blockCacheBlockNum) return;
    uint32_t slot = Ext2BlockCacheSlot(blockNum);
    SpinLockAcquire(&mnt->blockCacheLock);
    if (mnt->blockCacheBlockNum[slot] == blockNum)
        mnt->blockCacheBlockNum[slot] = 0;
    SpinLockRelease(&mnt->blockCacheLock);
}

// Write a single block from buf.
static bool Ext2WriteBlock(Ext2Mount* mnt, uint32_t blockNum, const void* buf)
{
    if (blockNum == 0) return false;
    // SerialPrintf("ext2: WriteBlock %u\n", blockNum);
    uint64_t off = static_cast<uint64_t>(blockNum) << mnt->blockShift;
    // Invalidate the indirect-block read cache if we're overwriting it.
    {
        SpinLockAcquire(&mnt->indCacheLock);
        for (uint32_t s = 0; s < Ext2Mount::IND_CACHE_SLOTS; ++s) {
            if (mnt->indCacheBlockNum[s] == blockNum)
                mnt->indCacheBlockNum[s] = 0;
        }
        SpinLockRelease(&mnt->indCacheLock);
    }
    // Invalidate the data block cache.
    Ext2BlockCacheInvalidate(mnt, blockNum);
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
    bool ok = Ext2DevWrite(mnt, inodeOff, data, sizeof(Ext2Inode));
    if (ok) Ext2InodeCachePut(mnt, ino, data);
    return ok;
}

// Flush the BGDT back to disk.
static bool Ext2WriteBGDT(Ext2Mount* mnt)
{
    uint32_t bgdtSize = mnt->groupCount * sizeof(Ext2BlockGroupDesc);
    bool ok = Ext2DevWrite(mnt, mnt->bgdtDiskOff, mnt->bgdt, bgdtSize);
    if (ok) mnt->bgdtDirty = false;
    return ok;
}

// ---------------------------------------------------------------------------
// Metadata write-back cache helpers
// ---------------------------------------------------------------------------

// Return a pointer to this group's cached block bitmap, loading from disk
// the first time it's accessed.  Returns nullptr on I/O failure.
static uint8_t* Ext2GetBlockBitmap(Ext2Mount* mnt, uint32_t group)
{
    if (group >= mnt->groupCount) return nullptr;
    if (mnt->blockBitmapCache[group]) return mnt->blockBitmapCache[group];

    auto* bm = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!bm) return nullptr;
    if (!Ext2ReadBlock(mnt, mnt->bgdt[group].bg_block_bitmap, bm)) {
        kfree(bm); return nullptr;
    }
    mnt->blockBitmapCache[group] = bm;
    mnt->blockBitmapDirty[group] = false;
    return bm;
}

static uint8_t* Ext2GetInodeBitmap(Ext2Mount* mnt, uint32_t group)
{
    if (group >= mnt->groupCount) return nullptr;
    if (mnt->inodeBitmapCache[group]) return mnt->inodeBitmapCache[group];

    auto* bm = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!bm) return nullptr;
    if (!Ext2ReadBlock(mnt, mnt->bgdt[group].bg_inode_bitmap, bm)) {
        kfree(bm); return nullptr;
    }
    mnt->inodeBitmapCache[group] = bm;
    mnt->inodeBitmapDirty[group] = false;
    return bm;
}

// Flush every dirty cached bitmap + the BGDT to disk.  Called from the
// VFS sync hook, on unmount, and after every EXT2_OPS_PER_AUTO_SYNC
// metadata mutations as a safety net so the crash window is bounded.
static void Ext2Sync(Ext2Mount* mnt)
{
    if (!mnt) return;
    for (uint32_t g = 0; g < mnt->groupCount; ++g) {
        if (mnt->blockBitmapCache[g] && mnt->blockBitmapDirty[g]) {
            if (Ext2WriteBlock(mnt, mnt->bgdt[g].bg_block_bitmap,
                               mnt->blockBitmapCache[g])) {
                mnt->blockBitmapDirty[g] = false;
            }
        }
        if (mnt->inodeBitmapCache[g] && mnt->inodeBitmapDirty[g]) {
            if (Ext2WriteBlock(mnt, mnt->bgdt[g].bg_inode_bitmap,
                               mnt->inodeBitmapCache[g])) {
                mnt->inodeBitmapDirty[g] = false;
            }
        }
    }
    if (mnt->bgdtDirty && Ext2WriteBGDT(mnt)) {
        // The superblock totals must agree with the group descriptors after
        // guest allocations. Previously only the group counts were persisted.
        uint32_t totals[2] = {};
        for (uint32_t g = 0; g < mnt->groupCount; ++g) {
            totals[0] += mnt->bgdt[g].bg_free_blocks_count;
            totals[1] += mnt->bgdt[g].bg_free_inodes_count;
        }
        if (!Ext2DevWrite(mnt, 1024 + __builtin_offsetof(Ext2Superblock, s_free_blocks_count),
                         totals, sizeof(totals)))
            mnt->bgdtDirty = true;
    }
    mnt->pendingMetaOps = 0;
}

// Called from each mutating op to bump the counter + trigger periodic flush.
static inline void Ext2BumpMetaOps(Ext2Mount* mnt)
{
    if (++mnt->pendingMetaOps >= EXT2_OPS_PER_AUTO_SYNC)
        Ext2Sync(mnt);
}

// ---------------------------------------------------------------------------
// Block and inode allocation
// ---------------------------------------------------------------------------

// Allocate a free block from the bitmap. Returns block number or 0 on failure.
static uint32_t Ext2AllocBlock(Ext2Mount* mnt)
{
    for (uint32_t g = 0; g < mnt->groupCount; ++g) {
        if (mnt->bgdt[g].bg_free_blocks_count == 0) continue;

        uint8_t* bitmap = Ext2GetBlockBitmap(mnt, g);
        if (!bitmap) continue;

        uint32_t blocksInGroup = mnt->blocksPerGroup;
        if (g == mnt->groupCount - 1)
            blocksInGroup = mnt->totalBlocks - g * mnt->blocksPerGroup;

        uint32_t start = mnt->nextFreeBlockHint ? mnt->nextFreeBlockHint[g] : 0;
        if (start >= blocksInGroup) start = 0;
        for (uint32_t pass = 0; pass < 2; ++pass) {
            uint32_t end = (pass == 0) ? blocksInGroup : start;
            uint32_t bit = (pass == 0) ? start : 0;
            // 64-bit word-at-a-time scan: skip 64 bits at once when fully set.
            while (bit < end) {
                // Align to qword boundary for fast scanning
                if ((bit & 63) == 0 && bit + 64 <= end) {
                    uint32_t qwordIdx = bit / 64;
                    uint64_t word = reinterpret_cast<const uint64_t*>(bitmap)[qwordIdx];
                    if (word == ~uint64_t{0}) { bit += 64; continue; }
                }
                uint32_t byteIdx = bit / 8;
                uint8_t  byte    = bitmap[byteIdx];
                if (byte == 0xFF) { bit = (byteIdx + 1) * 8; continue; }
                uint32_t bitInByte = bit & 7;
                if (!(byte & (1u << bitInByte))) {
                    bitmap[byteIdx] = byte | (uint8_t)(1u << bitInByte);
                    mnt->blockBitmapDirty[g] = true;
                    mnt->bgdt[g].bg_free_blocks_count--;
                    mnt->bgdtDirty = true;
                    Ext2BumpMetaOps(mnt);
                    if (mnt->nextFreeBlockHint) mnt->nextFreeBlockHint[g] = bit + 1;
                    return g * mnt->blocksPerGroup + bit + mnt->firstDataBlock;
                }
                ++bit;
            }
            if (start == 0) break; // pass-0 covered everything
        }
        // Group claims free blocks but bitmap shows none — clear hint
        // and trust the next group.
        if (mnt->nextFreeBlockHint) mnt->nextFreeBlockHint[g] = blocksInGroup;
    }
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

    uint8_t* bitmap = Ext2GetBlockBitmap(mnt, g);
    if (!bitmap) return false;
    if (!(bitmap[bit / 8] & (1 << (bit % 8)))) return true;
    bitmap[bit / 8] &= ~(1 << (bit % 8));
    mnt->blockBitmapDirty[g] = true;
    mnt->bgdt[g].bg_free_blocks_count++;
    mnt->bgdtDirty = true;
    Ext2BumpMetaOps(mnt);
    if (mnt->nextFreeBlockHint && bit < mnt->nextFreeBlockHint[g])
        mnt->nextFreeBlockHint[g] = bit;
    return true;
}

// Allocate a free inode. Returns inode number or 0 on failure.
// isDir: increment bg_used_dirs_count.
static uint32_t Ext2AllocInode(Ext2Mount* mnt, bool isDir)
{
    for (uint32_t g = 0; g < mnt->groupCount; ++g) {
        if (mnt->bgdt[g].bg_free_inodes_count == 0) continue;

        uint8_t* bitmap = Ext2GetInodeBitmap(mnt, g);
        if (!bitmap) continue;

        uint32_t start = mnt->nextFreeInodeHint ? mnt->nextFreeInodeHint[g] : 0;
        if (start >= mnt->inodesPerGroup) start = 0;
        for (uint32_t pass = 0; pass < 2; ++pass) {
            uint32_t end = (pass == 0) ? mnt->inodesPerGroup : start;
            uint32_t bit = (pass == 0) ? start : 0;
            while (bit < end) {
                // 64-bit word scan: skip 64 bits at once when fully set
                if ((bit & 63) == 0 && bit + 64 <= end) {
                    uint32_t qwordIdx = bit / 64;
                    uint64_t word = reinterpret_cast<const uint64_t*>(bitmap)[qwordIdx];
                    if (word == ~uint64_t{0}) { bit += 64; continue; }
                }
                uint32_t byteIdx = bit / 8;
                uint8_t  byte    = bitmap[byteIdx];
                if (byte == 0xFF) { bit = (byteIdx + 1) * 8; continue; }
                uint32_t bitInByte = bit & 7;
                if (!(byte & (1u << bitInByte))) {
                    bitmap[byteIdx] = byte | (uint8_t)(1u << bitInByte);
                    mnt->inodeBitmapDirty[g] = true;
                    mnt->bgdt[g].bg_free_inodes_count--;
                    if (isDir) mnt->bgdt[g].bg_used_dirs_count++;
                    mnt->bgdtDirty = true;
                    Ext2BumpMetaOps(mnt);
                    if (mnt->nextFreeInodeHint) mnt->nextFreeInodeHint[g] = bit + 1;
                    return g * mnt->inodesPerGroup + bit + 1;
                }
                ++bit;
            }
            if (start == 0) break;
        }
        if (mnt->nextFreeInodeHint) mnt->nextFreeInodeHint[g] = mnt->inodesPerGroup;
    }
    return 0;
}

// Free an inode back to the bitmap.
static bool Ext2FreeInode(Ext2Mount* mnt, uint32_t ino, bool isDir)
{
    if (ino == 0) return false;
    uint32_t g = (ino - 1) / mnt->inodesPerGroup;
    uint32_t bit = (ino - 1) % mnt->inodesPerGroup;
    if (g >= mnt->groupCount) return false;

    uint8_t* bitmap = Ext2GetInodeBitmap(mnt, g);
    if (!bitmap) return false;
    if (!(bitmap[bit / 8] & (1 << (bit % 8)))) return true;
    bitmap[bit / 8] &= ~(1 << (bit % 8));
    mnt->inodeBitmapDirty[g] = true;
    mnt->bgdt[g].bg_free_inodes_count++;
    if (isDir) mnt->bgdt[g].bg_used_dirs_count--;
    mnt->bgdtDirty = true;
    Ext2BumpMetaOps(mnt);
    if (mnt->nextFreeInodeHint && bit < mnt->nextFreeInodeHint[g])
        mnt->nextFreeInodeHint[g] = bit;
    Ext2InodeCacheInvalidate(mnt, ino);
    Ext2DirentCacheInvalidateParent(mnt, ino);
    return true;
}

// ---------------------------------------------------------------------------
// Write helpers: inode data write, block assignment
// ---------------------------------------------------------------------------

// Ensure inode has a block mapped at `fileBlock`. Allocates if needed.
// Returns disk block number or 0 on failure.
// Forward declaration: defined further down with the read path.
static uint32_t Ext2ReadIndPointer(Ext2Mount* mnt, uint32_t indBlock, uint32_t idx);

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
        ino->i_blocks += mnt->blockSize / 512;
        // Caller must write the inode back when its overall update is done.
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
            memset(zb, 0, mnt->blockSize);
            Ext2WriteBlock(mnt, nb, zb);
            kfree(zb);
            ino->i_block[12] = nb;
            ino->i_blocks += mnt->blockSize / 512;
        }
        // Use the indirect-block read cache (fed by Ext2ReadIndPointer)
        // to avoid hitting the disk every time we extend a file in
        // indirect-block range.
        uint32_t entry = Ext2ReadIndPointer(mnt, ino->i_block[12], fileBlock);
        if (entry) return entry;
        uint32_t nb = Ext2AllocBlock(mnt);
        if (!nb) return 0;
        uint64_t off = (static_cast<uint64_t>(ino->i_block[12]) << mnt->blockShift)
                       + fileBlock * 4;
        // Update cache if the indirect block is currently cached.
        {
            SpinLockAcquire(&mnt->indCacheLock);
            for (uint32_t s = 0; s < Ext2Mount::IND_CACHE_SLOTS; ++s) {
                if (mnt->indCacheBlockNum[s] == ino->i_block[12] && mnt->indCacheData[s]) {
                    *reinterpret_cast<uint32_t*>(mnt->indCacheData[s] + fileBlock * 4) = nb;
                    break;
                }
            }
            SpinLockRelease(&mnt->indCacheLock);
        }
        Ext2DevWrite(mnt, off, &nb, 4);
        ino->i_blocks += mnt->blockSize / 512;
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
            memset(zb, 0, mnt->blockSize);
            Ext2WriteBlock(mnt, nb, zb);
            kfree(zb);
            ino->i_block[13] = nb;
            ino->i_blocks += mnt->blockSize / 512;
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
            memset(zb, 0, mnt->blockSize);
            Ext2WriteBlock(mnt, indBlock, zb);
            kfree(zb);
            Ext2DevWrite(mnt, off1, &indBlock, 4);
            ino->i_blocks += mnt->blockSize / 512;
        }

        uint64_t off2 = (static_cast<uint64_t>(indBlock) << mnt->blockShift) + idx2 * 4;
        uint32_t entry = 0;
        Ext2DevRead(mnt, off2, &entry, 4);
        if (entry) return entry;
        uint32_t nb = Ext2AllocBlock(mnt);
        if (!nb) return 0;
        Ext2DevWrite(mnt, off2, &nb, 4);
        ino->i_blocks += mnt->blockSize / 512;
        return nb;
    }

    // Triply indirect not implemented for writes yet
    return 0;
}

// Write `len` bytes to inode data at `offset`. Returns bytes written.
//
// Performance: coalesces runs of contiguous full-block writes into a
// single Ext2DevWrite, bypassing per-block cache invalidation overhead.
// For typical sequential writes (nix-install extracting large files),
// ext2 allocates blocks contiguously, so the run often spans the whole
// request — collapsing many per-block virtio round-trips into one.
static int Ext2WriteInodeData(Ext2Mount* mnt, Ext2Inode* ino, uint32_t inoNum,
                              const void* buf, uint64_t len, uint64_t offset)
{
    auto* src = static_cast<const uint8_t*>(buf);
    uint64_t bytesWritten = 0;

    // Cap run-coalesce length to match read path / virtio DMA buffer.
    static constexpr uint64_t MAX_RUN_BYTES = 64 * 1024;

    auto* blockBuf = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!blockBuf) return -1;

    while (bytesWritten < len) {
        uint32_t fileBlock = static_cast<uint32_t>((offset + bytesWritten) >> mnt->blockShift);
        uint32_t blockOff  = static_cast<uint32_t>((offset + bytesWritten) & (mnt->blockSize - 1));
        uint32_t diskBlock = Ext2EnsureBlock(mnt, ino, inoNum, fileBlock);
        if (!diskBlock) break;

        uint32_t avail = mnt->blockSize - blockOff;
        uint64_t remaining = len - bytesWritten;

        // Full-block write: try to coalesce a run of contiguous blocks
        if (blockOff == 0 && remaining >= mnt->blockSize) {
            uint32_t runBlocks = 1;
            uint32_t maxRun = static_cast<uint32_t>(remaining >> mnt->blockShift);
            if (maxRun > (MAX_RUN_BYTES >> mnt->blockShift))
                maxRun = MAX_RUN_BYTES >> mnt->blockShift;

            while (runBlocks < maxRun) {
                uint32_t nextDb = Ext2EnsureBlock(mnt, ino, inoNum, fileBlock + runBlocks);
                if (nextDb != diskBlock + runBlocks) break;
                ++runBlocks;
            }

            uint64_t runBytes = static_cast<uint64_t>(runBlocks) << mnt->blockShift;
            uint64_t runOff   = static_cast<uint64_t>(diskBlock) << mnt->blockShift;

            // Invalidate caches for all blocks in the run
            for (uint32_t b = 0; b < runBlocks; ++b)
                Ext2BlockCacheInvalidate(mnt, diskBlock + b);

            // Batch-invalidate indirect-block cache entries
            SpinLockAcquire(&mnt->indCacheLock);
            for (uint32_t s = 0; s < Ext2Mount::IND_CACHE_SLOTS; ++s) {
                uint32_t bn = mnt->indCacheBlockNum[s];
                if (bn >= diskBlock && bn < diskBlock + runBlocks)
                    mnt->indCacheBlockNum[s] = 0;
            }
            SpinLockRelease(&mnt->indCacheLock);

            if (!Ext2DevWrite(mnt, runOff, src + bytesWritten, runBytes)) break;
            bytesWritten += runBytes;
        } else {
            // Partial block: read-modify-write via bounce buffer.
            uint64_t toCopy = remaining;
            if (toCopy > avail) toCopy = avail;

            if (blockOff == 0 && toCopy == mnt->blockSize) {
                // Full block but couldn't coalesce (shouldn't normally happen)
                Ext2BlockCacheInvalidate(mnt, diskBlock);
                if (!Ext2DevWrite(mnt, static_cast<uint64_t>(diskBlock) << mnt->blockShift,
                                  src + bytesWritten, mnt->blockSize))
                    break;
            } else {
                Ext2ReadBlock(mnt, diskBlock, blockBuf);
                memcpy(blockBuf + blockOff, src + bytesWritten, toCopy);
                if (!Ext2WriteBlock(mnt, diskBlock, blockBuf)) break;
            }
            bytesWritten += toCopy;
        }
    }

    kfree(blockBuf);

    // Update inode size if we extended the file, and unconditionally
    // write the inode back so any newly-allocated i_block[] entries
    // assigned by Ext2EnsureBlock are persisted (it no longer flushes
    // the inode itself per allocation).
    uint64_t newEnd = offset + bytesWritten;
    if (newEnd > ino->i_size) {
        ino->i_size = static_cast<uint32_t>(newEnd);
    }
    if (bytesWritten) Ext2Touch(ino);
    Ext2WriteInode(mnt, inoNum, ino);

    return static_cast<int>(bytesWritten);
}

// Free all data blocks of an inode (direct + indirect).
struct Ext2FreeBatch {
    Ext2FreeBatch* next;
    uint32_t count;
    uint32_t blocks[256];
};

static void Ext2DisposeFreeBatch(Ext2FreeBatch* batch)
{
    while (batch) { auto* next = batch->next; kfree(batch); batch = next; }
}

static bool Ext2CollectFreeTree(Ext2Mount* mnt, uint32_t block, unsigned depth,
                                Ext2FreeBatch** batch)
{
    if (!block) return true;
    if (block < mnt->firstDataBlock || block >= mnt->totalBlocks) return false;
    uint32_t rel = block - mnt->firstDataBlock;
    uint8_t* bitmap = Ext2GetBlockBitmap(mnt, rel / mnt->blocksPerGroup);
    uint32_t bit = rel % mnt->blocksPerGroup;
    if (!bitmap || !(bitmap[bit / 8] & (1 << (bit % 8)))) return false;
    if (depth) {
        auto* entries = static_cast<uint32_t*>(kmalloc(mnt->blockSize));
        if (!entries) return false;
        if (!Ext2ReadBlock(mnt, block, entries)) { kfree(entries); return false; }
        for (uint32_t i = 0; i < mnt->blockSize / sizeof(uint32_t); ++i) {
            if (!Ext2CollectFreeTree(mnt, entries[i], depth - 1, batch)) {
                kfree(entries);
                return false;
            }
        }
        kfree(entries);
    }
    if (!*batch || (*batch)->count == 256) {
        auto* chunk = static_cast<Ext2FreeBatch*>(kmalloc(sizeof(Ext2FreeBatch)));
        if (!chunk) return false;
        chunk->next = *batch;
        chunk->count = 0;
        *batch = chunk;
    }
    (*batch)->blocks[(*batch)->count++] = block;
    return true;
}

static bool Ext2FreeInodeBlocks(Ext2Mount* mnt, uint32_t number, Ext2Inode* inode)
{
    Ext2FreeBatch* batch = nullptr;
    // Fast symlinks store bytes, not disk block numbers, in i_block[].
    bool fastSymlink = (inode->i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK && inode->i_blocks == 0;
    if (fastSymlink) {
        if (Ext2InodeSize(inode) > sizeof(inode->i_block)) return false;
    } else {
        for (unsigned i = 0; i < 15; ++i) {
            if (!Ext2CollectFreeTree(mnt, inode->i_block[i], i < 12 ? 0 : i - 11, &batch)) {
                Ext2DisposeFreeBatch(batch);
                return false;
            }
        }
    }
    // Preload the inode bitmap too: bitmap release below needs no allocation
    // or reads. Persist detached pointers BEFORE any block becomes reusable.
    if (!Ext2GetInodeBitmap(mnt, (number - 1) / mnt->inodesPerGroup)) {
        Ext2DisposeFreeBatch(batch);
        return false;
    }
    Ext2Inode cleared = *inode;
    memset(cleared.i_block, 0, sizeof(cleared.i_block));
    cleared.i_blocks = 0;
    cleared.i_size = 0;
    if ((cleared.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG) cleared.i_dir_acl = 0;
    if (!Ext2WriteInode(mnt, number, &cleared)) {
        Ext2DisposeFreeBatch(batch);
        return false;
    }
    *inode = cleared;
    for (auto* chunk = batch; chunk; chunk = chunk->next)
        for (uint32_t i = 0; i < chunk->count; ++i)
            Ext2FreeBlock(mnt, chunk->blocks[i]);
    Ext2DisposeFreeBatch(batch);
    return true;
}

static bool Ext2ReclaimInode(Ext2Mount* mnt, uint32_t ino, Ext2Inode* data)
{
    if (!Ext2FreeInodeBlocks(mnt, ino, data)) return false;
    data->i_dtime = Ext2Now();
    if (!data->i_dtime) data->i_dtime = 1;
    if (!Ext2WriteInode(mnt, ino, data)) return false;
    return Ext2FreeInode(mnt, ino, (data->i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
}

// Called only at top-level filesystem write/sync boundaries, never from
// bitmap auto-sync or an IRQ close. Each queued inode remains allocated.
static void Ext2DrainOrphans(Ext2Mount* mnt)
{
    for (;;) {
        uint64_t flags = IrqSpinLockAcquire(&g_ext2OpenLock);
        auto** link = &g_ext2OpenInodes;
        while (*link && !((*link)->mnt == mnt && (*link)->orphan && (*link)->holders == 0))
            link = &(*link)->next;
        auto* orphan = *link;
        if (orphan) *link = orphan->next;
        IrqSpinLockRelease(&g_ext2OpenLock, flags);
        if (!orphan) return;
        Ext2Inode data;
        bool read = Ext2ReadInode(mnt, orphan->inode, &data);
        if (read && data.i_links_count) {
            // Namespace removal committed but its metadata write failed.
            // The queued orphan records the intended final link count.
            data.i_links_count = 0;
            data.i_ctime = Ext2Now();
            read = Ext2WriteInode(mnt, orphan->inode, &data);
        }
        if (!read || !Ext2ReclaimInode(mnt, orphan->inode, &data)) {
            flags = IrqSpinLockAcquire(&g_ext2OpenLock);
            orphan->next = g_ext2OpenInodes;
            g_ext2OpenInodes = orphan;
            IrqSpinLockRelease(&g_ext2OpenLock, flags);
            SerialPrintf("ext2: failed reclaiming orphan inode %u\n", orphan->inode);
            return;
        }
        kfree(orphan);
    }
}

static int Ext2DropLink(Ext2Mount* mnt, uint32_t ino, Ext2Inode* data, bool directory)
{
    if (!data->i_links_count) return -5;
    if (directory) data->i_links_count = 0; // removed parent entry and '.'
    else --data->i_links_count;
    data->i_ctime = Ext2Now();
    bool deferred = !data->i_links_count && Ext2MarkOpenOrphan(mnt, ino);
    if (!Ext2WriteInode(mnt, ino, data)) return -5;
    if (data->i_links_count || deferred) return 0;
    return Ext2ReclaimInode(mnt, ino, data) ? 0 : -5;
}

// ---------------------------------------------------------------------------
// Directory manipulation
// ---------------------------------------------------------------------------

// Add a directory entry to a directory inode.
static bool Ext2PrepareDirectoryWrite(Ext2Mount* mnt, uint32_t dirIno, Ext2Inode* dirData)
{
    // Reads scan every directory block. Before a linear mutation, retire any
    // host-created HTree index: its root/node payload is hidden by directory
    // record lengths, and all leaf entries remain valid for linear scanning.
    // Otherwise adding an entry to root slack destroys the still-advertised
    // index and the next e2fsck reports a corrupt HTree root.
    constexpr uint32_t EXT2_INDEX_FL = 0x1000;
    if (!(dirData->i_flags & EXT2_INDEX_FL)) return true;
    dirData->i_flags &= ~EXT2_INDEX_FL;
    return Ext2WriteInode(mnt, dirIno, dirData);
}

static bool Ext2DirAdd(Ext2Mount* mnt, uint32_t dirIno, Ext2Inode* dirData,
                       uint32_t childIno, const char* name, uint8_t fileType, bool* committed = nullptr)
{
    if (committed) *committed = false;
    uint32_t nameLen = 0;
    for (const char* p = name; *p; ++p) ++nameLen;
    if (nameLen == 0 || nameLen > 255) return false;
    if (!Ext2PrepareDirectoryWrite(mnt, dirIno, dirData)) return false;

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
        if (!diskBlock || !Ext2ReadBlock(mnt, diskBlock, blockBuf)) { kfree(blockBuf); return false; }

        uint32_t pos = 0;
        while (pos < mnt->blockSize) {
            auto* de = reinterpret_cast<Ext2DirEntry2*>(blockBuf + pos);
            if (!Ext2DirEntryValid(de, pos, mnt->blockSize)) { kfree(blockBuf); return false; }

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

                bool written = Ext2WriteBlock(mnt, diskBlock, blockBuf);
                kfree(blockBuf);
                if (!written) return false;
                if (committed) *committed = true;
                Ext2Touch(dirData);
                bool metadataWritten = Ext2WriteInode(mnt, dirIno, dirData);
                Ext2DirentCacheInvalidateParent(mnt, dirIno);
                return metadataWritten;
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
    memset(blockBuf, 0, mnt->blockSize);
    auto* newDe = reinterpret_cast<Ext2DirEntry2*>(blockBuf);
    newDe->inode = childIno;
    newDe->rec_len = static_cast<uint16_t>(mnt->blockSize); // spans entire block
    newDe->name_len = static_cast<uint8_t>(nameLen);
    newDe->file_type = fileType;
    for (uint32_t i = 0; i < nameLen; ++i) newDe->name[i] = name[i];

    bool written = Ext2WriteBlock(mnt, newBlock, blockBuf);
    kfree(blockBuf);
    if (!written) return false;
    if (committed) *committed = true;

    // Update directory size
    dirData->i_size = static_cast<uint32_t>(dirSize + mnt->blockSize);
    Ext2Touch(dirData);
    bool metadataWritten = Ext2WriteInode(mnt, dirIno, dirData);
    Ext2DirentCacheInvalidateParent(mnt, dirIno);
    return metadataWritten;
}

// Remove a directory entry by name. Returns the removed inode number or 0.
static uint32_t Ext2DirRemove(Ext2Mount* mnt, Ext2Inode* dirData,
                              uint32_t dirIno, const char* name, bool* committed = nullptr)
{
    if (committed) *committed = false;
    if (!Ext2PrepareDirectoryWrite(mnt, dirIno, dirData)) return 0;
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
            if (!Ext2DirEntryValid(de, pos, mnt->blockSize)) break;

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
                    bool written = Ext2WriteBlock(mnt, diskBlock, blockBuf);
                    kfree(blockBuf);
                    if (!written) return 0;
                    if (committed) *committed = true;
                    Ext2Touch(dirData);
                    bool metadataWritten = Ext2WriteInode(mnt, dirIno, dirData);
                    Ext2DirentCacheInvalidateParent(mnt, dirIno);
                    return metadataWritten ? removedIno : 0;
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
    const char* p = relPath;
    while (*p == '/') ++p; // skip leading slashes

    // Compute the effective end of the path, ignoring any trailing slashes.
    // POSIX treats mkdir("foo/") / rename("foo/") as operating on "foo"; without
    // this the leaf component parses as empty and every create op on a
    // trailing-slash path fails (BRO-195: SuperTuxKart's cache-dir mkdir loop).
    const char* end = p;
    while (*end) ++end;
    while (end > p && end[-1] == '/') --end;

    // Find the last slash within [p, end).
    const char* lastSlash = nullptr;
    for (const char* q = p; q < end; ++q)
        if (*q == '/') lastSlash = q;

    if (!lastSlash) {
        // No slash — parent is root, name is the entire (trimmed) path
        uint32_t i = 0;
        while (p + i < end && i < nameOutSize - 1) { nameOut[i] = p[i]; ++i; }
        nameOut[i] = '\0';
        return EXT2_ROOT_INO;
    }

    // Copy parent path (everything before the last slash)
    uint32_t parentLen = static_cast<uint32_t>(lastSlash - p);
    char parentPath[256];
    if (parentLen >= sizeof(parentPath)) return 0;
    for (uint32_t i = 0; i < parentLen; ++i) parentPath[i] = p[i];
    parentPath[parentLen] = '\0';

    // Copy name (after last slash, up to the trimmed end)
    const char* name = lastSlash + 1;
    while (name < end && *name == '/') ++name;
    uint32_t ni = 0;
    while (name + ni < end && name[ni] && ni < nameOutSize - 1) { nameOut[ni] = name[ni]; ++ni; }
    nameOut[ni] = '\0';

    return Ext2ResolvePath(mnt, EXT2_ROOT_INO, parentPath, 0);
}

// ---------------------------------------------------------------------------
// Inode operations
// ---------------------------------------------------------------------------

static bool Ext2ReadInode(Ext2Mount* mnt, uint32_t ino, Ext2Inode* out)
{
    if (ino == 0) return false;
    if (Ext2InodeCacheLookup(mnt, ino, out)) return true;
    uint32_t group = (ino - 1) / mnt->inodesPerGroup;
    uint32_t index = (ino - 1) % mnt->inodesPerGroup;
    if (group >= mnt->groupCount) return false;

    uint64_t tableOff = static_cast<uint64_t>(mnt->bgdt[group].bg_inode_table)
                        << mnt->blockShift;
    uint64_t inodeOff = tableOff + static_cast<uint64_t>(index) * mnt->inodeSize;
    if (!Ext2DevRead(mnt, inodeOff, out, sizeof(Ext2Inode))) return false;
    Ext2InodeCachePut(mnt, ino, out);
    return true;
}

static uint64_t Ext2InodeSize(const Ext2Inode* ino)
{
    return ino->i_size; // For files >4GB we'd use i_dir_acl, but read-only ext2 is fine with 32-bit
}

// Read a 4-byte block-pointer from an indirect block, using a per-mount
// one-slot cache to avoid re-reading the same indirect block during
// sequential file scans (BlockMap is called per file-block).
static uint32_t Ext2ReadIndPointer(Ext2Mount* mnt, uint32_t indBlock, uint32_t idx)
{
    if (!indBlock) return 0;
    if (indBlock >= mnt->totalBlocks) return 0; // out-of-range block pointer
    uint32_t entry = 0;
    SpinLockAcquire(&mnt->indCacheLock);

    // Search all cache slots
    for (uint32_t s = 0; s < Ext2Mount::IND_CACHE_SLOTS; ++s) {
        if (mnt->indCacheBlockNum[s] == indBlock && mnt->indCacheData[s]) {
            entry = *reinterpret_cast<uint32_t*>(mnt->indCacheData[s] + idx * 4);
            SpinLockRelease(&mnt->indCacheLock);
            return entry;
        }
    }

    // Cache miss — pick eviction slot (round-robin)
    uint32_t victim = mnt->indCacheLru % Ext2Mount::IND_CACHE_SLOTS;
    mnt->indCacheLru++;

    if (!mnt->indCacheData[victim]) {
        mnt->indCacheData[victim] = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
        if (!mnt->indCacheData[victim]) {
            SpinLockRelease(&mnt->indCacheLock);
            // Fall back to direct read
            uint64_t off = (static_cast<uint64_t>(indBlock) << mnt->blockShift) + idx * 4;
            if (!Ext2DevRead(mnt, off, &entry, 4)) return 0;
            return entry;
        }
    }

    uint64_t blockOff = static_cast<uint64_t>(indBlock) << mnt->blockShift;
    if (!Ext2DevRead(mnt, blockOff, mnt->indCacheData[victim], mnt->blockSize)) {
        mnt->indCacheBlockNum[victim] = 0;
        SpinLockRelease(&mnt->indCacheLock);
        return 0;
    }
    mnt->indCacheBlockNum[victim] = indBlock;
    entry = *reinterpret_cast<uint32_t*>(mnt->indCacheData[victim] + idx * 4);
    SpinLockRelease(&mnt->indCacheLock);
    return entry;
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
        return Ext2ReadIndPointer(mnt, ino->i_block[12], fileBlock);
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
        uint32_t indBlock = Ext2ReadIndPointer(mnt, dindBlock, idx1);
        if (!indBlock) {
            SerialPrintf("ext2: dind L1 fail idx1=%u dindBlock=%u\n", idx1, dindBlock);
            return 0;
        }
        uint32_t entry = Ext2ReadIndPointer(mnt, indBlock, idx2);
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
    uint32_t dindB = Ext2ReadIndPointer(mnt, tindBlock, idx1);
    if (!dindB) return 0;
    uint32_t indB = Ext2ReadIndPointer(mnt, dindB, idx2);
    if (!indB) return 0;
    return Ext2ReadIndPointer(mnt, indB, idx3);
}

// Read `len` bytes from inode data at `offset`. Returns bytes read.
//
// Performance: coalesces runs of contiguous full file-blocks into a
// single Ext2DevRead, reading directly into the caller's buffer
// (skipping the bounce buffer). For typical large files (libgtk-4.so
// etc.) the underlying ext2 allocator places blocks contiguously, so
// the run usually spans the whole request — collapsing dozens of
// per-block virtio round-trips into a single 64KB transfer.
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

    // Cap run-coalesce length at the underlying device's preferred
    // burst (matches virtio-blk's persistent 64KB DMA buffer).
    static constexpr uint64_t MAX_RUN_BYTES = 64 * 1024;

    // Bounce buffer used only for unaligned head/tail partial blocks.
    uint8_t* blockBuf = nullptr;
    auto getBounce = [&]() -> uint8_t* {
        if (!blockBuf) blockBuf = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
        return blockBuf;
    };

    while (bytesRead < len) {
        uint32_t fileBlock = static_cast<uint32_t>((offset + bytesRead) >> mnt->blockShift);
        uint32_t blockOff  = static_cast<uint32_t>((offset + bytesRead) & (mnt->blockSize - 1));
        uint32_t diskBlock = Ext2BlockMap(mnt, ino, fileBlock);

        uint64_t avail = mnt->blockSize - blockOff;
        uint64_t remaining = len - bytesRead;

        // Sparse hole: read returns zeros (POSIX semantics; essential
        // for ELF shared libraries where fuse2fs preserves zero
        // alignment padding as sparse blocks on disk).
        if (!diskBlock) {
            uint64_t toCopy = avail < remaining ? avail : remaining;
            memset(dst + bytesRead, 0, toCopy);
            bytesRead += toCopy;
            continue;
        }

        // Unaligned head OR final partial block: use bounce buffer for
        // one block, copy the slice we need.
        if (blockOff != 0 || remaining < mnt->blockSize) {
            uint8_t* bb = getBounce();
            if (!bb) break;
            if (!Ext2ReadBlock(mnt, diskBlock, bb)) {
                SerialPrintf("ext2: ReadBlock failed for diskBlock=%u (fileBlock=%u)\n",
                             diskBlock, fileBlock);
                break;
            }
            uint64_t toCopy = avail < remaining ? avail : remaining;
            memcpy(dst + bytesRead, bb + blockOff, toCopy);
            bytesRead += toCopy;
            continue;
        }

        // Aligned to a block boundary AND caller wants at least one
        // full block. Find the longest run of contiguous file-blocks
        // mapping to contiguous disk-blocks (capped at MAX_RUN_BYTES
        // and at the data still requested).
        //
        // Before hitting disk, check if the first block is cached.
        // If so, serve it from cache (breaks the run, but avoids I/O).
        if (Ext2BlockCacheLookup(mnt, diskBlock, dst + bytesRead)) {
            bytesRead += mnt->blockSize;
            continue;
        }

        uint32_t runBlocks = 1;
        uint32_t maxRun = static_cast<uint32_t>(remaining >> mnt->blockShift);
        if (maxRun > (MAX_RUN_BYTES >> mnt->blockShift))
            maxRun = MAX_RUN_BYTES >> mnt->blockShift;
        while (runBlocks < maxRun) {
            uint32_t nextDb = Ext2BlockMap(mnt, ino, fileBlock + runBlocks);
            if (nextDb != diskBlock + runBlocks) break; // sparse hole or non-contiguous
            ++runBlocks;
        }

        uint64_t runBytes = static_cast<uint64_t>(runBlocks) << mnt->blockShift;
        uint64_t runOff   = static_cast<uint64_t>(diskBlock) << mnt->blockShift;
        if (!Ext2DevRead(mnt, runOff, dst + bytesRead, runBytes)) {
            SerialPrintf("ext2: coalesced read failed off=%lu len=%lu\n",
                         runOff, runBytes);
            break;
        }
        // Populate the block cache with each block from the coalesced read.
        for (uint32_t b = 0; b < runBlocks; ++b) {
            Ext2BlockCachePut(mnt, diskBlock + b,
                              dst + bytesRead + (static_cast<uint64_t>(b) << mnt->blockShift));
        }
        bytesRead += runBytes;
    }

    if (blockBuf) kfree(blockBuf);
    return static_cast<int>(bytesRead);
}

// ---------------------------------------------------------------------------
// Path resolution (with symlink following)
// ---------------------------------------------------------------------------

// Look up a name in a directory inode. Returns the inode number or 0 on failure.
static uint32_t Ext2DirLookup(Ext2Mount* mnt, uint32_t parentIno, const Ext2Inode* dirIno, const char* name)
{
    uint64_t dirSize = Ext2InodeSize(dirIno);
    if (dirSize == 0) return 0;

    uint32_t nameLen = 0;
    for (const char* p = name; *p; ++p) ++nameLen;

    uint32_t cached = 0;
    if (parentIno && Ext2DirentCacheLookup(mnt, parentIno, name, nameLen, &cached))
        return cached;

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
            if (!Ext2DirEntryValid(de, pos, mnt->blockSize)) break;
            if (de->inode != 0 && de->name_len == nameLen) {
                bool match = true;
                for (uint32_t i = 0; i < nameLen; ++i) {
                    if (de->name[i] != name[i]) { match = false; break; }
                }
                if (match) {
                    uint32_t result = de->inode;
                    kfree(buf);
                    if (parentIno) Ext2DirentCachePut(mnt, parentIno, name, nameLen, result);
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
        memcpy(target, src, size);
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
        // Normal ENOTDIR — don't spam serial for routine path probes
        return 0;
    }

    uint32_t childIno = Ext2DirLookup(mnt, curIno, &dirIno, component);
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
    KRwLockReadLock(&g_ext2Lock);
    Ext2Inode data;
    if (!Ext2ReadInode(fp->mnt, fp->inode, &data)) {
        KRwLockReadUnlock(&g_ext2Lock); return -5;
    }
    int r = Ext2ReadInodeData(fp->mnt, &data, buf, len, *offset);
    if (r > 0) *offset += r;
    KRwLockReadUnlock(&g_ext2Lock);
    // Relatime: avoid a metadata write for every read. Re-read under the
    // exclusive lock so a concurrent write/utimens cannot be overwritten.
    uint32_t now = Ext2Now();
    if (r > 0 && data.i_atime != now &&
        (data.i_atime <= data.i_mtime || data.i_atime <= data.i_ctime ||
         (now > data.i_atime && now - data.i_atime >= 86400))) {
        KRwLockWriteLock(&g_ext2Lock);
        if (Ext2ReadInode(fp->mnt, fp->inode, &data) && data.i_atime != now &&
            (data.i_atime <= data.i_mtime || data.i_atime <= data.i_ctime ||
             (now > data.i_atime && now - data.i_atime >= 86400))) {
            data.i_atime = now;
            Ext2WriteInode(fp->mnt, fp->inode, &data);
        }
        KRwLockWriteUnlock(&g_ext2Lock);
    }
    return r;
}

static int Ext2FileWrite(Vnode* vn, const void* buf, uint64_t len, uint64_t* offset)
{
    auto* fp = static_cast<Ext2FilePriv*>(vn->priv);
    KRwLockWriteLock(&g_ext2Lock);
    if (!Ext2ReadInode(fp->mnt, fp->inode, &fp->inodeData)) {
        KRwLockWriteUnlock(&g_ext2Lock); return -5;
    }
    int r = Ext2WriteInodeData(fp->mnt, &fp->inodeData, fp->inode, buf, len, *offset);
    if (r > 0) *offset += r;
    KRwLockWriteUnlock(&g_ext2Lock);
    return r;
}

static void Ext2FileClose(Vnode* vn)
{
    auto* fp = static_cast<Ext2FilePriv*>(vn->priv);
    Ext2ReleaseInode(fp->hold);
    kfree(fp);
}

static void Ext2FillStat(VnodeStat* st, uint32_t ino, const Ext2Inode& data)
{
    st->isDir = (data.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    st->isSymlink = (data.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK;
    st->size = st->isDir ? 0 : Ext2InodeSize(&data);
    st->ino = ino;
    st->uid = data.i_uid;
    st->gid = data.i_gid;
    st->mode = data.i_mode;
    st->nlink = data.i_links_count;
    st->atime = {data.i_atime, 0};
    st->mtime = {data.i_mtime, 0};
    st->ctime = {data.i_ctime, 0};
}

static int Ext2FileStat(Vnode* vn, VnodeStat* st)
{
    auto* fp = static_cast<Ext2FilePriv*>(vn->priv);
    Ext2Inode data;
    KRwLockReadLock(&g_ext2Lock);
    bool ok = Ext2ReadInode(fp->mnt, fp->inode, &data);
    KRwLockReadUnlock(&g_ext2Lock);
    if (!ok) return -5;
    Ext2FillStat(st, fp->inode, data);
    return 0;
}

static int Ext2DirReaddir(Vnode* vn, DirEntry* out, uint32_t* cookie)
{
    auto* dp = static_cast<Ext2DirPriv*>(vn->priv);
    uint64_t dirSize = Ext2InodeSize(&dp->inodeData);
    // The caller only commits the cookie once a Linux dirent fits its buffer.
    // Resume from that cookie so a buffer boundary cannot consume an entry.
    if (cookie) dp->readOffset = *cookie;

    KRwLockReadLock(&g_ext2Lock);

    while (dp->readOffset < dirSize) {
        // Read directory entry at current offset
        uint8_t entBuf[264]; // max dir entry: 8 + 255 name
        int r = Ext2ReadInodeData(dp->mnt, &dp->inodeData, entBuf,
                                  sizeof(entBuf), dp->readOffset);
        if (r < 8) { KRwLockReadUnlock(&g_ext2Lock); return 0; } // end or error

        auto* de = reinterpret_cast<Ext2DirEntry2*>(entBuf);
        if (de->rec_len < 8 || (de->rec_len & 3) != 0) {
            KRwLockReadUnlock(&g_ext2Lock); return 0;
        }
        if (de->inode != 0 && de->name_len > de->rec_len - 8) {
            KRwLockReadUnlock(&g_ext2Lock); return 0;
        }

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

        if (cookie) *cookie = dp->readOffset;
        KRwLockReadUnlock(&g_ext2Lock);
        return 1;
    }

    KRwLockReadUnlock(&g_ext2Lock);
    return 0;
}

static void Ext2DirClose(Vnode* vn)
{
    auto* dp = static_cast<Ext2DirPriv*>(vn->priv);
    Ext2ReleaseInode(dp->hold);
    kfree(dp);
}

static int Ext2DirStat(Vnode* vn, VnodeStat* st)
{
    auto* dp = static_cast<Ext2DirPriv*>(vn->priv);
    Ext2Inode data;
    KRwLockReadLock(&g_ext2Lock);
    bool ok = Ext2ReadInode(dp->mnt, dp->inode, &data);
    KRwLockReadUnlock(&g_ext2Lock);
    if (!ok) return -5;
    Ext2FillStat(st, dp->inode, data);
    return 0;
}

// Caller holds g_ext2Lock for writing. Ext2's base inode stores whole seconds;
// explicit subsecond values are rounded down to that on-disk precision.
static int Ext2SetInodeTimes(Ext2Mount* mnt, uint32_t ino, const VnodeTime* times)
{
    Ext2Inode data;
    if (!Ext2ReadInode(mnt, ino, &data)) return -5;
    if (times[0].nsec == VFS_UTIME_OMIT && times[1].nsec == VFS_UTIME_OMIT) return 0;
    Process* cur = ProcessCurrent();
    if (cur && cur->euid != 0 && cur->euid != data.i_uid && !mnt->skipPermChecks) {
        bool bothNow = times[0].nsec == VFS_UTIME_NOW && times[1].nsec == VFS_UTIME_NOW;
        uint16_t permissions = cur->egid == data.i_gid ? (data.i_mode >> 3) : data.i_mode;
        if (!bothNow || !(permissions & 2)) return -1; // EPERM
    }
    uint32_t now = Ext2Now();
    for (int i = 0; i < 2; ++i) {
        if (times[i].nsec != VFS_UTIME_NOW && times[i].nsec != VFS_UTIME_OMIT &&
            (times[i].sec < 0 || times[i].sec > EXT2_MAX_TIMESTAMP ||
             times[i].nsec < 0 || times[i].nsec >= 1000000000)) return -22;
    }
    if (times[0].nsec != VFS_UTIME_OMIT)
        data.i_atime = times[0].nsec == VFS_UTIME_NOW ? now : static_cast<uint32_t>(times[0].sec);
    if (times[1].nsec != VFS_UTIME_OMIT)
        data.i_mtime = times[1].nsec == VFS_UTIME_NOW ? now : static_cast<uint32_t>(times[1].sec);
    data.i_ctime = now;
    return Ext2WriteInode(mnt, ino, &data) ? 0 : -5;
}

static int Ext2VnodeSetTimes(Vnode* vn, const VnodeTime* times)
{
    Ext2Mount* mnt;
    uint32_t ino;
    if (vn->type == VnodeType::Dir) {
        auto* dp = static_cast<Ext2DirPriv*>(vn->priv);
        mnt = dp->mnt; ino = dp->inode;
    } else {
        auto* fp = static_cast<Ext2FilePriv*>(vn->priv);
        mnt = fp->mnt; ino = fp->inode;
    }
    KRwLockWriteLock(&g_ext2Lock);
    int result = Ext2SetInodeTimes(mnt, ino, times);
    KRwLockWriteUnlock(&g_ext2Lock);
    return result;
}

// Caller holds the filesystem write lock.
static int Ext2SetInodeMode(Ext2Mount* mnt, uint32_t ino, uint16_t mode)
{
    Ext2Inode data;
    if (!Ext2ReadInode(mnt, ino, &data)) return -5;
    Process* cur = ProcessCurrent();
    if (cur && cur->euid != 0) {
        if (cur->euid != data.i_uid) return -1;
        if (cur->egid != data.i_gid) mode &= ~02000;
    }
    data.i_mode = (data.i_mode & EXT2_S_IFMT) | (mode & 07777);
    data.i_ctime = Ext2Now();
    return Ext2WriteInode(mnt, ino, &data) ? 0 : -5;
}

static int Ext2VnodeChmod(Vnode* vn, uint16_t mode)
{
    Ext2Mount* mnt;
    uint32_t ino;
    if (vn->type == VnodeType::Dir) {
        auto* dp = static_cast<Ext2DirPriv*>(vn->priv);
        mnt = dp->mnt; ino = dp->inode;
    } else {
        auto* fp = static_cast<Ext2FilePriv*>(vn->priv);
        mnt = fp->mnt; ino = fp->inode;
    }
    KRwLockWriteLock(&g_ext2Lock);
    int result = Ext2SetInodeMode(mnt, ino, mode);
    KRwLockWriteUnlock(&g_ext2Lock);
    return result;
}

static const VnodeOps g_ext2FileOps = {
    .open    = Ext2FileOpen,
    .read    = Ext2FileRead,
    .write   = Ext2FileWrite,
    .readdir = nullptr,
    .close   = Ext2FileClose,
    .stat    = Ext2FileStat,
    .fsync   = nullptr,
    .set_times = Ext2VnodeSetTimes,
    .chmod   = Ext2VnodeChmod,
};

static const VnodeOps g_ext2DirOps = {
    .open    = Ext2FileOpen,
    .read    = nullptr,
    .write   = nullptr,
    .readdir = Ext2DirReaddir,
    .close   = Ext2DirClose,
    .stat    = Ext2DirStat,
    .fsync   = nullptr,
    .set_times = Ext2VnodeSetTimes,
    .chmod   = Ext2VnodeChmod,
};

// ---------------------------------------------------------------------------
// VfsFsOps callbacks
// ---------------------------------------------------------------------------

// Device binding table (similar to FatFS pdrv concept)
static constexpr uint8_t EXT2_MAX_MOUNTS = 16;
static Device* g_ext2Devices[EXT2_MAX_MOUNTS] = {};
static Ext2Mount* g_ext2Mounts[EXT2_MAX_MOUNTS] = {};
// The global mmap page cache outlives an individual mount. Never reuse its
// namespace on remount; physical drive numbers and raw inode numbers alone
// would let cached pages from another filesystem satisfy this file's faults.
static uint64_t g_nextExt2CacheNamespace = 1;

static uint64_t Ext2CacheId(const Ext2Mount* mnt, uint32_t ino)
{
    return (static_cast<uint64_t>(mnt->cacheNamespace) << 32) | ino;
}

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
    KRwLockReadLock(&g_ext2Lock);
    int r = dev->ops->read(dev, 1024, &sb, sizeof(sb));
    KRwLockReadUnlock(&g_ext2Lock);
    if (r != sizeof(sb)) {
        SerialPrintf("ext2: failed to read superblock from %s\n", dev->name);
        return false;
    }

    if (sb.s_magic != EXT2_SUPER_MAGIC) {
        SerialPrintf("ext2: bad magic 0x%x (expected 0xEF53) on %s\n",
                     sb.s_magic, dev->name);
        return false;
    }

    // Sanity-check critical superblock fields to avoid division-by-zero or
    // wildly oversized allocations from a corrupted image.
    if (sb.s_blocks_per_group == 0 || sb.s_inodes_per_group == 0) {
        SerialPrintf("ext2: corrupt superblock — blocks_per_group=%u inodes_per_group=%u\n",
                     sb.s_blocks_per_group, sb.s_inodes_per_group);
        return false;
    }
    if (sb.s_log_block_size > 6) { // max 64K blocks
        SerialPrintf("ext2: unreasonable s_log_block_size=%u\n", sb.s_log_block_size);
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

    KRwLockReadLock(&g_ext2Lock);
    r = dev->ops->read(dev, bgdtOff, bgdt, bgdtSize);
    KRwLockReadUnlock(&g_ext2Lock);
    if (r != static_cast<int>(bgdtSize)) {
        SerialPuts("ext2: failed to read BGDT\n");
        kfree(bgdt);
        return false;
    }

    auto* mnt = static_cast<Ext2Mount*>(kmalloc(sizeof(Ext2Mount)));
    if (!mnt) { kfree(bgdt); return false; }

    uint64_t cacheNamespace = __atomic_fetch_add(&g_nextExt2CacheNamespace, 1, __ATOMIC_RELAXED);
    if (cacheNamespace > 0xffffffffULL) {
        kfree(mnt); kfree(bgdt);
        SerialPuts("ext2: page-cache namespace exhausted\n");
        return false;
    }
    mnt->cacheNamespace = static_cast<uint32_t>(cacheNamespace);
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

    // Metadata cache arrays — lazy-populated on first use.
    mnt->bgdtDirty         = false;
    mnt->pendingMetaOps    = 0;
    mnt->indCacheLock      = SpinLock{};
    mnt->indCacheLru       = 0;
    for (uint32_t s = 0; s < Ext2Mount::IND_CACHE_SLOTS; ++s) {
        mnt->indCacheBlockNum[s] = 0;
        mnt->indCacheData[s]     = nullptr;
    }
    mnt->blockBitmapCache  = static_cast<uint8_t**>(kmalloc(groupCount * sizeof(uint8_t*)));
    mnt->blockBitmapDirty  = static_cast<bool*>(kmalloc(groupCount * sizeof(bool)));
    mnt->inodeBitmapCache  = static_cast<uint8_t**>(kmalloc(groupCount * sizeof(uint8_t*)));
    mnt->inodeBitmapDirty  = static_cast<bool*>(kmalloc(groupCount * sizeof(bool)));
    mnt->nextFreeBlockHint = static_cast<uint32_t*>(kmalloc(groupCount * sizeof(uint32_t)));
    mnt->nextFreeInodeHint = static_cast<uint32_t*>(kmalloc(groupCount * sizeof(uint32_t)));
    if (!mnt->blockBitmapCache || !mnt->blockBitmapDirty ||
        !mnt->inodeBitmapCache || !mnt->inodeBitmapDirty ||
        !mnt->nextFreeBlockHint || !mnt->nextFreeInodeHint) {
        if (mnt->blockBitmapCache) kfree(mnt->blockBitmapCache);
        if (mnt->blockBitmapDirty) kfree(mnt->blockBitmapDirty);
        if (mnt->inodeBitmapCache) kfree(mnt->inodeBitmapCache);
        if (mnt->inodeBitmapDirty) kfree(mnt->inodeBitmapDirty);
        if (mnt->nextFreeBlockHint) kfree(mnt->nextFreeBlockHint);
        if (mnt->nextFreeInodeHint) kfree(mnt->nextFreeInodeHint);
        kfree(bgdt); kfree(mnt);
        return false;
    }
    for (uint32_t i = 0; i < groupCount; ++i) {
        mnt->blockBitmapCache[i] = nullptr;
        mnt->blockBitmapDirty[i] = false;
        mnt->inodeBitmapCache[i] = nullptr;
        mnt->inodeBitmapDirty[i] = false;
        mnt->nextFreeBlockHint[i] = 0;
        mnt->nextFreeInodeHint[i] = 0;
    }

    // Inode read cache: 1024 direct-mapped slots. ~256 KB per mount.
    mnt->inodeCacheLock = SpinLock{};
    mnt->inodeCache = kmalloc(EXT2_INODE_CACHE_SIZE * sizeof(Ext2InodeCacheEntry));
    if (mnt->inodeCache) {
        memset(mnt->inodeCache, 0, EXT2_INODE_CACHE_SIZE * sizeof(Ext2InodeCacheEntry));
    }

    // Dirent name cache: 1024 direct-mapped slots. ~108 KB per mount.
    mnt->direntCacheLock = SpinLock{};
    mnt->direntCache = kmalloc(EXT2_DIRENT_CACHE_SIZE * sizeof(Ext2DirentCacheEntry));
    if (mnt->direntCache) {
        memset(mnt->direntCache, 0, EXT2_DIRENT_CACHE_SIZE * sizeof(Ext2DirentCacheEntry));
    }

    // Data block read cache: 8192 direct-mapped slots.
    // Pointer array ~64 KB; block buffers lazy-alloced on first use (~32 MB at full capacity).
    mnt->blockCacheLock = SpinLock{};
    mnt->blockCacheBlockNum = static_cast<uint32_t*>(
        kmalloc(Ext2Mount::BLOCK_CACHE_SLOTS * sizeof(uint32_t)));
    mnt->blockCacheData = static_cast<uint8_t**>(
        kmalloc(Ext2Mount::BLOCK_CACHE_SLOTS * sizeof(uint8_t*)));
    if (mnt->blockCacheBlockNum && mnt->blockCacheData) {
        memset(mnt->blockCacheBlockNum, 0,
               Ext2Mount::BLOCK_CACHE_SLOTS * sizeof(uint32_t));
        memset(mnt->blockCacheData, 0,
               Ext2Mount::BLOCK_CACHE_SLOTS * sizeof(uint8_t*));
    } else {
        // Non-fatal: cache just won't be used.
        if (mnt->blockCacheBlockNum) { kfree(mnt->blockCacheBlockNum); mnt->blockCacheBlockNum = nullptr; }
        if (mnt->blockCacheData) { kfree(mnt->blockCacheData); mnt->blockCacheData = nullptr; }
    }

    *mountPriv = mnt;
    g_ext2Mounts[pdrv] = mnt;
    mnt->skipPermChecks = false;  // Caller can override for nix mount
    SerialPrintf("ext2: pdrv %u page-cache namespace %u (writable files uncached)\n",
                 pdrv, mnt->cacheNamespace);
    DbgPrintf("ext2: mounted successfully\n");
    return true;
}

static bool Ext2FsCanUnmount(void* mountPriv)
{
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);
    KRwLockWriteLock(&g_ext2Lock);
    Ext2DrainOrphans(mnt);
    uint64_t flags = IrqSpinLockAcquire(&g_ext2OpenLock);
    bool busy = false;
    for (auto* p = g_ext2OpenInodes; p; p = p->next)
        if (p->mnt == mnt) { busy = true; break; }
    IrqSpinLockRelease(&g_ext2OpenLock, flags);
    KRwLockWriteUnlock(&g_ext2Lock);
    return !busy;
}

static void Ext2FsUnmount(void* mountPriv)
{
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);
    if (!mnt) return;
    // Flush any dirty metadata before releasing cache buffers.
    Ext2DrainOrphans(mnt);
    Ext2Sync(mnt);
    if (mnt->blockBitmapCache) {
        for (uint32_t g = 0; g < mnt->groupCount; ++g)
            if (mnt->blockBitmapCache[g]) kfree(mnt->blockBitmapCache[g]);
        kfree(mnt->blockBitmapCache);
    }
    if (mnt->inodeBitmapCache) {
        for (uint32_t g = 0; g < mnt->groupCount; ++g)
            if (mnt->inodeBitmapCache[g]) kfree(mnt->inodeBitmapCache[g]);
        kfree(mnt->inodeBitmapCache);
    }
    if (mnt->blockBitmapDirty) kfree(mnt->blockBitmapDirty);
    if (mnt->inodeBitmapDirty) kfree(mnt->inodeBitmapDirty);
    if (mnt->nextFreeBlockHint) kfree(mnt->nextFreeBlockHint);
    if (mnt->nextFreeInodeHint) kfree(mnt->nextFreeInodeHint);
    if (mnt->inodeCache) kfree(mnt->inodeCache);
    if (mnt->direntCache) kfree(mnt->direntCache);
    if (mnt->bgdt) kfree(mnt->bgdt);
    for (uint32_t i = 0; i < EXT2_MAX_MOUNTS; ++i)
        if (g_ext2Mounts[i] == mnt) g_ext2Mounts[i] = nullptr;
    kfree(mnt);
}

// VFS sync hook — flush all dirty metadata to disk.
static void Ext2FsSync(void* mountPriv)
{
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);
    if (!mnt) return;
    KRwLockWriteLock(&g_ext2Lock);
    Ext2DrainOrphans(mnt);
    Ext2Sync(mnt);
    KRwLockWriteUnlock(&g_ext2Lock);
}

static Vnode* Ext2FsOpenMode(void* mountPriv, uint8_t pdrv,
                            const char* relPath, int flags, uint16_t createMode)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);

    // Determine whether this open may mutate the filesystem.
    bool needsWrite = (flags & VFS_O_CREATE) || (flags & VFS_O_TRUNC);

    // Handle root directory
    bool isRoot = (!relPath[0] || (relPath[0] == '/' && !relPath[1]));
    uint32_t ino = 0;
    bool created = false;
    Ext2Inode inodeData;

    if (needsWrite) {
        // Take write lock up front — create and truncate mutate metadata.
        KRwLockWriteLock(&g_ext2Lock);
        Ext2DrainOrphans(mnt);
    } else {
        KRwLockReadLock(&g_ext2Lock);
    }

    if (isRoot) {
        ino = EXT2_ROOT_INO;
    } else {
        const char* p = relPath;
        while (*p == '/') ++p;
        ino = Ext2ResolvePath(mnt, EXT2_ROOT_INO, p, 0);
    }

    // File not found — create if requested
    if (!ino && (flags & VFS_O_CREATE)) {
        // Throttled: first 20 then every 100. NAR unpack creates thousands of
        // files; logging every one is pure noise.
        static uint32_t s_createCount = 0;
        s_createCount++;
        if (s_createCount <= 20 || (s_createCount % 100) == 0)
            SerialPrintf("ext2: CREATE file '%s' [#%u]\n", relPath, s_createCount);
        char name[256];
        uint32_t parentIno = Ext2ResolveParent(mnt, relPath, name, sizeof(name));
        if (!parentIno || !name[0]) { KRwLockWriteUnlock(&g_ext2Lock); return nullptr; }

        Ext2Inode parentData;
        if (!Ext2ReadInode(mnt, parentIno, &parentData)) { KRwLockWriteUnlock(&g_ext2Lock); return nullptr; }
        if ((parentData.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) { KRwLockWriteUnlock(&g_ext2Lock); return nullptr; }

        // Allocate new inode
        ino = Ext2AllocInode(mnt, false);
        if (!ino) { KRwLockWriteUnlock(&g_ext2Lock); return nullptr; }

        // Initialize inode
        memset(&inodeData, 0, sizeof(inodeData));
        {
            Process* cur = ProcessCurrent();
            uint16_t mask = cur ? cur->umask : 022;
            inodeData.i_mode = EXT2_S_IFREG | (createMode & 07777 & ~mask);
            inodeData.i_uid = cur ? static_cast<uint16_t>(cur->euid) : 0;
            inodeData.i_gid = cur ? static_cast<uint16_t>(cur->egid) : 0;
        }
        inodeData.i_links_count = 1;
        Ext2Touch(&inodeData, true);
        Ext2WriteInode(mnt, ino, &inodeData);

        // Add directory entry
        bool entryCommitted;
        if (!Ext2DirAdd(mnt, parentIno, &parentData, ino, name, 1 /*EXT2_FT_REG_FILE*/, &entryCommitted)) {
            if (!entryCommitted) Ext2FreeInode(mnt, ino, false);
            KRwLockWriteUnlock(&g_ext2Lock);
            return nullptr;
        }
        created = true;
    }

    if (!ino) {
        if (needsWrite)
            KRwLockWriteUnlock(&g_ext2Lock);
        else
            KRwLockReadUnlock(&g_ext2Lock);
        if (g_kdebugTrace)
            SerialPrintf("ext2: open '%s' → not found\n", relPath);
        return nullptr;
    }

    if (!Ext2ReadInode(mnt, ino, &inodeData)) {
        if (needsWrite) KRwLockWriteUnlock(&g_ext2Lock);
        else            KRwLockReadUnlock(&g_ext2Lock);
        return nullptr;
    }

    // Permission check (skipped for root or mounts with skipPermChecks set).
    // Only applies to existing files — newly created files skip this.
    if (!created) {
        Process* cur = ProcessCurrent();
        if (cur && cur->euid != 0 && !mnt->skipPermChecks) {
            uint16_t fmode = inodeData.i_mode & 07777;
            uint16_t perm;
            if (cur->euid == inodeData.i_uid)
                perm = (fmode >> 6) & 7;
            else if (cur->egid == inodeData.i_gid)
                perm = (fmode >> 3) & 7;
            else
                perm = fmode & 7;

            bool denied = false;
            if (flags & VFS_O_WRITE)
                denied = !(perm & 2);
            else
                denied = !(perm & 4);  // read access

            if (denied) {
                if (needsWrite) KRwLockWriteUnlock(&g_ext2Lock);
                else            KRwLockReadUnlock(&g_ext2Lock);
                return nullptr;  // EACCES (caller sees null = open failed)
            }
        }
    }

    // Truncate if requested
    if ((flags & VFS_O_TRUNC) && (inodeData.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG) {
        if (!Ext2FreeInodeBlocks(mnt, ino, &inodeData)) {
            Ext2WriteInode(mnt, ino, &inodeData);
            KRwLockWriteUnlock(&g_ext2Lock);
            return nullptr;
        }
        Ext2Touch(&inodeData);
        Ext2WriteInode(mnt, ino, &inodeData);
    }

    uint16_t mode = inodeData.i_mode & EXT2_S_IFMT;
    Ext2OpenInode* hold = nullptr;
    if (mode == EXT2_S_IFDIR || mode == EXT2_S_IFREG)
        hold = Ext2HoldInode(mnt, ino);
    if (needsWrite) KRwLockWriteUnlock(&g_ext2Lock);
    else KRwLockReadUnlock(&g_ext2Lock);
    if (!hold) return nullptr;

    if (mode == EXT2_S_IFDIR) {
        auto* dp = static_cast<Ext2DirPriv*>(kmalloc(sizeof(Ext2DirPriv)));
        if (!dp) { Ext2ReleaseInode(hold); return nullptr; }
        dp->mnt = mnt;
        dp->inode = ino;
        dp->inodeData = inodeData;
        dp->readOffset = 0;
        dp->hold = hold;

        auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!vn) { kfree(dp); Ext2ReleaseInode(hold); return nullptr; }
        vn->ops = &g_ext2DirOps;
        vn->type = VnodeType::Dir;
        vn->priv = dp;
        vn->refCount = 1;
        vn->cacheId = Ext2CacheId(mnt, ino);
        return vn;
    }

    if (mode == EXT2_S_IFREG) {
        auto* fp = static_cast<Ext2FilePriv*>(kmalloc(sizeof(Ext2FilePriv)));
        if (!fp) { Ext2ReleaseInode(hold); return nullptr; }
        fp->mnt = mnt;
        fp->inode = ino;
        fp->inodeData = inodeData;
        fp->hold = hold;

        auto* vn = static_cast<Vnode*>(kmalloc(sizeof(Vnode)));
        if (!vn) { kfree(fp); Ext2ReleaseInode(hold); return nullptr; }
        vn->ops = &g_ext2FileOps;
        vn->type = VnodeType::File;
        vn->priv = fp;
        vn->refCount = 1;
        // Every ext2 mount is currently writable, including /nix. The global
        // mmap cache has no write/truncate/inode-reuse invalidation protocol,
        // so caching these pages can return old source or library bytes.
        // Keep demand paging and the coherent ext2 block cache, but bypass
        // shared mmap-page caching until mutation synchronization is available.
        // Ext2CacheId remains the namespace-safe identity for future caching;
        // stat uses the real inode independently of this opt-out value.
        vn->cacheId = 0;
        return vn;
    }

    // Symlinks should have been resolved during path resolution
    return nullptr;
}

static Vnode* Ext2FsOpen(void* mountPriv, uint8_t pdrv, const char* relPath, int flags)
{
    return Ext2FsOpenMode(mountPriv, pdrv, relPath, flags, 0666);
}

static int Ext2FsStatPath(void* mountPriv, uint8_t pdrv,
                          const char* relPath, VnodeStat* st)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);

    bool isRoot = (!relPath[0] || (relPath[0] == '/' && !relPath[1]));

    KRwLockReadLock(&g_ext2Lock);

    uint32_t ino;
    if (isRoot) {
        ino = EXT2_ROOT_INO;
    } else {
        const char* p = relPath;
        while (*p == '/') ++p;
        ino = Ext2ResolvePath(mnt, EXT2_ROOT_INO, p, 0);
    }

    if (!ino) {
        DbgPrintf("ext2: stat '%s' → not found\n", relPath);
        KRwLockReadUnlock(&g_ext2Lock);
        return -1;
    }

    Ext2Inode inodeData;
    if (!Ext2ReadInode(mnt, ino, &inodeData)) {
        SerialPrintf("ext2: stat '%s' ino=%u → read failed\n", relPath, ino);
        KRwLockReadUnlock(&g_ext2Lock);
        return -1;
    }

    KRwLockReadUnlock(&g_ext2Lock);

    Ext2FillStat(st, ino, inodeData);
    return 0;
}

// lstat: like stat but does NOT follow the final symlink
static int Ext2FsLstatPath(void* mountPriv, uint8_t pdrv,
                            const char* relPath, VnodeStat* st)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);

    bool isRoot = (!relPath[0] || (relPath[0] == '/' && !relPath[1]));

    KRwLockReadLock(&g_ext2Lock);

    if (isRoot) {
        Ext2Inode inodeData;
        if (!Ext2ReadInode(mnt, EXT2_ROOT_INO, &inodeData)) {
            KRwLockReadUnlock(&g_ext2Lock);
            return -1;
        }
        KRwLockReadUnlock(&g_ext2Lock);
        Ext2FillStat(st, EXT2_ROOT_INO, inodeData);
        return 0;
    }

    // Resolve parent directory (following symlinks in the path), then
    // look up the final component directly without following it.
    char name[256];
    uint32_t parentIno = Ext2ResolveParent(mnt, relPath, name, sizeof(name));
    if (!parentIno || !name[0]) { KRwLockReadUnlock(&g_ext2Lock); return -1; }

    Ext2Inode parentData;
    if (!Ext2ReadInode(mnt, parentIno, &parentData)) { KRwLockReadUnlock(&g_ext2Lock); return -1; }

    uint32_t ino = Ext2DirLookup(mnt, parentIno, &parentData, name);
    if (!ino) { KRwLockReadUnlock(&g_ext2Lock); return -1; }

    Ext2Inode inodeData;
    if (!Ext2ReadInode(mnt, ino, &inodeData)) { KRwLockReadUnlock(&g_ext2Lock); return -1; }

    KRwLockReadUnlock(&g_ext2Lock);

    Ext2FillStat(st, ino, inodeData);
    return 0;
}

// ---------------------------------------------------------------------------
// VfsFsOps write operations
// ---------------------------------------------------------------------------

// 1 = empty, 0 = contains children, negative = read/corruption error.
static int Ext2DirectoryEmpty(Ext2Mount* mnt, const Ext2Inode* data)
{
    auto* buf = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!buf) return -12;
    for (uint64_t off = 0; off < Ext2InodeSize(data); off += mnt->blockSize) {
        uint32_t block = Ext2BlockMap(mnt, data, off >> mnt->blockShift);
        if (!block || !Ext2ReadBlock(mnt, block, buf)) { kfree(buf); return -5; }
        for (uint32_t pos = 0; pos < mnt->blockSize;) {
            auto* entry = reinterpret_cast<Ext2DirEntry2*>(buf + pos);
            if (!Ext2DirEntryValid(entry, pos, mnt->blockSize)) { kfree(buf); return -5; }
            bool dot = entry->name_len == 1 && entry->name[0] == '.';
            bool dotdot = entry->name_len == 2 && entry->name[0] == '.' && entry->name[1] == '.';
            if (entry->inode && !dot && !dotdot) { kfree(buf); return 0; }
            pos += entry->rec_len;
        }
    }
    kfree(buf);
    return 1;
}

static int Ext2FsRemovePath(void* mountPriv, uint8_t pdrv, const char* relPath, bool removeDirectory)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);
    KRwLockWriteLock(&g_ext2Lock);
    Ext2DrainOrphans(mnt);
    Ext2OpenInode* deletionHold = nullptr;
    auto finish = [&](int result) {
        Ext2ReleaseInode(deletionHold);
        KRwLockWriteUnlock(&g_ext2Lock);
        return result;
    };
    char name[256];
    uint32_t parentIno = Ext2ResolveParent(mnt, relPath, name, sizeof(name));
    if (!parentIno || !name[0]) return finish(-2);
    if (!strcmp(name, ".") || !strcmp(name, "..")) return finish(-22);
    Ext2Inode parent, target;
    if (!Ext2ReadInode(mnt, parentIno, &parent)) return finish(-5);
    uint32_t ino = Ext2DirLookup(mnt, parentIno, &parent, name);
    if (!ino) return finish(-2);
    if (ino == EXT2_ROOT_INO) return finish(-16);
    if (!Ext2ReadInode(mnt, ino, &target)) return finish(-5);
    bool directory = (target.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    if (directory != removeDirectory) return finish(removeDirectory ? -20 : -21);
    if (directory) {
        int empty = Ext2DirectoryEmpty(mnt, &target);
        if (empty != 1) return finish(empty < 0 ? empty : -39);
        if (!parent.i_links_count) return finish(-5);
    }
    deletionHold = Ext2HoldInode(mnt, ino);
    if (!deletionHold) return finish(-12);
    bool entryRemoved;
    int error = Ext2DirRemove(mnt, &parent, parentIno, name, &entryRemoved) == ino ? 0 : -5;
    if (!entryRemoved) return finish(-5);
    if (directory) {
        --parent.i_links_count;
        if (!Ext2WriteInode(mnt, parentIno, &parent)) error = -5;
    }
    int dropped = Ext2DropLink(mnt, ino, &target, directory);
    return finish(error ? error : dropped);
}

static int Ext2FsUnlink(void* mountPriv, uint8_t pdrv, const char* relPath)
{
    return Ext2FsRemovePath(mountPriv, pdrv, relPath, false);
}

static int Ext2FsMkdir(void* mountPriv, uint8_t pdrv, const char* relPath)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);

    DbgPrintf("ext2: MKDIR '%s'\n", relPath);

    KRwLockWriteLock(&g_ext2Lock);
    Ext2DrainOrphans(mnt);

    char name[256];
    uint32_t parentIno = Ext2ResolveParent(mnt, relPath, name, sizeof(name));
    if (!parentIno || !name[0]) { KRwLockWriteUnlock(&g_ext2Lock); return -1; }

    Ext2Inode parentData;
    if (!Ext2ReadInode(mnt, parentIno, &parentData)) { KRwLockWriteUnlock(&g_ext2Lock); return -1; }

    // Check if already exists
    if (Ext2DirLookup(mnt, parentIno, &parentData, name)) {
        KRwLockWriteUnlock(&g_ext2Lock);
        return 0; // Already exists, not an error (like FAT driver)
    }

    // Allocate inode
    uint32_t newIno = Ext2AllocInode(mnt, true);
    if (!newIno) { KRwLockWriteUnlock(&g_ext2Lock); return -1; }

    // Initialize directory inode
    Ext2Inode newData;
    memset(&newData, 0, sizeof(newData));
    {
        Process* cur = ProcessCurrent();
        uint16_t mask = cur ? cur->umask : 022;
        newData.i_mode = EXT2_S_IFDIR | (0777 & ~mask);
        newData.i_uid = cur ? static_cast<uint16_t>(cur->euid) : 0;
        newData.i_gid = cur ? static_cast<uint16_t>(cur->egid) : 0;
    }
    newData.i_links_count = 2; // . and parent's entry
    Ext2Touch(&newData, true);

    // Allocate first data block for . and .. entries
    uint32_t dataBlock = Ext2AllocBlock(mnt);
    if (!dataBlock) {
        Ext2FreeInode(mnt, newIno, true);
        KRwLockWriteUnlock(&g_ext2Lock);
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
        KRwLockWriteUnlock(&g_ext2Lock);
        return -1;
    }
    memset(blockBuf, 0, mnt->blockSize);

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
    bool entryCommitted;
    if (!Ext2DirAdd(mnt, parentIno, &parentData, newIno, name, 2 /*EXT2_FT_DIR*/, &entryCommitted)) {
        if (!entryCommitted) {
            Ext2FreeBlock(mnt, dataBlock);
            Ext2FreeInode(mnt, newIno, true);
        }
        KRwLockWriteUnlock(&g_ext2Lock);
        return -1;
    }

    // Increment parent link count (for ..)
    parentData.i_links_count++;
    Ext2WriteInode(mnt, parentIno, &parentData);

    KRwLockWriteUnlock(&g_ext2Lock);
    return 0;
}

static bool Ext2DirReplace(Ext2Mount* mnt, uint32_t dirIno, Ext2Inode* directory,
                            const char* name, uint32_t inode, uint8_t type, bool* committed = nullptr)
{
    if (committed) *committed = false;
    if (!Ext2PrepareDirectoryWrite(mnt, dirIno, directory)) return false;
    auto* buf = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
    if (!buf) return false;
    size_t length = strlen(name);
    for (uint64_t off = 0; off < Ext2InodeSize(directory); off += mnt->blockSize) {
        uint32_t block = Ext2BlockMap(mnt, directory, off >> mnt->blockShift);
        if (!block || !Ext2ReadBlock(mnt, block, buf)) break;
        for (uint32_t pos = 0; pos < mnt->blockSize;) {
            auto* entry = reinterpret_cast<Ext2DirEntry2*>(buf + pos);
            if (!Ext2DirEntryValid(entry, pos, mnt->blockSize)) break;
            if (entry->inode && entry->name_len == length && !memcmp(entry->name, name, length)) {
                entry->inode = inode;
                entry->file_type = type;
                bool ok = Ext2WriteBlock(mnt, block, buf);
                kfree(buf);
                if (ok && committed) *committed = true;
                Ext2DirentCacheInvalidateParent(mnt, dirIno);
                Ext2Touch(directory);
                return ok && Ext2WriteInode(mnt, dirIno, directory);
            }
            pos += entry->rec_len;
        }
    }
    kfree(buf);
    return false;
}

static int Ext2FsRename(void* mountPriv, uint8_t pdrv,
                        const char* oldRelPath, const char* newRelPath)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);
    KRwLockWriteLock(&g_ext2Lock);
    Ext2DrainOrphans(mnt);
    Ext2OpenInode* deletionHold = nullptr;
    auto finish = [&](int result) {
        Ext2ReleaseInode(deletionHold);
        KRwLockWriteUnlock(&g_ext2Lock);
        return result;
    };
    char oldName[256], newName[256];
    uint32_t oldParent = Ext2ResolveParent(mnt, oldRelPath, oldName, sizeof(oldName));
    uint32_t newParent = Ext2ResolveParent(mnt, newRelPath, newName, sizeof(newName));
    if (!oldParent || !newParent) return finish(-2);
    if (!oldName[0] || !newName[0]) return finish(-16);
    if (!strcmp(oldName, ".") || !strcmp(oldName, "..") ||
        !strcmp(newName, ".") || !strcmp(newName, "..")) return finish(-22);
    Ext2Inode oldDir, newDir, target, victim;
    if (!Ext2ReadInode(mnt, oldParent, &oldDir) || !Ext2ReadInode(mnt, newParent, &newDir))
        return finish(-5);
    uint32_t ino = Ext2DirLookup(mnt, oldParent, &oldDir, oldName);
    if (!ino) return finish(-2);
    uint32_t existing = Ext2DirLookup(mnt, newParent, &newDir, newName);
    if (existing == ino) return finish(0);
    if (ino == EXT2_ROOT_INO) return finish(-16);
    if (!Ext2ReadInode(mnt, ino, &target)) return finish(-5);
    bool directory = (target.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    uint8_t type = directory ? 2 : (target.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK ? 7 : 1;
    bool victimDirectory = false;
    uint8_t victimType = 1;
    if (existing) {
        if (!Ext2ReadInode(mnt, existing, &victim)) return finish(-5);
        victimDirectory = (victim.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
        if (directory != victimDirectory) return finish(directory ? -20 : -21);
        victimType = victimDirectory ? 2 : (victim.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK ? 7 : 1;
        if (victimDirectory) {
            int empty = Ext2DirectoryEmpty(mnt, &victim);
            if (empty != 1) return finish(empty < 0 ? empty : -39);
        }
    }
    if (directory && oldParent != newParent) {
        // Walk '..' before modifying anything; moving into a descendant
        // would detach a cyclic directory tree from the filesystem root.
        uint32_t ancestor = newParent;
        unsigned depth = 0;
        while (ancestor != EXT2_ROOT_INO && depth++ < 1024) {
            if (ancestor == ino) return finish(-22);
            Ext2Inode data;
            if (!Ext2ReadInode(mnt, ancestor, &data)) return finish(-5);
            ancestor = Ext2DirLookup(mnt, ancestor, &data, "..");
            if (!ancestor) return finish(-5);
        }
        if (ancestor != EXT2_ROOT_INO) return finish(-40);
    }

    // Install destination first. Replacement uses the existing entry's
    // storage, and keeps the victim inode alive until namespace changes work.
    if (existing) {
        deletionHold = Ext2HoldInode(mnt, existing);
        if (!deletionHold) return finish(-12);
    }
    bool entryInstalled;
    bool installed = existing ? Ext2DirReplace(mnt, newParent, &newDir, newName, ino, type, &entryInstalled)
                              : Ext2DirAdd(mnt, newParent, &newDir, ino, newName, type, &entryInstalled);
    auto restoreDestination = [&]() {
        if (existing) Ext2DirReplace(mnt, newParent, &newDir, newName, existing, victimType);
        else Ext2DirRemove(mnt, &newDir, newParent, newName);
    };
    if (!installed) {
        if (entryInstalled) restoreDestination();
        return finish(-5);
    }
    // Same-directory operations must use metadata after the destination edit.
    bool entryRemoved = false;
    if (!Ext2ReadInode(mnt, oldParent, &oldDir) ||
        Ext2DirRemove(mnt, &oldDir, oldParent, oldName, &entryRemoved) != ino) {
        restoreDestination();
        if (entryRemoved) {
            Ext2ReadInode(mnt, oldParent, &oldDir);
            Ext2DirAdd(mnt, oldParent, &oldDir, ino, oldName, type);
        }
        return finish(-5);
    }
    // Namespace committed. Even if a later metadata write fails, complete
    // victim accounting and retain the first error; never skip its unlink.
    int error = 0;
    if (directory && oldParent != newParent &&
        !Ext2DirReplace(mnt, ino, &target, "..", newParent, 2)) error = -5;
    if (!Ext2ReadInode(mnt, oldParent, &oldDir) || !Ext2ReadInode(mnt, newParent, &newDir)) {
        error = -5;
    } else {
        if (directory && oldParent != newParent) {
            if (!oldDir.i_links_count) error = -5;
            else {
                --oldDir.i_links_count;
                ++newDir.i_links_count;
                if (!Ext2WriteInode(mnt, oldParent, &oldDir)) error = -5;
            }
        }
        if (victimDirectory) {
            if (!newDir.i_links_count) error = -5;
            else --newDir.i_links_count;
        }
        if (!Ext2WriteInode(mnt, newParent, &newDir)) error = -5;
    }
    target.i_ctime = Ext2Now();
    if (!Ext2WriteInode(mnt, ino, &target)) error = -5;
    int dropped = existing ? Ext2DropLink(mnt, existing, &victim, victimDirectory) : 0;
    return finish(error ? error : dropped);
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
    if (targetLen == 0 || targetLen > 4096) return -22; // -EINVAL

    KRwLockWriteLock(&g_ext2Lock);

    char name[256];
    uint32_t parentIno = Ext2ResolveParent(mnt, relPath, name, sizeof(name));
    if (!parentIno || !name[0]) {
        SerialPrintf("ext2 symlink: ResolveParent FAILED for '%s' (target='%s')\n",
                     relPath, target);
        KRwLockWriteUnlock(&g_ext2Lock); return -2; // -ENOENT
    }

    Ext2Inode parentData;
    if (!Ext2ReadInode(mnt, parentIno, &parentData)) {
        SerialPrintf("ext2 symlink: ReadInode parent %u FAILED for '%s'\n",
                     parentIno, relPath);
        KRwLockWriteUnlock(&g_ext2Lock); return -5; // -EIO
    }

    // Check if already exists
    if (Ext2DirLookup(mnt, parentIno, &parentData, name)) {
        KRwLockWriteUnlock(&g_ext2Lock);
        return -17; // -EEXIST
    }

    uint32_t newIno = Ext2AllocInode(mnt, false);
    if (!newIno) {
        SerialPrintf("ext2 symlink: AllocInode FAILED (likely out of inodes) for '%s'\n",
                     relPath);
        KRwLockWriteUnlock(&g_ext2Lock); return -28; // -ENOSPC
    }

    Ext2Inode newData;
    memset(&newData, 0, sizeof(newData));
    newData.i_mode = EXT2_S_IFLNK | 0777;
    newData.i_links_count = 1;
    Ext2Touch(&newData, true);

    // Fast symlink: store target directly in i_block[] if it fits (≤60 bytes)
    if (targetLen <= 60) {
        auto* dst = reinterpret_cast<char*>(newData.i_block);
        memcpy(dst, target, targetLen);
        newData.i_size = targetLen;
        newData.i_blocks = 0;
    } else {
        // Slow symlink: allocate a data block
        uint32_t blk = Ext2AllocBlock(mnt);
        if (!blk) {
            SerialPrintf("ext2 symlink: AllocBlock FAILED (disk full?) for '%s' target len=%u\n",
                         relPath, targetLen);
            Ext2FreeInode(mnt, newIno, false);
            KRwLockWriteUnlock(&g_ext2Lock);
            return -28; // -ENOSPC
        }
        auto* buf = static_cast<uint8_t*>(kmalloc(mnt->blockSize));
        if (!buf) {
            SerialPrintf("ext2 symlink: kmalloc(%u) FAILED for '%s'\n",
                         mnt->blockSize, relPath);
            Ext2FreeBlock(mnt, blk);
            Ext2FreeInode(mnt, newIno, false);
            KRwLockWriteUnlock(&g_ext2Lock);
            return -12; // -ENOMEM
        }
        memset(buf, 0, mnt->blockSize);
        for (uint32_t i = 0; i < targetLen; ++i) buf[i] = static_cast<uint8_t>(target[i]);
        Ext2WriteBlock(mnt, blk, buf);
        kfree(buf);

        newData.i_block[0] = blk;
        newData.i_size = targetLen;
        newData.i_blocks = mnt->blockSize / 512;
    }

    Ext2WriteInode(mnt, newIno, &newData);

    // Add directory entry with type=7 (EXT2_FT_SYMLINK)
    bool entryCommitted;
    if (!Ext2DirAdd(mnt, parentIno, &parentData, newIno, name, 7, &entryCommitted)) {
        SerialPrintf("ext2 symlink: DirAdd FAILED parent=%u name='%s' for '%s'\n",
                     parentIno, name, relPath);
        if (!entryCommitted && Ext2FreeInodeBlocks(mnt, newIno, &newData))
            Ext2FreeInode(mnt, newIno, false);
        KRwLockWriteUnlock(&g_ext2Lock);
        return -28; // -ENOSPC (probably couldn't extend dir)
    }

    KRwLockWriteUnlock(&g_ext2Lock);
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

    KRwLockReadLock(&g_ext2Lock);

    // Resolve path WITHOUT following the final symlink component.
    // We split the path into parent + name and resolve the parent,
    // then look up the name directly in the parent directory.
    char name[256];
    uint32_t parentIno = Ext2ResolveParent(mnt, relPath, name, sizeof(name));
    if (!parentIno || !name[0]) { KRwLockReadUnlock(&g_ext2Lock); return -1; }

    Ext2Inode parentData;
    if (!Ext2ReadInode(mnt, parentIno, &parentData)) { KRwLockReadUnlock(&g_ext2Lock); return -1; }

    uint32_t ino = Ext2DirLookup(mnt, parentIno, &parentData, name);
    if (!ino) { KRwLockReadUnlock(&g_ext2Lock); return -22; } // -EINVAL

    Ext2Inode inodeData;
    if (!Ext2ReadInode(mnt, ino, &inodeData)) { KRwLockReadUnlock(&g_ext2Lock); return -1; }

    if ((inodeData.i_mode & EXT2_S_IFMT) != EXT2_S_IFLNK) {
        KRwLockReadUnlock(&g_ext2Lock);
        return -22; // -EINVAL: not a symlink
    }

    char* target = Ext2ReadSymlink(mnt, &inodeData);
    if (!target) { KRwLockReadUnlock(&g_ext2Lock); return -1; }

    uint32_t targetLen = 0;
    for (const char* p = target; *p; ++p) ++targetLen;

    uint32_t copyLen = targetLen;
    if (copyLen > bufsiz) copyLen = static_cast<uint32_t>(bufsiz);
    for (uint32_t i = 0; i < copyLen; ++i) buf[i] = target[i];

    kfree(target);
    KRwLockReadUnlock(&g_ext2Lock);
    return static_cast<int>(copyLen);
}

// ---------------------------------------------------------------------------
// Ext2Chmod / Ext2Chown — modify inode metadata
// ---------------------------------------------------------------------------

static int Ext2FsChmod(void* mountPriv, uint8_t pdrv, const char* relPath,
                       uint16_t mode)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);
    KRwLockWriteLock(&g_ext2Lock);
    const char* path = relPath;
    while (*path == '/') ++path;
    uint32_t ino = *path ? Ext2ResolvePath(mnt, EXT2_ROOT_INO, path, 0) : EXT2_ROOT_INO;
    int result = ino ? Ext2SetInodeMode(mnt, ino, mode) : -2;
    KRwLockWriteUnlock(&g_ext2Lock);
    return result;
}

int Ext2Chmod(const char* path, uint16_t mode)
{
    return VfsChmodPath(path, mode);
}

int Ext2Chown(const char* path, uint32_t uid, uint32_t gid)
{
    Ext2Mount* mnt = nullptr;
    for (int i = 0; i < EXT2_MAX_MOUNTS; ++i) {
        if (!g_ext2Mounts[i]) continue;
        mnt = g_ext2Mounts[i];
        break;
    }
    if (!mnt) return -2;

    KRwLockWriteLock(&g_ext2Lock);

    const char* p = path;
    while (*p == '/') ++p;
    uint32_t ino = *p ? Ext2ResolvePath(mnt, EXT2_ROOT_INO, p, 0) : EXT2_ROOT_INO;
    if (!ino) { KRwLockWriteUnlock(&g_ext2Lock); return -2; }

    Ext2Inode inodeData;
    if (!Ext2ReadInode(mnt, ino, &inodeData)) { KRwLockWriteUnlock(&g_ext2Lock); return -5; }

    if (uid != 0xFFFFFFFF) inodeData.i_uid = static_cast<uint16_t>(uid);
    if (gid != 0xFFFFFFFF) inodeData.i_gid = static_cast<uint16_t>(gid);
    inodeData.i_ctime = Ext2Now();
    Ext2WriteInode(mnt, ino, &inodeData);

    KRwLockWriteUnlock(&g_ext2Lock);
    return 0;
}

static int Ext2FsSetTimes(void* mountPriv, uint8_t pdrv, const char* relPath,
                          const VnodeTime* times, bool nofollow)
{
    (void)pdrv;
    auto* mnt = static_cast<Ext2Mount*>(mountPriv);
    KRwLockWriteLock(&g_ext2Lock);
    const char* path = relPath;
    while (*path == '/') ++path;
    uint32_t ino = EXT2_ROOT_INO;
    if (*path && nofollow) {
        char name[256];
        uint32_t parent = Ext2ResolveParent(mnt, path, name, sizeof(name));
        Ext2Inode parentData;
        ino = parent && Ext2ReadInode(mnt, parent, &parentData)
                ? Ext2DirLookup(mnt, parent, &parentData, name) : 0;
    } else if (*path) {
        ino = Ext2ResolvePath(mnt, EXT2_ROOT_INO, path, 0);
    }
    int result = ino ? Ext2SetInodeTimes(mnt, ino, times) : -2;
    KRwLockWriteUnlock(&g_ext2Lock);
    return result;
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
    .sync       = Ext2FsSync,
    .set_times  = Ext2FsSetTimes,
    .open_mode  = Ext2FsOpenMode,
    .chmod      = Ext2FsChmod,
    .remove_path = Ext2FsRemovePath,
    .can_unmount = Ext2FsCanUnmount,
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Ext2VfsRegister()
{
    EnsureLock();
    VfsRegisterFs("ext2", &g_ext2FsOps);
}

void Ext2SetSkipPermChecks(uint8_t pdrv, bool skip)
{
    if (pdrv >= EXT2_MAX_MOUNTS || !g_ext2Mounts[pdrv]) return;
    g_ext2Mounts[pdrv]->skipPermChecks = skip;
}

} // namespace brook

// Diagnostic: dump ext2 rwlock state (called from scheduler thread dump)
extern "C" void Ext2DumpLockState()
{
    brook::SerialPrintf("  ext2Lock: readers=%d writerActive=%u writersWaiting=%u "
                        "readWaitHead=%p writeWaitHead=%p\n",
                        brook::g_ext2Lock.readerCount,
                        brook::g_ext2Lock.writerActive,
                        brook::g_ext2Lock.writersWaiting,
                        brook::g_ext2Lock.readWaitHead,
                        brook::g_ext2Lock.writeWaitHead);
}

// Bind a block device to an ext2 pdrv slot.
extern "C" bool Ext2BindDevice(uint8_t pdrv, brook::Device* dev)
{
    if (pdrv >= brook::EXT2_MAX_MOUNTS || !dev) return false;
    brook::g_ext2Devices[pdrv] = dev;
    brook::SerialPrintf("ext2: drive %u → '%s'\n", pdrv, dev->name);
    return true;
}
