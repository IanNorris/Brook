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

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "/bigext2";
    const uint64_t denseBytes = argc > 2 ? strtoull(argv[2], 0, 0) : 5 * GiB + 12345;
    make_base();
    buffer = malloc(BUFFER);
    check(buffer != 0, "buffer");
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
