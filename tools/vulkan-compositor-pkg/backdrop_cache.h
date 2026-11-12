#pragma once
#include <stdint.h>

struct BackdropIdentity {uint64_t display,generation,bytes;uint32_t width,height;};

/* Identity zero denotes an uncommitted or failed capture.  The caller must
 * invalidate before replacing its buffer and commit only after capture and
 * registration both succeed. */
static inline int backdrop_cache_matches(struct BackdropIdentity cached,struct BackdropIdentity current,
                                         uint64_t capacity,int has_buffer){
    return has_buffer && cached.display && cached.generation && cached.bytes && cached.width && cached.height &&
        cached.display==current.display && cached.generation==current.generation && cached.bytes==current.bytes &&
        cached.width==current.width && cached.height==current.height && capacity>=current.bytes;
}
