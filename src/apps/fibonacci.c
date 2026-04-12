// fibonacci.c — Fibonacci benchmark for Brook OS.
// Validates: printf, clock_gettime, volatile, arithmetic.

#include <stdio.h>
#include <time.h>

static long long fib(int n) {
    if (n <= 1) return n;
    long long a = 0, b = 1;
    for (int i = 2; i <= n; i++) {
        long long c = a + b;
        a = b;
        b = c;
    }
    return b;
}

int main(void) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i <= 50; i += 5)
        printf("fib(%2d) = %lld\n", i, fib(i));

    // Benchmark: compute fib(90) one million times.
    volatile long long result = 0;
    for (int j = 0; j < 1000000; j++)
        result = fib(90);

    clock_gettime(CLOCK_MONOTONIC, &end);
    long elapsed = (end.tv_sec - start.tv_sec) * 1000
                 + (end.tv_nsec - start.tv_nsec) / 1000000;
    printf("Benchmark: fib(90) x 1M = %lld in %ld ms\n",
           (long long)result, elapsed);
    return 0;
}
