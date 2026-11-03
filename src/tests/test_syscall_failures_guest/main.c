// Repeated, predictable failures for a host-attached diagnostic watch.
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
    puts("SYSCALL_FAILURES_READY");
    fflush(stdout);
    for (int i = 0; i < 100000; ++i) {
        if (close(-1) != -1 || errno != EBADF) return 1;
        if (nanosleep(&delay, 0)) return 2;
    }
    return 0;
}
