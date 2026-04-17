// SDL_endian.h — Minimal shim for Brook OS (no SDL dependency)
// Provides endian conversion macros used by DOOM's i_swap.h
// x86-64 is always little-endian.

#ifndef _SDL_endian_h
#define _SDL_endian_h

#define SDL_LIL_ENDIAN  1234
#define SDL_BIG_ENDIAN  4321
#define SDL_BYTEORDER   SDL_LIL_ENDIAN

#define SDL_SwapLE16(x) (x)
#define SDL_SwapLE32(x) (x)

// Aliases used in i_swap.h
#ifndef SYS_LIL_ENDIAN
#define SYS_LIL_ENDIAN SDL_LIL_ENDIAN
#endif
#ifndef SYS_BIG_ENDIAN
#define SYS_BIG_ENDIAN SDL_BIG_ENDIAN
#endif

#endif
