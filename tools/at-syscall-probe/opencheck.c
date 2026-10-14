#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static unsigned char sample[64];
static ssize_t sample_size;
static void fatal(const char *s) { perror(s); exit(1); }
static void check(int ok, const char *s)
{
    printf("OPENCHECK %s %s\n", ok ? "PASS" : "FAIL", s);
    failures += !ok;
}
static void missing(const char *path, int flags, int expected, const char *label)
{
    errno = 0;
    int fd = open(path, flags);
    int error = errno;
    if (fd >= 0) close(fd);
    check(fd == -1 && error == expected, label);
}
static void same_library(const char *path, const char *label)
{
    unsigned char data[sizeof(sample)];
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { check(0, label); return; }
    ssize_t n = read(fd, data, sizeof(data));
    check(n == sample_size && n > 0 && !memcmp(data, sample, n) &&
          fstat(fd, &st) == 0 && S_ISREG(st.st_mode), label);
    close(fd);
}
int main(int argc, char **argv)
{
    if (argc != 3 || argv[1][0] != '/' || strncmp(argv[2], "/boot/lib/", 10)) {
        fprintf(stderr, "usage: %s absolute-writable-directory /boot/lib/existing-library.so.VERSION\n", argv[0]);
        return 2;
    }
    const char *basename = strrchr(argv[2], '/') + 1;
    if (!*basename || !strstr(basename, ".so")) return 2;
    int fd = open(argv[2], O_RDONLY);
    if (fd < 0) fatal("reference boot library");
    sample_size = read(fd, sample, sizeof(sample));
    close(fd);
    if (sample_size <= 0) fatal("reference library read");
    char fixture[192], path[512];
    if (snprintf(fixture, sizeof(fixture), "%s/opencheck-XXXXXX", argv[1]) >= (int)sizeof(fixture)) return 2;
    if (!mkdtemp(fixture)) fatal("mkdtemp");
    snprintf(path, sizeof(path), "%s/refs/bisect/", fixture);
    missing(path, O_RDONLY|O_DIRECTORY, ENOENT, "missing trailing-slash Git refs directory returns ENOENT");
    missing(path, O_RDONLY, ENOENT, "missing trailing-slash path without O_DIRECTORY returns ENOENT");
    snprintf(path, sizeof(path), "%s/refs/worktree", fixture);
    missing(path, O_RDONLY|O_DIRECTORY, ENOENT, "missing Git refs directory without slash returns ENOENT");
    snprintf(path, sizeof(path), "%s/%s", fixture, basename);
    missing(path, O_RDONLY, ENOENT, "unrelated missing pathname cannot alias an existing boot library");
    snprintf(path, sizeof(path), "/nix/store/__opencheck_missing_%ld/lib/", (long)getpid());
    missing(path, O_RDONLY, ENOENT, "library-prefix missing directory cannot alias /boot/lib");
    snprintf(path, sizeof(path), "/usr/lib/__opencheck_missing_%ld/%s", (long)getpid(), basename);
    missing(path, O_RDONLY|O_DIRECTORY, ENOENT, "O_DIRECTORY cannot use a library basename alias");
    missing(path, O_WRONLY, ENOENT, "write open cannot mutate a library through basename alias");
    same_library(path, "legacy library prefix retains read-only basename compatibility");
    snprintf(path, sizeof(path), "/lib/%s", basename);
    same_library(path, "exact /boot-prefix library compatibility is preserved");
    missing(path, O_RDONLY|O_DIRECTORY, ENOTDIR, "existing library requested as directory reports ENOTDIR");
    snprintf(path, sizeof(path), "/nix/store/__opencheck_missing_%ld/lib/%s", (long)getpid(), basename);
    same_library(path, "Nix library basename compatibility is preserved");
    struct stat st;
    if (stat("/boot/lib/console_history.txt", &st) == 0 && S_ISREG(st.st_mode)) {
        snprintf(path, sizeof(path), "/usr/lib/__opencheck_missing_%ld/console_history.txt", (long)getpid());
        missing(path, O_RDONLY, ENOENT, "non-library boot basename is never aliased");
    }
    if (rmdir(fixture)) fatal("rmdir fixture");
    printf("OPENCHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
