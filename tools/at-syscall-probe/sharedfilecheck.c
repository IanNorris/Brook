#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int failures;
static void fatal(const char *s) { perror(s); exit(2); }
static void check(int ok, const char *label)
{
    printf("SHAREDFILECHECK %s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
static void reject_shared(int fd, int prot, int flags, const char *label)
{
    errno = 0;
    void *p = mmap(NULL, 4096, prot, flags, fd, 0);
    int error = errno;
    check(p == MAP_FAILED && error == EOPNOTSUPP, label);
    if (p != MAP_FAILED && munmap(p, 4096)) fatal("unmap unsupported mapping");
}
int main(int argc, char **argv)
{
    if (argc != 2 || argv[1][0] != '/') {
        fprintf(stderr, "usage: %s absolute-writable-directory\n", argv[0]);
        return 2;
    }
    char path[256];
    if (snprintf(path, sizeof(path), "%s/sharedfile-XXXXXX", argv[1]) >= (int)sizeof(path)) return 2;
    int fd = mkstemp(path);
    if (fd < 0) fatal("mkstemp");
    unsigned char expected[4096], actual[4096];
    for (size_t i = 0; i < sizeof(expected); ++i) expected[i] = i * 17 + 3;
    /* Size the file through real writes: regular ftruncate is still a stub. */
    size_t done = 0;
    while (done < sizeof(expected)) {
        ssize_t n = write(fd, expected+done, sizeof(expected)-done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fatal("write fixture");
        done += n;
    }
    reject_shared(fd, PROT_READ|PROT_WRITE, MAP_SHARED,
                  "regular writable MAP_SHARED reports EOPNOTSUPP");
    reject_shared(fd, PROT_WRITE, MAP_SHARED,
                  "write-only shared file mapping is also rejected");
    reject_shared(fd, PROT_READ|PROT_WRITE, 0x03 /* MAP_SHARED_VALIDATE */,
                  "shared-validate cannot bypass writable-file rejection");

    unsigned char *private = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (private == MAP_FAILED) fatal("private writable mmap");
    check(!memcmp(private, expected, 4096), "writable private mapping initially matches file");
    memset(private, 0x5a, 4096);
    if (munmap(private, 4096)) fatal("private munmap");
    check(pread(fd, actual, 4096, 0) == 4096 && !memcmp(actual, expected, 4096),
          "private mapping writes do not persist to file");
    const unsigned char *readonly = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (readonly == MAP_FAILED) fatal("readonly private mmap");
    check(!memcmp(readonly, expected, 4096), "read-only private mapping remains supported");
    if (munmap((void *)readonly, 4096)) fatal("readonly munmap");

    int memfd = syscall(SYS_memfd_create, "sharedfilecheck", 1 /* MFD_CLOEXEC */);
    if (memfd < 0 || ftruncate(memfd, 4096)) fatal("memfd create/size");
    unsigned char *left = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, memfd, 0);
    unsigned char *right = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, memfd, 0);
    if (left == MAP_FAILED || right == MAP_FAILED) fatal("shared memfd mmap");
    left[123] = 0x5a;
    check(right[123] == 0x5a, "shared memfd first mapping updates second");
    right[321] = 0xa5;
    check(left[321] == 0xa5, "shared memfd second mapping updates first");
    if (munmap(left, 4096) || munmap(right, 4096)) fatal("memfd munmap");
    close(memfd); close(fd);
    if (unlink(path)) fatal("cleanup");
    printf("SHAREDFILECHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
