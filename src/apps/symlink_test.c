/*
 * symlink_test.c — exercise sys_symlink on ext2 and report errno.
 *
 * Tries four cases that together cover nar-unpack's usage:
 *   1. Short symlink in root (/data/symtest/link1 -> "target")
 *   2. Long symlink in root (target > 60 bytes: slow symlink path)
 *   3. Symlink in a subdirectory we mkdir'd
 *   4. Symlink created after many files in the same directory (dir-block growth)
 *
 * When something fails the kernel's SerialPrintf tells us which stage
 * (AllocInode / AllocBlock / DirAdd / ResolveParent / etc.) bailed out.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int check(const char* what, int rc) {
    if (rc == 0) { printf("  OK: %s\n", what); return 0; }
    printf("  FAIL: %s: %s (errno=%d)\n", what, strerror(errno), errno);
    return 1;
}

int main(void) {
    int fails = 0;
    printf("=== symlink_test ===\n");

    mkdir("/data/symtest", 0755);  // ensure parent exists

    unlink("/data/symtest/link1");
    unlink("/data/symtest/link2");

    printf("[1] short symlink in /data/symtest\n");
    fails += check("symlink(\"target\", \"/data/symtest/link1\")",
                   symlink("target", "/data/symtest/link1"));

    printf("[2] long symlink (>60b) in /data/symtest\n");
    const char* longTarget =
        "/nix/store/abcdefghijklmnopqrstuvwxyz01234567-some-very-long-path/lib/libfoo.so.42";
    fails += check("long symlink", symlink(longTarget, "/data/symtest/link2"));

    printf("[3] mkdir + symlink inside\n");
    mkdir("/data/symtest/symtest_dir", 0755);
    unlink("/data/symtest/symtest_dir/inner");
    fails += check("mkdir /data/symtest/symtest_dir",
                   mkdir("/data/symtest/symtest_dir", 0755) == 0 || errno == EEXIST ? 0 : -1);
    fails += check("symlink in subdir",
                   symlink("target", "/data/symtest/symtest_dir/inner"));

    printf("[4] many siblings then one more symlink (forces dir block growth)\n");
    mkdir("/data/symtest/symtest_many", 0755);
    for (int i = 0; i < 200; i++) {
        char p[96];
        snprintf(p, sizeof(p), "/data/symtest/symtest_many/s%03d", i);
        unlink(p);
        if (symlink("x", p) != 0) {
            printf("  FAIL at i=%d: %s (errno=%d)\n", i, strerror(errno), errno);
            fails++;
            break;
        }
    }
    if (fails == 0) printf("  OK: 200 symlinks\n");

    if (fails == 0) printf("=== PASS ===\n");
    else            printf("=== FAIL (%d) ===\n", fails);
    return fails ? 1 : 0;
}
