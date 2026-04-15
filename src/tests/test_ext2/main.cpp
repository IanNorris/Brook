// test_ext2/main.cpp — Host-native unit tests for ext2 on-disk structures and logic.
//
// These tests verify the ext2 superblock, inode, and directory parsing against
// a real ext2 image created by mkfs.ext2, without needing the Brook kernel.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cassert>

// ---- On-disk structures (mirrors ext2_vfs.cpp) ----

struct Ext2Superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
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
};

static constexpr uint16_t EXT2_SUPER_MAGIC = 0xEF53;

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

struct Ext2Inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
};

struct Ext2DirEntry2 {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
};

static constexpr uint16_t EXT2_S_IFMT  = 0xF000;
static constexpr uint16_t EXT2_S_IFREG = 0x8000;
static constexpr uint16_t EXT2_S_IFDIR = 0x4000;
static constexpr uint16_t EXT2_S_IFLNK = 0xA000;
static constexpr uint32_t EXT2_ROOT_INO = 2;

// ---- Test disk access via FILE* ----

static FILE*    g_disk = nullptr;
static uint32_t g_blockSize = 0;
static uint32_t g_blockShift = 0;
static uint32_t g_inodeSize = 0;
static uint32_t g_inodesPerGroup = 0;
static uint32_t g_groupCount = 0;
static Ext2BlockGroupDesc* g_bgdt = nullptr;

static bool DiskRead(uint64_t off, void* buf, uint64_t len) {
    if (fseek(g_disk, off, SEEK_SET) != 0) return false;
    return fread(buf, 1, len, g_disk) == len;
}

static bool ReadInode(uint32_t ino, Ext2Inode* out) {
    uint32_t group = (ino - 1) / g_inodesPerGroup;
    uint32_t index = (ino - 1) % g_inodesPerGroup;
    uint64_t tableOff = (uint64_t)g_bgdt[group].bg_inode_table << g_blockShift;
    uint64_t inodeOff = tableOff + (uint64_t)index * g_inodeSize;
    return DiskRead(inodeOff, out, sizeof(Ext2Inode));
}

static uint32_t BlockMap(const Ext2Inode* ino, uint32_t fileBlock) {
    if (fileBlock < 12) return ino->i_block[fileBlock];
    return 0;
}

static uint32_t DirLookup(const Ext2Inode* dirIno, const char* name) {
    uint32_t nameLen = strlen(name);
    uint8_t buf[4096];
    uint64_t dirSize = dirIno->i_size;
    uint64_t off = 0;
    while (off < dirSize) {
        uint32_t fb = (uint32_t)(off >> g_blockShift);
        uint32_t db = BlockMap(dirIno, fb);
        if (!db) break;
        if (!DiskRead((uint64_t)db << g_blockShift, buf, g_blockSize)) break;
        uint32_t pos = 0;
        while (pos < g_blockSize && off + pos < dirSize) {
            auto* de = (Ext2DirEntry2*)(buf + pos);
            if (de->rec_len == 0) break;
            if (de->inode != 0 && de->name_len == nameLen &&
                memcmp(de->name, name, nameLen) == 0)
                return de->inode;
            pos += de->rec_len;
        }
        off += g_blockSize;
    }
    return 0;
}

// ---- Tests ----

static int g_tests = 0, g_pass = 0;

