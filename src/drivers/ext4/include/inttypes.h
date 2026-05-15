/* inttypes.h shim for lwext4 in Brook kernel freestanding environment.
 * Provides PRIu32/PRIx64 etc. format macros used by ext4_debug.h.
 * These match Clang's definitions for x86-64 LP64. */
#pragma once

#include <stdint.h>

#define PRId8    "d"
#define PRId16   "d"
#define PRId32   "d"
#define PRId64   "ld"

#define PRIu8    "u"
#define PRIu16   "u"
#define PRIu32   "u"
#define PRIu64   "lu"

#define PRIx8    "x"
#define PRIx16   "x"
#define PRIx32   "x"
#define PRIx64   "lx"

#define PRIX8    "X"
#define PRIX16   "X"
#define PRIX32   "X"
#define PRIX64   "lX"
