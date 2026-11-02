/* Create and remove more than the old per-mount inode ceiling under /tmp. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/statvfs.h>
#include <unistd.h>

int main(void) {
    char dir[] = "/tmp/brook-inodes-XXXXXX", path[128];
    struct statvfs before, after;
    unsigned made = 0;
    int error = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (statvfs("/tmp", &before) || !mkdtemp(dir)) { perror("setup"); return 1; }
    for (; made < 1500; ++made) {
        snprintf(path, sizeof(path), "%s/f%u", dir, made);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) { error = errno; break; }
        if (close(fd)) { error = errno; ++made; break; }
    }
    printf("TMPFS_PROBE_CREATED count=%u error=%d files=%lu free_before=%lu\n",
           made, error, (unsigned long)before.f_files, (unsigned long)before.f_ffree);
    unsigned cleanupErrors = 0;
    for (unsigned i = 0; i < made; ++i) {
        snprintf(path, sizeof(path), "%s/f%u", dir, i);
        if (unlink(path)) ++cleanupErrors;
    }
    if (rmdir(dir)) ++cleanupErrors;
    if (statvfs("/tmp", &after)) { perror("statvfs"); return 1; }
    printf("TMPFS_PROBE_CLEANUP errors=%u free_after=%lu\n", cleanupErrors,
           (unsigned long)after.f_ffree);
    if (error || made != 1500 || cleanupErrors || before.f_files < 4096) return 1;
    puts("TMPFS_PROBE_OK");
    return 0;
}