#define CHECK(cond, msg) do { \
    g_tests++; \
    if (cond) { g_pass++; } \
    else { printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

static void TestStructSizes() {
    CHECK(sizeof(Ext2Superblock) <= 1024, "Ext2Superblock fits in 1024 bytes");
    CHECK(sizeof(Ext2BlockGroupDesc) == 32, "Ext2BlockGroupDesc is 32 bytes");
    CHECK(sizeof(Ext2Inode) == 128, "Ext2Inode is 128 bytes");
}

static void TestSuperblock() {
    Ext2Superblock sb;
    CHECK(DiskRead(1024, &sb, sizeof(sb)), "Read superblock");
    CHECK(sb.s_magic == EXT2_SUPER_MAGIC, "Superblock magic 0xEF53");
    CHECK(sb.s_blocks_count > 0, "Non-zero block count");
    CHECK(sb.s_inodes_count > 0, "Non-zero inode count");

    g_blockSize = 1024u << sb.s_log_block_size;
    g_blockShift = 10 + sb.s_log_block_size;
    g_inodeSize = (sb.s_rev_level >= 1) ? sb.s_inode_size : 128;
    g_inodesPerGroup = sb.s_inodes_per_group;
    g_groupCount = (sb.s_blocks_count + sb.s_blocks_per_group - 1) / sb.s_blocks_per_group;

    CHECK(g_blockSize == 1024 || g_blockSize == 2048 || g_blockSize == 4096,
          "Valid block size");
    CHECK(g_inodeSize >= 128, "Inode size >= 128");

    printf("  ext2: %u blocks, %u inodes, %u byte blocks, %u groups\n",
           sb.s_blocks_count, sb.s_inodes_count, g_blockSize, g_groupCount);
}

static void TestBGDT() {
    uint64_t bgdtOff = (g_blockSize == 1024) ? 2048 : g_blockSize;
    uint32_t bgdtSize = g_groupCount * sizeof(Ext2BlockGroupDesc);
    g_bgdt = (Ext2BlockGroupDesc*)malloc(bgdtSize);
    CHECK(g_bgdt != nullptr, "BGDT alloc");
    CHECK(DiskRead(bgdtOff, g_bgdt, bgdtSize), "Read BGDT");
    CHECK(g_bgdt[0].bg_inode_table > 0, "Group 0 inode table valid");
}

static void TestRootInode() {
    Ext2Inode root;
    CHECK(ReadInode(EXT2_ROOT_INO, &root), "Read root inode");
    CHECK((root.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR, "Root is a directory");
    CHECK(root.i_size > 0, "Root dir has content");
    CHECK(root.i_links_count >= 2, "Root has >= 2 links");
}

static void TestDirectoryEntries() {
    Ext2Inode root;
    CHECK(ReadInode(EXT2_ROOT_INO, &root), "Read root inode for dir test");

    uint8_t buf[4096];
    uint32_t db = BlockMap(&root, 0);
    CHECK(db > 0, "Root has data block");
    CHECK(DiskRead((uint64_t)db << g_blockShift, buf, g_blockSize), "Read root dir block");

    auto* de = (Ext2DirEntry2*)buf;
    CHECK(de->inode == EXT2_ROOT_INO, ". entry points to root");
    CHECK(de->name_len == 1 && de->name[0] == '.', ". entry name");
}

static void TestFileLookup() {
    Ext2Inode root;
    CHECK(ReadInode(EXT2_ROOT_INO, &root), "Read root for file lookup");

    uint32_t mntIno = DirLookup(&root, "BROOK.MNT");
    CHECK(mntIno > 0, "BROOK.MNT found in root");

    if (mntIno > 0) {
        Ext2Inode mntInode;
        CHECK(ReadInode(mntIno, &mntInode), "Read BROOK.MNT inode");
        CHECK((mntInode.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG, "BROOK.MNT is regular file");
        CHECK(mntInode.i_size == 5, "BROOK.MNT is 5 bytes (/data)");

        uint32_t db = BlockMap(&mntInode, 0);
        CHECK(db > 0, "BROOK.MNT has data block");
        if (db) {
            char data[16] = {};
            CHECK(DiskRead((uint64_t)db << g_blockShift, data, mntInode.i_size),
                  "Read BROOK.MNT data");
            CHECK(memcmp(data, "/data", 5) == 0, "BROOK.MNT contains '/data'");
        }
    }
}

static void TestDirectoryLookup() {
    Ext2Inode root;
    CHECK(ReadInode(EXT2_ROOT_INO, &root), "Read root for dir lookup");

    uint32_t driversIno = DirLookup(&root, "drivers");
    CHECK(driversIno > 0, "drivers/ found in root");

    if (driversIno) {
        Ext2Inode driversInode;
        CHECK(ReadInode(driversIno, &driversInode), "Read drivers inode");
        CHECK((driversInode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR, "drivers is a directory");
    }

    uint32_t binIno = DirLookup(&root, "bin");
    CHECK(binIno > 0, "bin/ found in root");
}

static void TestSymlink() {
    Ext2Inode root;
    if (!ReadInode(EXT2_ROOT_INO, &root)) return;

    uint32_t shIno = DirLookup(&root, "sh");
    if (shIno == 0) {
        printf("  (skipping symlink test — 'sh' not found)\n");
        return;
    }

    Ext2Inode shInode;
    CHECK(ReadInode(shIno, &shInode), "Read sh symlink inode");
    CHECK((shInode.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK, "sh is a symlink");

    if (shInode.i_size <= 60 && shInode.i_blocks == 0) {
        char target[64] = {};
        memcpy(target, shInode.i_block, shInode.i_size);
        printf("  symlink sh -> %s\n", target);
        CHECK(strcmp(target, "bin/busybox") == 0, "sh points to bin/busybox");
    }
}

int main(int argc, char* argv[]) {
    const char* diskPath = (argc >= 2) ? argv[1] : "brook_ext2_disk.img";

    g_disk = fopen(diskPath, "rb");
    if (!g_disk) {
        printf("Cannot open '%s' — skipping ext2 tests.\n", diskPath);
        printf("Create it with: scripts/create_ext2_disk.sh && scripts/update_ext2_disk.sh\n");
        return 0;
    }

    printf("=== ext2 host tests ===\n");
    printf("Disk: %s\n\n", diskPath);

    TestStructSizes();
    TestSuperblock();
    TestBGDT();
    TestRootInode();
    TestDirectoryEntries();
    TestFileLookup();
    TestDirectoryLookup();
    TestSymlink();

    printf("\n%d/%d tests passed\n", g_pass, g_tests);
    if (g_bgdt) free(g_bgdt);
    fclose(g_disk);

    return (g_pass == g_tests) ? 0 : 1;
}
