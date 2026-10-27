#ifndef BROOK_TOPLEVEL_SIZE_H
#define BROOK_TOPLEVEL_SIZE_H
#include <stdint.h>
struct toplevel_size_hints { int32_t min_w,min_h,max_w,max_h; };
static int toplevel_size_valid(const struct toplevel_size_hints *h) {
    return h->min_w>=0 && h->min_h>=0 && h->max_w>=0 && h->max_h>=0 &&
        (!h->max_w || h->max_w>=h->min_w) && (!h->max_h || h->max_h>=h->min_h);
}
static int32_t toplevel_initial_axis(int32_t fallback,int32_t minimum,int32_t maximum,int32_t cap) {
    if(cap<1)cap=1;
    if(fallback>cap)fallback=cap;
    if(minimum>cap)minimum=cap;
    if(maximum>cap)maximum=cap;
    if(fallback<minimum)fallback=minimum;
    if(maximum && fallback>maximum)fallback=maximum;
    return fallback>0?fallback:1;
}
#endif
