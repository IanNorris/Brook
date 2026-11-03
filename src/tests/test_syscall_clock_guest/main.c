// Long enough for a host to attach an incarnation-bound syscall watch.
#include <stdio.h>
#include <time.h>

int main(void)
{
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
    puts("SYSCALL_CLOCK_READY");
    fflush(stdout);
    for (int i = 0; i < 100000; ++i)
        if (nanosleep(&delay, 0)) return 1;
    puts("SYSCALL_CLOCK_DONE");
    return 0;
}
