#pragma once
#include <stdint.h>
#include "desktop_layers_abi.h"

/* Direct and CPU snapshots carry the provider's non-reused surface commit.
 * The CPU tag is bound by the OS to its exact publication generation. */
struct AsyncWindowSource {uint64_t incarnation,generation,commit,snapshotCommit;uint32_t window,omitted;};
struct AsyncSourceBarrier {uint64_t incarnation,commit;};
/* Map each window to its content descriptor after tile culling. Validate the
 * entire map before marking anything, so malformed metadata fails closed. */
static inline int async_sources_mark_omitted(struct AsyncWindowSource *sources,unsigned count,
                                             const unsigned *indices,const uint8_t *used,unsigned usedCount){
    if(!sources || !indices || !used || count>BROOK_DESKTOP_LAYER_LIMIT)return 0;
    for(unsigned i=0;i<count;++i)if(indices[i]>=usedCount)return 0;
    for(unsigned i=0;i<count;++i)sources[i].omitted=!used[indices[i]];
    return 1;
}
static inline int async_sources_presentable(const struct AsyncSourceBarrier *barriers,
                                            const struct AsyncWindowSource *sources,unsigned count){
    if(!barriers || !sources || count>BROOK_DESKTOP_LAYER_LIMIT)return 0;
    for(unsigned i=0;i<count;++i){const struct AsyncWindowSource *s=&sources[i];
        if(!s->window || s->window>BROOK_DESKTOP_LAYER_LIMIT || !s->incarnation)return 0;
        if(s->omitted)continue;
        const struct AsyncSourceBarrier *b=&barriers[s->window-1];
        if(b->incarnation==s->incarnation && (s->commit?s->commit:s->snapshotCommit)<b->commit)return 0;
    }
    return 1;
}
static inline void async_sources_committed(struct AsyncSourceBarrier *barriers,
                                           const struct AsyncWindowSource *sources,unsigned count){
    if(!barriers || !sources || count>BROOK_DESKTOP_LAYER_LIMIT)return;
    for(unsigned i=0;i<count;++i){const struct AsyncWindowSource *s=&sources[i];
        const uint64_t presented=s->commit?s->commit:s->snapshotCommit;
        if(s->omitted || !presented || !s->window || s->window>BROOK_DESKTOP_LAYER_LIMIT || !s->incarnation)continue;
        struct AsyncSourceBarrier *b=&barriers[s->window-1];
        if(b->incarnation!=s->incarnation || b->commit<presented)
            *b=(struct AsyncSourceBarrier){s->incarnation,presented};
    }
}
