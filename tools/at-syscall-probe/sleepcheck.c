#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

int main(void)
{
    int failures = 0;
    const long durations[] = {0, 1, 10000, 999999, 1000001, 5000000};
    for (unsigned i = 0; i < sizeof(durations)/sizeof(durations[0]); ++i) {
        struct timespec req = {0, durations[i]};
        uint64_t start = now_ns();
        int result = syscall(SYS_nanosleep, &req, NULL);
        uint64_t elapsed = now_ns() - start;
        int ok = result == 0 && elapsed >= (uint64_t)durations[i];
        printf("SLEEPCHECK %s requested=%ldns elapsed=%lluns\n",
               ok ? "PASS" : "FAIL", durations[i], (unsigned long long)elapsed);
        failures += !ok;
    }
    const struct timespec invalid[] = {{-1, 0}, {0, -1}, {0, 1000000000}};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        errno = 0;
        int result = syscall(SYS_nanosleep, &invalid[i], NULL);
        int ok = result == -1 && errno == EINVAL;
        printf("SLEEPCHECK %s invalid timespec %u\n", ok ? "PASS" : "FAIL", i);
        failures += !ok;
    }
    printf("SLEEPCHECK %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
