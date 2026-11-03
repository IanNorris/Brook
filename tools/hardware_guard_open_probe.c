// Run only on a disposable recovery fixture with no active checker session.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    puts("brook-ssd-recovery-readonly-v1");
    static const char path[] = "/dev/brook-guard-esp";
    const int modes[] = { O_RDONLY, O_RDONLY | O_CLOEXEC, O_WRONLY,
                          O_RDWR, O_RDONLY | O_CREAT, O_RDONLY | O_TRUNC };
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        errno = 0;
        int fd = open(path, modes[i], 0600);
        if (fd >= 0) {
            close(fd);
            printf("GUARD_OPEN_PROBE_FAILED flags=%x opened\n", modes[i]);
            return 1;
        }
        const int expected = (modes[i] & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) ? EROFS : EACCES;
        if (errno != expected) {
            printf("GUARD_OPEN_PROBE_FAILED flags=%x errno=%d expected=%d\n", modes[i], errno, expected);
            return 1;
        }
    }
    puts("GUARD_OPEN_PROBE_PASS");
    return 0;
}
