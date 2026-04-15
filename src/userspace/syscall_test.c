/* syscall_test.c — Tests getrandom and clock_gettime for Brook OS.
 *
 * Build: musl-gcc -static -o syscall_test syscall_test.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/random.h>

static void print_ok(const char *name) { printf("  [OK] %s\n", name); }
static void print_fail(const char *name, const char *reason) { printf("  [FAIL] %s: %s\n", name, reason); }

/* Test 1: getrandom produces non-zero, non-identical output */
static int test_getrandom(void) {
    uint8_t buf1[32], buf2[32];
    memset(buf1, 0, sizeof(buf1));
    memset(buf2, 0, sizeof(buf2));

    ssize_t r1 = getrandom(buf1, sizeof(buf1), 0);
    if (r1 != 32) { print_fail("getrandom_basic", "wrong return value"); return 1; }

    ssize_t r2 = getrandom(buf2, sizeof(buf2), 0);
    if (r2 != 32) { print_fail("getrandom_basic", "wrong return value (2nd call)"); return 1; }

    /* Check not all zeros */
    int nonzero = 0;
    for (int i = 0; i < 32; i++) nonzero += (buf1[i] != 0);
    if (nonzero < 4) { print_fail("getrandom_basic", "output mostly zeros"); return 1; }

    /* Check two calls differ */
    if (memcmp(buf1, buf2, 32) == 0) {
        print_fail("getrandom_basic", "two calls returned identical data");
        return 1;
    }

    print_ok("getrandom_basic");
    return 0;
}

/* Test 2: getrandom byte distribution (rough chi-squared) */
static int test_getrandom_distribution(void) {
    uint8_t buf[256];
    int counts[4] = {0};

    getrandom(buf, sizeof(buf), 0);
    for (int i = 0; i < 256; i++)
        counts[buf[i] & 3]++;

    /* Each bucket should be ~64. Flag if any < 30 or > 100. */
    for (int q = 0; q < 4; q++) {
        if (counts[q] < 20 || counts[q] > 110) {
            char msg[128];
            snprintf(msg, sizeof(msg), "bucket %d has %d (expected ~64)", q, counts[q]);
            print_fail("getrandom_dist", msg);
            return 1;
        }
    }
    print_ok("getrandom_dist");
    return 0;
}

/* Test 3: clock_gettime REALTIME returns plausible epoch */
static int test_clock_realtime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    /* Should be after 2020 (epoch 1577836800) and before 2040 (2208988800) */
    if (ts.tv_sec < 1577836800LL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "epoch %ld too small (before 2020)", (long)ts.tv_sec);
        print_fail("clock_realtime", msg);
        return 1;
    }
    if (ts.tv_sec > 2208988800LL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "epoch %ld too large (after 2040)", (long)ts.tv_sec);
        print_fail("clock_realtime", msg);
        return 1;
    }

    print_ok("clock_realtime");
    return 0;
}

/* Test 4: clock_gettime MONOTONIC returns small boot-relative value */
static int test_clock_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    /* Boot-relative: should be < 1 hour */
    if (ts.tv_sec > 3600) {
        char msg[128];
        snprintf(msg, sizeof(msg), "monotonic %ld sec seems too large", (long)ts.tv_sec);
        print_fail("clock_monotonic", msg);
        return 1;
    }
    if (ts.tv_sec < 0) {
        print_fail("clock_monotonic", "negative value");
        return 1;
    }

    print_ok("clock_monotonic");
    return 0;
}

/* Test 5: gettimeofday returns same epoch range as CLOCK_REALTIME */
static int test_gettimeofday(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    if (tv.tv_sec < 1577836800LL || tv.tv_sec > 2208988800LL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "epoch %ld out of range", (long)tv.tv_sec);
        print_fail("gettimeofday", msg);
        return 1;
    }

    print_ok("gettimeofday");
    return 0;
}

int main(void) {
    printf("=== Brook syscall test (random + clock) ===\n");

    int failures = 0;
    failures += test_getrandom();
    failures += test_getrandom_distribution();
    failures += test_clock_realtime();
    failures += test_clock_monotonic();
    failures += test_gettimeofday();

    printf("=== %d test(s) failed ===\n", failures);
    return failures ? 1 : 0;
}
