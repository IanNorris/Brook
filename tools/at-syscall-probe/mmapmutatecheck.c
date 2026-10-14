#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static void fatal(const char *s) { perror(s); exit(2); }
static void check(int ok, const char *label)
{
    printf("MMAPMUTATECHECK %s %s\n", ok ? "PASS" : "FAIL", label);
    fflush(stdout);
    failures += !ok;
}
static void write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *p = buffer;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fatal("write");
        p += n; length -= n;
    }
}
static ino_t verify(const char *path, const unsigned char *expected, size_t length, const char *label)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) fatal("open/fstat verification");
    if ((size_t)st.st_size != length) fatal("unexpected fixture size");
    unsigned char direct[16384];
    if (length > sizeof(direct)) fatal("fixture too large");
    size_t done = 0;
    while (done < length) {
        ssize_t n = pread(fd, direct+done, length-done, done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fatal("pread");
        done += n;
    }
    unsigned char *mapped = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) fatal("mmap");
    int ok = !memcmp(direct, expected, length) && !memcmp(mapped, expected, length);
    if (!ok) {
        for (size_t i = 0; i < length; ++i) {
            if (direct[i] != expected[i] || mapped[i] != expected[i]) {
                printf("MMAPMUTATECHECK mismatch offset=0x%zx expected=%02x pread=%02x mmap=%02x\n",
                       i, expected[i], direct[i], mapped[i]);
                break;
            }
        }
    }
    check(ok, label);
    if (munmap(mapped, length) || close(fd)) fatal("close mapping");
    return st.st_ino;
}
int main(int argc, char **argv)
{
    int allow_no_reuse = argc == 3 && !strcmp(argv[2], "--allow-no-reuse");
    if (argc < 2 || argc > 3 || argv[1][0] != '/' || (argc == 3 && !allow_no_reuse)) {
        fprintf(stderr, "usage: %s absolute-writable-directory [--allow-no-reuse]\n", argv[0]);
        return 2;
    }
    char fixture[192], path[256];
    if (snprintf(fixture, sizeof(fixture), "%s/mmapmutate-XXXXXX", argv[1]) >= (int)sizeof(fixture)) return 2;
    if (!mkdtemp(fixture)) fatal("mkdtemp");
    snprintf(path, sizeof(path), "%s/source", fixture);
    unsigned char expected[8192];
    memset(expected, 'A', sizeof(expected));
    int fd = open(path, O_CREAT|O_EXCL|O_RDWR, 0600);
    if (fd < 0) fatal("create");
    write_all(fd, expected, 4090); close(fd);
    ino_t first = verify(path, expected, 4090, "prime partial final page");

    /* Old cached EOF padding must not replace the six newly appended bytes. */
    fd = open(path, O_WRONLY);
    if (fd < 0 || lseek(fd, 4090, SEEK_SET) != 4090) fatal("append open/seek");
    memcpy(expected+4090, "TAIL!\n", 6);
    write_all(fd, expected+4090, 6); close(fd);
    check(verify(path, expected, 4096, "new mapping sees six-byte append over old EOF padding") == first,
          "append preserves inode");

    fd = open(path, O_WRONLY);
    if (fd < 0) fatal("pwrite open");
    memset(expected+128, 'B', 127);
    if (pwrite(fd, expected+128, 127, 128) != 127) fatal("pwrite");
    close(fd);
    verify(path, expected, 4096, "new mapping sees in-place pwrite");

    fd = open(path, O_WRONLY|O_TRUNC);
    if (fd < 0) fatal("truncate open");
    memset(expected, 'C', sizeof(expected));
    write_all(fd, expected, sizeof(expected)); close(fd);
    check(verify(path, expected, sizeof(expected), "new mapping sees O_TRUNC replacement and growth") == first,
          "O_TRUNC preserves inode");

    /* Close/unmap before unlink: open-unlinked vnode lifetime is separate. */
    if (unlink(path)) fatal("unlink");
    fd = open(path, O_CREAT|O_EXCL|O_RDWR, 0600);
    if (fd < 0) fatal("recreate");
    memset(expected, 'D', sizeof(expected));
    write_all(fd, expected, 5130); close(fd);
    ino_t second = verify(path, expected, 5130, "new mapping sees recreated file bytes");
    if (first != second && allow_no_reuse)
        printf("MMAPMUTATECHECK SKIP inode reuse allocator chose %ju instead of %ju\n",
               (uintmax_t)second, (uintmax_t)first);
    else check(first == second, "recreated fixture reuses previously cached inode");
    printf("MMAPMUTATECHECK inode_before=%ju inode_after=%ju reused=%d\n",
           (uintmax_t)first, (uintmax_t)second, first == second);
    if (unlink(path) || rmdir(fixture)) fatal("cleanup");
    printf("MMAPMUTATECHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
