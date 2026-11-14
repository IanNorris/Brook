#include "desktop_client_cache.h"
#include <stdio.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"desktop_client_cache_test:%d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
    struct DesktopClientHeld h;memset(&h,0,sizeof(h));
    struct BrookDesktopLayer q={0};q.window=7;q.incarnation=3;q.generation=11;q.bytes=4096;q.width=32;q.height=32;q.stride=32;
    CHECK(!desktop_client_can_keep(&h,&q,5));                  /* nothing held */
    desktop_client_hold(&h,&q,5);
    CHECK(desktop_client_can_keep(&h,&q,5));
    CHECK(!desktop_client_can_keep(&h,&q,6));                  /* buffer reallocated */
    struct BrookDesktopLayer other=q;other.window=8;CHECK(!desktop_client_can_keep(&h,&other,5));
    other=q;other.incarnation=4;CHECK(!desktop_client_can_keep(&h,&other,5));
    other=q;other.bytes=8192;CHECK(!desktop_client_can_keep(&h,&other,5));  /* resized */
    /* The request describes the held pixels, not the query's newer generation. */
    struct BrookDesktopLayer wire=q;wire.generation=12;desktop_client_keep_request(&h,&wire);
    CHECK(wire.generation==11 && wire.bytes==4096 && wire.stride==32);
    /* Bands are reused only for the identical tracked region. */
    struct DesktopTrackBands b;memset(&b,0,sizeof(b));b.rows=8;b.count=4;b.hash[0]=99;
    CHECK(!desktop_client_bands_match(&h,32,0,0,32,32));
    desktop_client_store_bands(&h,&b,32,0,0,32,32);
    CHECK(desktop_client_bands_match(&h,32,0,0,32,32) && h.bands.hash[0]==99);
    CHECK(!desktop_client_bands_match(&h,32,0,0,31,32));
    /* A new capture replaces the record (and its bands). */
    q.generation=12;desktop_client_hold(&h,&q,5);CHECK(h.generation==12 && !h.bandsValid);
    /* Saturated generations and zero serials are never held. */
    q.generation=~0ull;desktop_client_hold(&h,&q,5);CHECK(!h.serial);
    q.generation=1;desktop_client_hold(&h,&q,0);CHECK(!h.serial);
    desktop_client_hold(&h,&q,9);desktop_client_forget(&h);CHECK(!desktop_client_can_keep(&h,&q,9));
    puts("desktop_client_cache_test ok");return 0;
}
