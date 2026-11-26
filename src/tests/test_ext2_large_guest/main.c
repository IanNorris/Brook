// Guest half of scripts/test_ext2_large_file_qemu.py: files above 4 GiB on
// an ext2 volume. Prints EXT2_LARGE_* lines; the host checks the hashes
// against its own model and the image with e2fsck/debugfs afterwards.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#define KiB 1024ULL
#define MiB (1024ULL * KiB)
#define GiB (1024ULL * MiB)

static void check(bool value, const char *step)
{
    if (value) return;
    dprintf(1, "EXT2_LARGE_FAIL %s errno=%d\n", step, errno);
    _exit(1);
}

// ---- SHA-256 (FIPS 180-4) ----
typedef struct { uint32_t h[8]; uint64_t bytes; uint8_t block[64]; unsigned used; } Sha256;
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_init(Sha256 *s)
{
    static const uint32_t init[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h, init, sizeof(init)); s->bytes = 0; s->used = 0;
}
static void sha_block(Sha256 *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + (ROR(e,6) ^ ROR(e,11) ^ ROR(e,25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a,2) ^ ROR(a,13) ^ ROR(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}
static void sha_update(Sha256 *s, const uint8_t *p, size_t n)
{
    s->bytes += n;
    if (s->used) {
        while (n && s->used < 64) { s->block[s->used++] = *p++; --n; }
        if (s->used < 64) return;
        sha_block(s, s->block); s->used = 0;
    }
    for (; n >= 64; n -= 64, p += 64) sha_block(s, p);
    memcpy(s->block, p, n); s->used = (unsigned)n;
}
static void sha_final(Sha256 *s, char out[65])
{
    uint64_t bits = s->bytes * 8;
    uint8_t pad = 0x80;
    sha_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56) sha_update(s, &pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, len, 8);
    for (int i = 0; i < 8; ++i) sprintf(out + 8 * i, "%08x", s->h[i]);
}

// ---- Content model, shared with the host script ----
// A 1 MiB LCG buffer; every 4 KiB block starts with its own little-endian
// file offset, so a wrapped offset never reads back as the expected data.
static uint8_t base[MiB];
static void make_base(void)
{
    uint64_t x = 1;
    for (uint64_t i = 0; i < MiB; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        base[i] = (uint8_t)(x >> 56);
    }
}
static uint8_t pattern(uint64_t offset)
{
    uint64_t block = offset & ~4095ULL, within = offset - block;
    if (within < 8) return (uint8_t)(block >> (8 * within));
    return base[offset % MiB];
}
static void fill(uint8_t *out, uint64_t offset, uint64_t length)
{
    for (uint64_t i = 0; i < length; ++i) out[i] = pattern(offset + i);
}

struct region { uint64_t offset, length; };
#define TRIPLE_START ((12 + 1024 + 1024ULL * 1024) * 4096) // 4 KiB blocks
static const struct region regions[] = {
    {0, 64 * KiB},
    {4 * GiB - 2048, 4096},                  // 4 GiB - 1, 4 GiB, 4 GiB + 1
    {TRIPLE_START - 8 * KiB, 16 * KiB},      // doubly -> triply indirect
    {5 * GiB, 64 * KiB},
    {6 * GiB - 64 * KiB, 64 * KiB},          // end region
};
#define REGIONS (sizeof(regions) / sizeof(regions[0]))
static bool in_region(uint64_t offset)
{
    for (unsigned i = 0; i < REGIONS; ++i)
        if (offset >= regions[i].offset && offset < regions[i].offset + regions[i].length) return true;
    return false;
}

static uint8_t *buffer;
#define BUFFER (4 * MiB)

// Read the whole file, compare with the model (regions or dense), hash it.
static void hash_file(int fd, uint64_t size, bool dense, const char *label)
{
    Sha256 sha; sha_init(&sha);
    uint64_t at = 0;
    check(lseek(fd, 0, SEEK_SET) == 0, "rewind");
    while (at < size) {
        ssize_t got = read(fd, buffer, BUFFER);
        check(got > 0, "read whole file");
        for (ssize_t i = 0; i < got; ++i) {
            uint8_t want = dense || in_region(at + i) ? pattern(at + i) : 0;
            if (buffer[i] != want) {
                dprintf(1, "EXT2_LARGE_FAIL %s content at %llu got %u want %u\n", label,
                        (unsigned long long)(at + i), buffer[i], want);
                _exit(1);
            }
        }
        sha_update(&sha, buffer, (size_t)got);
        at += (uint64_t)got;
    }
    check(at == size && read(fd, buffer, 1) == 0, "EOF after size");
    char digest[65]; sha_final(&sha, digest);
    dprintf(1, "EXT2_LARGE_HASH %s size=%llu sha256=%s\n", label, (unsigned long long)size, digest);
}

static void expect_size(int fd, uint64_t size, const char *step)
{
    struct stat st;
    check(fstat(fd, &st) == 0 && (uint64_t)st.st_size == size, step);
    check(lseek(fd, 0, SEEK_END) == (off_t)size, "SEEK_END");
}


static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static uint64_t free_bytes(const char *root)
{
    struct statfs fs;
    check(statfs(root, &fs) == 0, "statfs");
    return (uint64_t)fs.f_bfree * fs.f_bsize;
}

// fallocate, preadv/pwritev(2) and copy_file_range on files above 4 GiB.
static void falloc_phase(const char *root)
{
    char path[256], copy[256], fill_path[256];
    snprintf(path, sizeof(path), "%s/prealloc", root);
    snprintf(copy, sizeof(copy), "%s/copied", root);
    snprintf(fill_path, sizeof(fill_path), "%s/fill", root);
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    check(fd >= 0, "create prealloc");
    const uint64_t before = free_bytes(root);
    uint64_t start = now_ms();
    check(fallocate(fd, 0, 0, (off_t)(6 * GiB)) == 0, "fallocate 6 GiB");
    uint64_t elapsed = now_ms() - start;
    const uint64_t used = before - free_bytes(root);
    dprintf(1, "EXT2_LARGE_FALLOC size=%llu ms=%llu used=%llu\n", (unsigned long long)(6 * GiB),
            (unsigned long long)elapsed, (unsigned long long)used);
    check(used >= 6 * GiB, "fallocate allocated the blocks");
    expect_size(fd, 6 * GiB, "fallocate size");
    const uint64_t zeros[] = {0, 4 * GiB - 1, 4 * GiB, TRIPLE_START, 6 * GiB - 1};
    for (unsigned i = 0; i < sizeof(zeros) / sizeof(zeros[0]); ++i) {
        uint8_t byte = 0xee;
        check(pread(fd, &byte, 1, (off_t)zeros[i]) == 1 && byte == 0, "fallocated byte is zero");
    }
    // Already allocated: quick, nothing more used.
    start = now_ms();
    check(fallocate(fd, 0, 0, (off_t)(6 * GiB)) == 0, "fallocate again");
    dprintf(1, "EXT2_LARGE_FALLOC_AGAIN ms=%llu\n", (unsigned long long)(now_ms() - start));
    check(before - free_bytes(root) == used, "refallocate uses nothing");
    // KEEP_SIZE past EOF keeps the size.
    check(fallocate(fd, FALLOC_FL_KEEP_SIZE, (off_t)(6 * GiB - 4096), 1 << 20) == 0, "fallocate KEEP_SIZE");
    expect_size(fd, 6 * GiB, "KEEP_SIZE size");
    errno = 0;
    check(fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 4096) == -1 && errno == EOPNOTSUPP,
          "punch hole unsupported");

    // pwritev/preadv across 4 GiB, then the v2 forms with the file position.
    static uint8_t a[3000], b[5000], c[100], ra[3000], rb[5000], rc[100];
    const uint64_t at = 4 * GiB - 4000;
    fill(a, at, sizeof(a)); fill(b, at + sizeof(a), sizeof(b)); fill(c, at + sizeof(a) + sizeof(b), sizeof(c));
    struct iovec out[3] = {{a, sizeof(a)}, {b, sizeof(b)}, {c, sizeof(c)}};
    check(pwritev(fd, out, 3, (off_t)at) == 8100, "pwritev");
    struct iovec in[3] = {{ra, sizeof(ra)}, {rb, sizeof(rb)}, {rc, sizeof(rc)}};
    check(preadv(fd, in, 3, (off_t)at) == 8100, "preadv");
    check(!memcmp(a, ra, sizeof(a)) && !memcmp(b, rb, sizeof(b)) && !memcmp(c, rc, sizeof(c)), "preadv content");
    check(lseek(fd, 0, SEEK_CUR) == (off_t)(6 * GiB), "preadv keeps position");
    check(lseek(fd, (off_t)at, SEEK_SET) == (off_t)at, "seek for v2");
    memset(ra, 0, sizeof(ra));
    struct iovec one = {ra, sizeof(ra)};
    check(preadv2(fd, &one, 1, -1, 0) == (ssize_t)sizeof(ra) && !memcmp(a, ra, sizeof(a)), "preadv2 at position");
    check(lseek(fd, 0, SEEK_CUR) == (off_t)(at + sizeof(ra)), "preadv2 advances position");
    struct iovec two = {c, sizeof(c)};
    check(pwritev2(fd, &two, 1, (off_t)(5 * GiB), RWF_DSYNC) == (ssize_t)sizeof(c), "pwritev2 DSYNC");
    check(pread(fd, rc, sizeof(rc), (off_t)(5 * GiB)) == (ssize_t)sizeof(rc) && !memcmp(c, rc, sizeof(c)),
          "pwritev2 content");
    errno = 0;
    check(preadv2(fd, &one, 1, 0, RWF_NOWAIT) == -1 && errno == EOPNOTSUPP, "RWF_NOWAIT refused");

    // copy_file_range from past 4 GiB.
    int out_fd = open(copy, O_CREAT | O_RDWR | O_TRUNC, 0644);
    check(out_fd >= 0, "create copy");
    loff_t from = (loff_t)at, to = 10;
    ssize_t copied = 0;
    while (copied < 1 << 20) {
        ssize_t n = copy_file_range(fd, &from, out_fd, &to, (size_t)(1 << 20) - copied, 0);
        check(n > 0, "copy_file_range");
        copied += n;
    }
    check(from == (loff_t)(at + (1 << 20)) && to == 10 + (1 << 20), "copy_file_range offsets");
    check(pread(out_fd, rb, sizeof(rb), 10) == (ssize_t)sizeof(rb), "read copy");
    for (unsigned i = 0; i < sizeof(a); ++i) check(rb[i] == a[i], "copy content");
    for (unsigned i = sizeof(a); i < sizeof(rb); ++i) check(rb[i] == b[i - sizeof(a)], "copy content b");
    errno = 0;
    from = 0;
    check(copy_file_range(fd, &from, fd, &from, 4096, 0) == -1 && errno == EINVAL, "overlap refused");
    close(out_fd);
    check(unlink(copy) == 0, "unlink copy");

    // ENOSPC: fallocate more than is free; the allocated prefix stays.
    int full = open(fill_path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    check(full >= 0, "create fill");
    const uint64_t avail = free_bytes(root);
    errno = 0;
    start = now_ms();
    check(fallocate(full, 0, 0, (off_t)(avail + GiB)) == -1 && errno == ENOSPC, "fallocate ENOSPC");
    struct stat st;
    check(fstat(full, &st) == 0, "fstat fill");
    dprintf(1, "EXT2_LARGE_FALLOC_ENOSPC free=%llu kept=%llu left=%llu ms=%llu\n",
            (unsigned long long)avail, (unsigned long long)st.st_size,
            (unsigned long long)free_bytes(root), (unsigned long long)(now_ms() - start));
    check((uint64_t)st.st_size > avail / 2 && (uint64_t)st.st_size <= avail, "ENOSPC keeps prefix");
    // Use up the rest (fallocate rolls back its last 64-block batch), then
    // a write must fail with ENOSPC rather than return 0.
    check(lseek(full, 0, SEEK_END) == st.st_size, "seek fill end");
    memset(buffer, 7, 64 * KiB);
    ssize_t wrote;
    while ((wrote = write(full, buffer, 64 * KiB)) > 0) {}
    check(wrote == -1 && errno == ENOSPC, "write until ENOSPC");
    errno = 0;
    check(pwrite(full, "x", 1, (off_t)(avail + GiB)) == -1 && errno == ENOSPC, "write on full fs is ENOSPC");
    check(ftruncate(full, 0) == 0, "release fill");
    close(full);
    check(unlink(fill_path) == 0, "unlink fill");
    check(fsync(fd) == 0, "fsync prealloc");
    close(fd);
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "/bigext2";
    const uint64_t denseBytes = argc > 2 ? strtoull(argv[2], 0, 0) : 5 * GiB + 12345;
    make_base();
    buffer = malloc(BUFFER);
    check(buffer != 0, "buffer");
    if (argc > 3 && !strcmp(argv[3], "falloc")) {
        falloc_phase(root);
        sync();
        dprintf(1, "EXT2_LARGE_QEMU_PASS\n");
        return 0;
    }
    char path[256], copy[256], dense[256];
    snprintf(path, sizeof(path), "%s/sparse", root);
    snprintf(copy, sizeof(copy), "%s/sendfile-copy", root);
    snprintf(dense, sizeof(dense), "%s/dense", root);

    // ---- Sparse 6 GiB ----
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    check(fd >= 0, "create sparse");
    for (unsigned i = 0; i < REGIONS; ++i) {
        fill(buffer, regions[i].offset, regions[i].length);
        check(pwrite(fd, buffer, regions[i].length, (off_t)regions[i].offset) == (ssize_t)regions[i].length,
              "pwrite region");
    }
    expect_size(fd, 6 * GiB, "sparse size");
    const uint64_t probes[] = {4 * GiB - 1, 4 * GiB, 4 * GiB + 1, TRIPLE_START, 5 * GiB, 6 * GiB - 1};
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        uint8_t byte = 0;
        check(pread(fd, &byte, 1, (off_t)probes[i]) == 1 && byte == pattern(probes[i]), "pread probe");
    }
    uint8_t byte;
    check(pread(fd, &byte, 1, (off_t)(6 * GiB)) == 0, "pread at EOF");
    check(pread(fd, &byte, 1, (off_t)(4 * GiB + 64 * KiB)) == 1 && byte == 0, "pread hole past 4 GiB");

    // mmap a window straddling 4 GiB.
    uint8_t *map = mmap(0, 8192, PROT_READ, MAP_SHARED, fd, (off_t)(4 * GiB - 4096));
    if (map == MAP_FAILED) {
        dprintf(1, "EXT2_LARGE_NOTE mmap past 4 GiB errno=%d\n", errno);
        check(false, "mmap past 4 GiB");
    }
    for (uint64_t i = 4096 - 2048; i < 4096 + 2048; ++i)
        check(map[i] == pattern(4 * GiB - 4096 + i), "mmap content past 4 GiB");
    check(munmap(map, 8192) == 0, "munmap");

    // sendfile from an offset past 4 GiB.
    int out = open(copy, O_CREAT | O_RDWR | O_TRUNC, 0644);
    check(out >= 0, "create sendfile target");
    off_t from = (off_t)(4 * GiB - 1000);
    check(sendfile(out, fd, &from, 2000) == 2000 && from == (off_t)(4 * GiB + 1000), "sendfile");
    check(pread(out, buffer, 2000, 0) == 2000, "read sendfile copy");
    for (uint64_t i = 0; i < 2000; ++i) check(buffer[i] == pattern(4 * GiB - 1000 + i), "sendfile content");
    close(out);
    check(unlink(copy) == 0, "unlink sendfile copy");

    hash_file(fd, 6 * GiB, false, "sparse-6g");
    check(ftruncate(fd, (off_t)(4 * GiB + GiB / 2)) == 0, "truncate 4.5 GiB");
    expect_size(fd, 4 * GiB + GiB / 2, "size 4.5 GiB");
    check(pread(fd, &byte, 1, (off_t)(5 * GiB)) == 0, "read past truncated EOF");
    hash_file(fd, 4 * GiB + GiB / 2, false, "sparse-4.5g");
    check(ftruncate(fd, (off_t)(3 * GiB)) == 0, "truncate 3 GiB");
    expect_size(fd, 3 * GiB, "size 3 GiB");
    hash_file(fd, 3 * GiB, false, "sparse-3g");
    check(fsync(fd) == 0, "fsync sparse");
    close(fd);

    // ---- Dense ----
    if (denseBytes) {
        struct statfs fs;
        check(statfs(root, &fs) == 0, "statfs");
        dprintf(1, "EXT2_LARGE_STATFS bsize=%llu blocks=%llu free=%llu\n",
                (unsigned long long)fs.f_bsize, (unsigned long long)fs.f_blocks,
                (unsigned long long)fs.f_bfree);
        check((uint64_t)fs.f_bfree * fs.f_bsize > denseBytes + 64 * MiB, "space for dense file");
        fd = open(dense, O_CREAT | O_RDWR | O_TRUNC, 0644);
        check(fd >= 0, "create dense");
        Sha256 sha; sha_init(&sha);
        for (uint64_t at = 0; at < denseBytes;) {
            uint64_t n = denseBytes - at < BUFFER ? denseBytes - at : BUFFER;
            fill(buffer, at, n);
            sha_update(&sha, buffer, n);
            ssize_t wrote = write(fd, buffer, n);
            check(wrote == (ssize_t)n, "dense write");
            at += n;
        }
        char digest[65]; sha_final(&sha, digest);
        dprintf(1, "EXT2_LARGE_WROTE dense size=%llu sha256=%s\n", (unsigned long long)denseBytes, digest);
        check(fsync(fd) == 0, "fsync dense");
        expect_size(fd, denseBytes, "dense size");
        hash_file(fd, denseBytes, true, "dense");
        close(fd);
        // O_APPEND past 4 GiB lands at EOF.
        fd = open(dense, O_WRONLY | O_APPEND);
        check(fd >= 0, "open append");
        fill(buffer, denseBytes, 100);
        check(write(fd, buffer, 100) == 100, "append write");
        expect_size(fd, denseBytes + 100, "size after append");
        close(fd);
        fd = open(dense, O_RDONLY);
        check(fd >= 0, "reopen dense");
        check(pread(fd, buffer, 200, (off_t)(denseBytes - 100)) == 200, "read appended tail");
        for (uint64_t i = 0; i < 200; ++i) check(buffer[i] == pattern(denseBytes - 100 + i), "appended content");
        close(fd);
        dprintf(1, "EXT2_LARGE_DENSE_FINAL size=%llu\n", (unsigned long long)(denseBytes + 100));
    }
    sync();
    dprintf(1, "EXT2_LARGE_QEMU_PASS\n");
    return 0;
}
