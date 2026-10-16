#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Leave links for offline fsck and reboot readback. Use a fresh directory. */
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (mkdir(argv[1], 0755) && errno != EEXIST) return 3;
    const unsigned lengths[] = {1, 58, 59, 60, 61, 254};
    unsigned failures = 0;
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        unsigned length = lengths[i];
        char target[256], path[512], got[256];
        memset(target, 'a' + i, length); target[length] = 0;
        snprintf(path, sizeof(path), "%s/link-%u", argv[1], length);
        int created = symlink(target, path);
        if (created && errno != EEXIST) { ++failures; continue; }
        struct stat info;
        memset(got, 0x5a, sizeof(got));
        ssize_t bytes = readlink(path, got, sizeof(got));
        int ok = !lstat(path, &info) && S_ISLNK(info.st_mode) &&
            info.st_size == length && bytes == length &&
            !memcmp(got, target, length) && got[length] == 0x5a;
        printf("SYMLINKCHECK %s target_bytes=%u\n", ok ? "PASS" : "FAIL", length);
        failures += !ok;
    }
    sync();
    printf("SYMLINKCHECK %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
