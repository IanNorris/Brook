#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(bool value, const char *step)
{
    if (value) return;
    dprintf(2, "PATH_MAX_FAIL %s errno=%d\n", step, errno);
    _exit(1);
}

static void path_join(char out[PATH_MAX], const char *root, const char *leaf)
{
    int count = snprintf(out, PATH_MAX, "%s/%s", root, leaf);
    check(count > 0 && count < PATH_MAX, "fixture path");
}

static void make_tree(char path[PATH_MAX], const char *root, const char *name)
{
    path_join(path, root, name);
    check(mkdir(path, 0755) == 0, "tree root");
    for (unsigned i = 0; i < 10; ++i) {
        size_t n = strlen(path);
        path[n++] = '/';
        memset(path + n, 'A' + i, 120);
        path[n + 120] = 0;
        check(mkdir(path, 0755) == 0, "nested mkdir");
    }
    check(strlen(path) > 1024, "deep path length");
}

static void copy_file(const char *source, const char *target)
{
    int in = open(source, O_RDONLY);
    check(in >= 0, "open copy source");
    int out = open(target, O_CREAT | O_WRONLY | O_EXCL, 0755);
    check(out >= 0, "open copy target");
    for (;;) {
        char buf[4096];
        ssize_t n = read(in, buf, sizeof(buf));
        check(n >= 0, "read");
        if (!n) break;
        ssize_t done = 0;
        while (done < n) {
            ssize_t written = write(out, buf + done, n - done);
            check(written > 0, "write");
            done += written;
        }
    }
    check(close(out) == 0 && close(in) == 0, "close copy");
}

static void verify_link(const char *name, const char *expected)
{
    char text[PATH_MAX];
    ssize_t size = readlink(name, text, sizeof(text));
    check(size == (ssize_t)strlen(expected) && !memcmp(text, expected, size), "readlink long target");
    memset(text, 0x55, sizeof(text));
    check(readlink(name, text, 19) == 19 && !memcmp(text, expected, 19) &&
          text[19] == 0x55, "readlink short output");
}

static void exercise(const char *root, bool unix_links)
{
    char from[PATH_MAX], to[PATH_MAX], copied[PATH_MAX], renamed[PATH_MAX], path[PATH_MAX];
    char resolved_root[PATH_MAX], expected_cwd[PATH_MAX];
    check(chdir(root) == 0 && getcwd(resolved_root, sizeof(resolved_root)) && chdir("/") == 0,
          "canonical volume root");
    make_tree(from, root, "FROM");
    make_tree(to, root, "TO");
    path_join(expected_cwd, resolved_root, to + strlen(root) + 1);
    path_join(path, from, "CLIENT.ELF");
    copy_file("/boot/PATHQA.ELF", path);
    path_join(copied, to, "COPY.ELF");
    copy_file(path, copied);
    struct stat src, dst;
    check(stat(path, &src) == 0 && lstat(copied, &dst) == 0 &&
          src.st_size > 0 && src.st_size == dst.st_size, "deep stat and copy");
    path_join(renamed, to, "RUN.ELF");
    check(rename(copied, renamed) == 0, "deep rename");
    int fd = open(to, O_DIRECTORY | O_RDONLY);
    check(fd >= 0 && fstatat(fd, "RUN.ELF", &dst, 0) == 0, "dirfd stat");

    char component[257];
    memset(component, 'Z', 256);
    component[255] = 0;
    int leaf = openat(fd, component, O_CREAT | O_WRONLY | O_EXCL, 0600);
    check(leaf >= 0 && close(leaf) == 0, "255-byte leaf");
    component[255] = 'Z';
    component[256] = 0;
    check(openat(fd, component, O_RDONLY) == -1 && errno == ENAMETOOLONG, "256-byte leaf");
    component[255] = 0;
    check(unlinkat(fd, component, 0) == 0, "unlinkat leaf");
    check(close(fd) == 0, "close dirfd");

    size_t base = strlen(to);
    memcpy(path, to, base);
    memset(path + base, '/', PATH_MAX - 1 - base - strlen("RUN.ELF"));
    memcpy(path + PATH_MAX - 1 - strlen("RUN.ELF"), "RUN.ELF", sizeof("RUN.ELF"));
    check(strlen(path) == 4095 && stat(path, &dst) == 0, "4095-byte pathname");
    char over[PATH_MAX + 1];
    over[0] = '/';
    memcpy(over + 1, path, sizeof(path));
    check(strlen(over) == 4096 && stat(over, &dst) == -1 && errno == ENAMETOOLONG, "4096-byte pathname");

    check(chdir(to) == 0, "chdir long path");
    check(getcwd(path, sizeof(path)) != NULL && !strcmp(path, expected_cwd), "getcwd long path");
    char tiny[32];
    check(!getcwd(tiny, sizeof(tiny)) && errno == ERANGE, "short cwd output");
    verify_link("/proc/self/cwd", expected_cwd);
    check(chdir("/") == 0, "return to root");

    if (unix_links) {
        path_join(path, from, "deep-link");
        check(symlink(renamed, path) == 0, "deep symlink");
        verify_link(path, renamed);
        check(stat(path, &src) == 0 && src.st_ino == dst.st_ino, "follow long symlink");
        path_join(path, from, "hard-link");
        check(link(renamed, path) == 0 && stat(path, &src) == 0 && src.st_ino == dst.st_ino, "deep hard link");

        char executable[PATH_MAX];
        path_join(executable, expected_cwd, "RUN.ELF");
        pid_t child = fork();
        check(child >= 0, "fork exec");
        if (!child) {
            char *const argv[] = {renamed, "--child", executable, NULL};
            execv(renamed, argv);
            check(false, "exec long path");
        }
        int status = 0;
        check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "long executable path and proc readlink");
    }
    dprintf(1, "PATH_MAX_VOLUME_PASS %s length=%zu\n", root, strlen(renamed));
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--child")) {
        verify_link("/proc/self/exe", argv[2]);
        return 0;
    }
    struct stat root, tmp, ext2;
    check(stat("/", &root) == 0 && stat("/tmp", &tmp) == 0 &&
          stat("/longext2", &ext2) == 0 && root.st_dev != tmp.st_dev &&
          root.st_dev != ext2.st_dev && tmp.st_dev != ext2.st_dev, "separate FAT, tmpfs and ext2 volumes");
    exercise("/tmp", true);
    exercise("/longext2", true);
    check(mkdir("/FATQA", 0755) == 0, "FAT fixture root");
    exercise("/FATQA", false);
    dprintf(1, "PATH_MAX_QEMU_PASS\n");
    return 0;
}
