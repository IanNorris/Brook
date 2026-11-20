/* Read-only, boot-pinned prerequisite for the explicitly selected old updater. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/shared/brook_system_esp.h"

static int directory(int parent, const char *name, unsigned long verify)
{
    const int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 && ioctl(fd, verify, 0UL) == 0) return fd;
    if (fd >= 0) close(fd);
    return -1;
}

int main(void)
{
    struct stat st;
    if (geteuid() != 0) return 1;
    const int esp = directory(AT_FDCWD, "/media/system", BROOK_IOCTL_VERIFY_SYSTEM_ESP_ROOT);
    if (esp < 0) return 1;
    const int recovery = directory(esp, "RECOVERY", BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER);
    const int updates = directory(esp, "UPDATES", BROOK_IOCTL_VERIFY_SYSTEM_ESP_MEMBER);
    if (recovery < 0 || updates < 0) return 2;

    // The listening updater created this O_EXCL pathname. Supported guard
    // provisioners must acquire it before opening or creating BOOTSTAT.BIN.
    const int lock = open("/tmp/hardware-update.lock", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (lock < 0 || fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        st.st_nlink != 1 || st.st_size != 0 || (st.st_mode & 0777) != 0600) return 3;

    if (fstatat(recovery, "BOOTSTAT.BIN", &st, AT_SYMLINK_NOFOLLOW) == 0) return 4;
    if (errno != ENOENT) return 5;
    return puts("BROOK_SLOT_BOOTSTRAP_JOURNAL_ABSENT") < 0 ? 6 : 0;
}
