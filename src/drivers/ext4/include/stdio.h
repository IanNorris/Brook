/* stdio.h shim for lwext4 in Brook kernel freestanding environment.
 * lwext4 only uses printf() and fflush() — both provided by ext4_osal_brook.c */
#pragma once

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

int printf(const char* fmt, ...);
int fflush(void* stream);

/* stdout — used by fflush(stdout) in ext4_debug.h */
#define stdout ((void*)0)

#ifdef __cplusplus
}
#endif
