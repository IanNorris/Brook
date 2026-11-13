#include "backdrop_cache.h"
#include <assert.h>

int main(void){
    struct BackdropIdentity valid={9,27,3200u*1800u*4u,3200,1800};
    struct BackdropIdentity changed=valid;
    assert(backdrop_cache_matches(valid,valid,valid.bytes,1));
    assert(!backdrop_cache_matches(valid,valid,valid.bytes-1,1));
    assert(!backdrop_cache_matches(valid,valid,valid.bytes,0));
    changed.display++;assert(!backdrop_cache_matches(valid,changed,valid.bytes,1));changed=valid;
    changed.generation++;assert(!backdrop_cache_matches(valid,changed,valid.bytes,1));changed=valid;
    changed.bytes--;assert(!backdrop_cache_matches(valid,changed,valid.bytes,1));changed=valid;
    changed.width--;assert(!backdrop_cache_matches(valid,changed,valid.bytes,1));changed=valid;
    changed.height--;assert(!backdrop_cache_matches(valid,changed,valid.bytes,1));
    struct BackdropIdentity empty={0};assert(!backdrop_cache_matches(empty,empty,0,1));
    valid.generation=0;assert(!backdrop_cache_matches(valid,valid,valid.bytes,1));
}
