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

#ifndef SYS_faccessat2
#define SYS_faccessat2 439
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

static int failures;
static void fatal(const char *s) { perror(s); exit(1); }
static void result(long value, int error, int expected, const char *label)
{
    int ok = expected ? value == -1 && error == expected : value == 0;
    printf("ACCESSCHECK %s %s (result=%ld errno=%d)\n",
           ok ? "PASS" : "FAIL", label, value, error);
    failures += !ok;
}
#define CHECK(call, expected, label) do { \
    errno = 0; long value = (call); int error = errno; \
    result(value, error, expected, label); \
} while (0)

int main(int argc, char **argv)
{
    if (argc != 2 || argv[1][0] != '/') {
        fprintf(stderr, "usage: %s absolute-writable-directory\n", argv[0]);
        return 2;
    }
    char fixture[192], absolute[256], original[512];
    if (!getcwd(original, sizeof(original))) fatal("getcwd");
    if (snprintf(fixture, sizeof(fixture), "%s/accesscheck-XXXXXX", argv[1]) >= (int)sizeof(fixture)) return 2;
    if (!mkdtemp(fixture) || chdir(fixture) || mkdir(".git", 0700)) fatal("fixture");
    int file = open(".git/config", O_CREAT|O_EXCL|O_RDWR, 0600);
    if (file < 0 || write(file, "[core]\n", 7) != 7) fatal("config");
    int directory = open(fixture, O_RDONLY|O_DIRECTORY);
    if (directory < 0 || symlink("missing-target", "dangling")) fatal("directory/symlink");
    snprintf(absolute, sizeof(absolute), "%s/.git/config", fixture);

    CHECK(rename(".git/config", ".git/config.new"), 0, "relative rename uses cwd");
    CHECK(rename(".git/config.new", ".git/config"), 0, "relative rename restores config");

    /* Use actual syscall 21 on x86-64, bypassing libc's faccessat fallback. */
    CHECK(syscall(SYS_access, ".git/config", R_OK), 0, "raw access relative Git config after chdir");
    CHECK(syscall(SYS_access, absolute, R_OK), 0, "raw access absolute same config");
    CHECK(syscall(SYS_faccessat, AT_FDCWD, ".git/config", F_OK), 0, "faccessat AT_FDCWD");
    CHECK(syscall(SYS_access, "missing", F_OK), ENOENT, "missing relative path");
    CHECK(syscall(SYS_access, "", F_OK), ENOENT, "empty access pathname");
    CHECK(syscall(SYS_access, absolute, 8), EINVAL, "invalid access mode");
    CHECK(syscall(SYS_access, (char *)NULL, F_OK), EFAULT, "null access pathname");
    CHECK(syscall(SYS_access, "/etc/passwd", F_OK), 0, "synthetic passwd existence retained");

    if (chdir("/")) fatal("chdir root");
    CHECK(syscall(SYS_faccessat, directory, ".git/config", R_OK), 0, "directory fd resolves independently of cwd");
    CHECK(syscall(SYS_faccessat2, directory, ".git/config", R_OK, AT_EACCESS), 0, "faccessat2 effective-id flag accepted");
    CHECK(syscall(SYS_faccessat, -1, ".git/config", F_OK), EBADF, "invalid relative dirfd");
    CHECK(syscall(SYS_faccessat, file, ".git/config", F_OK), ENOTDIR, "regular file is not a directory fd");
    CHECK(syscall(SYS_faccessat, -1, absolute, F_OK), 0, "absolute path ignores invalid dirfd");
    CHECK(syscall(SYS_faccessat2, directory, "dangling", F_OK, AT_SYMLINK_NOFOLLOW), 0, "nofollow sees dangling symlink");
    CHECK(syscall(SYS_faccessat2, directory, "dangling", F_OK, 0), ENOENT, "following dangling symlink fails");
    CHECK(syscall(SYS_faccessat2, file, "", F_OK, AT_EMPTY_PATH), 0, "empty path accepts regular file fd");
    CHECK(syscall(SYS_faccessat2, AT_FDCWD, "", F_OK, AT_EMPTY_PATH), 0, "empty path accepts cwd");
    CHECK(syscall(SYS_faccessat2, -1, "", F_OK, AT_EMPTY_PATH), EBADF, "empty path validates fd");
    CHECK(syscall(SYS_faccessat2, directory, "", F_OK, 0), ENOENT, "empty path requires flag");
    CHECK(syscall(SYS_faccessat2, directory, ".git/config", F_OK, 0x40000000), EINVAL, "unknown faccessat2 flag");

    char too_long[300];
    memset(too_long, 'a', sizeof(too_long)-1); too_long[sizeof(too_long)-1] = 0;
    CHECK(syscall(SYS_access, too_long, F_OK), ENAMETOOLONG, "oversize pathname rejected without truncation");
    long page = sysconf(_SC_PAGESIZE);
    char *guard = mmap(NULL, 2*page, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (guard == MAP_FAILED || munmap(guard+page, page)) fatal("unmapped guard page");
    guard[page-1] = 0;
    CHECK(syscall(SYS_faccessat2, file, guard+page-1, F_OK, AT_EMPTY_PATH), 0, "NUL in final mapped byte");
    guard[page-1] = 'x';
    CHECK(syscall(SYS_access, guard+page-1, F_OK), EFAULT, "pathname crosses truly unmapped page");
    CHECK(syscall(SYS_faccessat, directory, guard+page, F_OK), EFAULT, "unmapped pathname start");
    if (munmap(guard, page)) fatal("munmap");

    close(directory); close(file);
    if (chdir(fixture) || unlink("dangling") || unlink(".git/config") ||
        rmdir(".git") || chdir(original) || rmdir(fixture)) fatal("cleanup");
    printf("ACCESSCHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
