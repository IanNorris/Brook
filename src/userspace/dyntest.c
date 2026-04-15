/* dyntest.c — Test dynamic linking on Brook OS.
 * Build with musl dynamic linking:
 *   musl-gcc -o dyntest dyntest.c
 * (no -static flag)
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void) {
    printf("=== Brook dynamic linking test ===\n");
    printf("  [OK] printf works (libc loaded)\n");

    /* Test that malloc works (musl's internal allocator via mmap) */
    char *p = malloc(1024);
    if (p) {
        p[0] = 'A';
        p[1023] = 'Z';
        printf("  [OK] malloc works\n");
        free(p);
    } else {
        printf("  [FAIL] malloc returned NULL\n");
        return 1;
    }

    /* Test time (uses clock_gettime under the hood) */
    time_t now = time(NULL);
    if (now > 1577836800) {
        printf("  [OK] time() = %ld (after 2020)\n", (long)now);
    } else {
        printf("  [FAIL] time() = %ld (too small)\n", (long)now);
        return 1;
    }

    printf("=== 0 test(s) failed ===\n");
    return 0;
}
