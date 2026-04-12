// memtest.c — Memory subsystem test for Brook OS.
// Validates: malloc/free, memset, mmap (via large malloc), brk.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    printf("Memory allocation test:\n");

    // Test power-of-4 sizes from 16 B to 64 KB.
    for (int size = 16; size <= 65536; size *= 4) {
        void* p = malloc(size);
        if (!p) { printf("  malloc(%d) FAILED\n", size); continue; }
        memset(p, 0xAA, size);
        // Verify first and last byte
        if (((unsigned char*)p)[0] != 0xAA ||
            ((unsigned char*)p)[size - 1] != 0xAA) {
            printf("  malloc(%d) VERIFY FAILED\n", size);
            free(p);
            return 1;
        }
        printf("  malloc(%d) = %p OK\n", size, p);
        free(p);
    }

    // Test large allocation (4 MB — typically served by mmap).
    size_t bigSize = 4 * 1024 * 1024;
    void* big = malloc(bigSize);
    if (big) {
        memset(big, 0x55, bigSize);
        printf("  malloc(4MB) = %p OK\n", big);
        free(big);
    } else {
        printf("  malloc(4MB) FAILED\n");
        return 1;
    }

    printf("All tests passed!\n");
    return 0;
}
