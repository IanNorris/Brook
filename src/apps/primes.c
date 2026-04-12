/* primes.c — Sieve of Eratosthenes benchmark for Brook OS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LIMIT 1000000

int main(void) {
    struct timespec t0, t1;
    printf("Sieve of Eratosthenes up to %d\n", LIMIT);

    unsigned char *sieve = (unsigned char *)malloc(LIMIT + 1);
    if (!sieve) {
        printf("malloc failed!\n");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);

    memset(sieve, 1, LIMIT + 1);
    sieve[0] = sieve[1] = 0;

    for (long i = 2; i * i <= LIMIT; i++) {
        if (sieve[i]) {
            for (long j = i * i; j <= LIMIT; j += i)
                sieve[j] = 0;
        }
    }

    int count = 0;
    long last = 0;
    for (long i = 2; i <= LIMIT; i++) {
        if (sieve[i]) {
            count++;
            last = i;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;

    printf("Found %d primes (largest: %ld)\n", count, last);
    printf("Sieve completed in %ld ms\n", ms);

    /* Print last 20 primes */
    printf("Last 20 primes:");
    int printed = 0;
    for (long i = LIMIT; i >= 2 && printed < 20; i--) {
        if (sieve[i]) {
            printf(" %ld", i);
            printed++;
        }
    }
    printf("\n");

    free(sieve);
    return 0;
}
