#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "BOOT_STORAGE_ACCESS_FAIL line=%d check=%s errno=%d\n", \
                __LINE__, #condition, errno); \
        _exit(2); \
    } \
} while (0)

static const char *const esp = "/media/system/EFI/BOOT/BOOTX64.EFI";
static const char *const kernel = "/media/system/KERNEL/BROOK.ELF";
static const char *const initrd = "/media/system/INITRD.IMG";
static const char *const key = "/etc/hardware-update-key";

static void check_denials(int dir, int espfd, int keyfd, const char *alias)
{
    char bytes[2];
    struct stat metadata;
    CHECK(open(esp, O_RDONLY) == -1 && errno == EACCES);
    CHECK(open(kernel, O_RDONLY) == -1 && errno == EACCES);
    CHECK(open(initrd, O_RDONLY) == -1 && errno == EACCES);
    CHECK(openat(dir, "EFI/BOOT/BOOTX64.EFI", O_PATH) == -1 && errno == EACCES);
    CHECK(open(key, O_RDONLY) == -1 && errno == EACCES);
    CHECK(open(alias, O_RDONLY) == -1 && errno == EACCES);
    CHECK(open("/eTc/HARDWARE-UPDATE-KEY", O_PATH) == -1 && errno == EACCES);
    CHECK(pread(espfd, bytes, sizeof(bytes), 0) == -1 && errno == EACCES);
    CHECK(pread(keyfd, bytes, sizeof(bytes), 0) == -1 && errno == EACCES);
    CHECK(fstat(keyfd, &metadata) == -1 && errno == EACCES);
    const char *raw[] = {"/dev/ramdisk0", "/dev/ahci-system", "/dev/ahci0",
                         "/dev/brook-guard-esp", "/dev/brook-guard-data"};
    for (unsigned i = 0; i < sizeof(raw) / sizeof(raw[0]); ++i)
        CHECK(open(raw[i], O_RDONLY) == -1 && (errno == ENOENT || errno == EPERM || errno == EACCES));
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 && !strcmp(argv[1], "--disposable-fixture"));
    const int dir = open("/media/system", O_RDONLY | O_DIRECTORY);
    const int espfd = open(esp, O_RDONLY);
    const int keyfd = open(key, O_RDONLY);
    struct stat metadata;
    CHECK(dir >= 0 && espfd >= 0 && keyfd >= 0);
    CHECK(fstat(keyfd, &metadata) == 0 && (metadata.st_mode & 0777) == 0600);
    const int kernelfd = open(kernel, O_RDONLY);
    const int initrdfd = open(initrd, O_RDONLY);
    CHECK(kernelfd >= 0 && initrdfd >= 0);
    CHECK(close(kernelfd) == 0 && close(initrdfd) == 0);
    char alias[32];
    for (unsigned i = 1; i <= 9; ++i) {
        CHECK(snprintf(alias, sizeof(alias), "/ETC/HARDWA~%u", i) > 0);
        struct stat short_name;
        if (stat(alias, &short_name) == 0 && short_name.st_ino == metadata.st_ino) break;
        CHECK(i < 9);
    }
    char magic[2];
    CHECK(read(espfd, magic, sizeof(magic)) == sizeof(magic) && !memcmp(magic, "MZ", 2));
    CHECK(mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, keyfd, 0) == MAP_FAILED);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(setresgid(1000, 1000, 1000) == 0 && setresuid(1000, 1000, 1000) == 0);
        check_denials(dir, espfd, keyfd, alias);
        const pid_t namespaced = syscall(SYS_clone, CLONE_NEWUSER | CLONE_NEWNS | SIGCHLD,
                                         0UL, 0UL, 0UL, 0UL);
        CHECK(namespaced >= 0);
        if (!namespaced) {
            const int map = open("/proc/self/uid_map", O_WRONLY);
            const char user[] = "0 1000 1\n";
            CHECK(map >= 0 && write(map, user, sizeof(user) - 1) == sizeof(user) - 1 && close(map) == 0);
            CHECK(geteuid() == 0);
            check_denials(dir, espfd, keyfd, alias);
            _exit(0);
        }
        int ns_status = 0;
        CHECK(waitpid(namespaced, &ns_status, 0) == namespaced &&
              WIFEXITED(ns_status) && WEXITSTATUS(ns_status) == 0);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    fprintf(stderr, "BOOT_STORAGE_ACCESS_PASS\n");
    return 0;
}
