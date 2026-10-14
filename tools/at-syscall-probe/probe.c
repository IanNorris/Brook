#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "ATCHECK FAIL: %s errno=%d (%s)\n", #expr, errno, strerror(errno)); \
    return 1; } } while (0)

int main(int argc, char **argv) {
    const char *parent = argc > 1 ? argv[1] : "/data/tmp";
    int root = open(parent, O_RDONLY | O_DIRECTORY);
    CHECK(root >= 0);
    char name[64];
    snprintf(name, sizeof(name), "atcheck-%d", getpid());
    CHECK(mkdirat(root, name, 0755) == 0);
    int dir = openat(root, name, O_RDONLY | O_DIRECTORY);
    CHECK(dir >= 0);
    int file = openat(dir, "file", O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(file >= 0);
    CHECK(write(file, "test", 4) == 4);
    CHECK(close(file) == 0);
    CHECK(symlinkat("file", dir, "link") == 0);
    char target[16] = {0};
    CHECK(readlinkat(dir, "link", target, sizeof(target)) == 4);
    CHECK(strcmp(target, "file") == 0);
    file = openat(dir, "link", O_RDONLY);
    CHECK(file >= 0);
    char contents[4];
    CHECK(read(file, contents, sizeof(contents)) == 4);
    CHECK(memcmp(contents, "test", 4) == 0);
    CHECK(close(file) == 0);
    errno = 0;
    CHECK(symlinkat("file", -123, "invalid") == -1 && errno == EBADF);
    CHECK(unlinkat(dir, "link", 0) == 0);
    CHECK(unlinkat(dir, "file", 0) == 0);
    CHECK(close(dir) == 0);
    CHECK(unlinkat(root, name, AT_REMOVEDIR) == 0);
    CHECK(close(root) == 0);
    sync();
    puts("ATCHECK PASS: mkdirat/openat/symlinkat/readlinkat/unlinkat and bad dirfd");
    return 0;
}
