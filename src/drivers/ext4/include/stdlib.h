/* stdlib.h shim for lwext4 in Brook kernel freestanding environment.
 * With CONFIG_USE_USER_MALLOC=1, lwext4 uses ext4_user_{malloc,calloc,realloc,free}
 * instead of these — but the header is still included by several source files. */
#pragma once

#ifndef __SIZE_TYPE__
typedef unsigned long size_t;
#else
typedef __SIZE_TYPE__ size_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline int abs(int x) { return x < 0 ? -x : x; }
static inline long labs(long x) { return x < 0 ? -x : x; }

/* These won't be called when CONFIG_USE_USER_MALLOC=1, but satisfy the declarations. */
void* malloc(size_t size);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
void  free(void* ptr);

/* qsort — simple shell sort (no recursion, no malloc, O(n^1.5) average).
 * lwext4 uses this for directory index entry sorting. */
static inline void qsort(void* base, size_t nmemb, size_t size,
                          int (*compar)(const void*, const void*))
{
    unsigned char* b = (unsigned char*)base;
    /* Swap buffer on stack — fine for small elements (ext4 dir entries). */
    unsigned char tmp[256];
    if (size > sizeof(tmp)) return;  /* safety: refuse huge elements */

    for (size_t gap = nmemb / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < nmemb; ++i) {
            for (size_t j = i; j >= gap; j -= gap) {
                unsigned char* a_ptr = b + (j - gap) * size;
                unsigned char* b_ptr = b + j * size;
                if (compar(a_ptr, b_ptr) <= 0) break;
                /* swap */
                for (size_t k = 0; k < size; ++k) {
                    tmp[k] = a_ptr[k];
                    a_ptr[k] = b_ptr[k];
                    b_ptr[k] = tmp[k];
                }
            }
        }
    }
}

#ifdef __cplusplus
}
#endif
