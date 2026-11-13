#pragma once
#include <errno.h>
#include <stdint.h>

/* Unsupported scenes use software fallback. Limit new GPU preparations while
 * they persist; callers must continue polling outstanding GPU work. */
struct DesktopRetry { uint64_t unsupportedSince; };
/* The synchronous path has no outstanding fence to poll between attempts. */
static inline unsigned desktop_sync_retry_delay_us(int error){
    return error==ENOTSUP || error==EBUSY?50000u:10000u;
}
static inline void desktop_retry_failure(struct DesktopRetry *retry,int error,unsigned active,uint64_t now){
    retry->unsupportedSince=(error==ENOTSUP || (error==EBUSY && !active))?now:0;
}
static inline void desktop_retry_success(struct DesktopRetry *retry){retry->unsupportedSince=0;}
static inline int desktop_retry_waiting(struct DesktopRetry *retry,uint64_t now){
    if(!retry->unsupportedSince)return 0;
    if(!now || now<retry->unsupportedSince || now-retry->unsupportedSince>=50000000ull){
        retry->unsupportedSince=0;return 0;
    }
    return 1;
}
