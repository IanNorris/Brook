#pragma once
#include <stdint.h>

/* Retain headroom through transient changes. After 60 consecutive uses at
 * one quarter of capacity or less, recreate at the current required size.
 * The caller must ensure no previous GPU operation can still use the buffer. */
static inline int desktop_source_should_resize(uint64_t capacity,uint64_t required,unsigned *smallUses){
    if(!smallUses || !required)return 0;
    if(required>UINT64_MAX-4095){*smallUses=0;return 1;}
    required=(required+4095)&~UINT64_C(4095);
    if(!capacity || required>capacity){*smallUses=0;return 1;}
    if(required>capacity/4){*smallUses=0;return 0;}
    if(*smallUses<60)++*smallUses;
    if(*smallUses==60){*smallUses=0;return 1;}
    return 0;
}
