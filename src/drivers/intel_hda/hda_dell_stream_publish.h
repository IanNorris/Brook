#pragma once
#include <stdint.h>

namespace brook {
// Single-owner publication primitive for the future Dell streaming backend.
// Absolute positions are byte counts in one DMA epoch; ringBase identifies
// its page- and cache-line-aligned origin. sample() must reject stale clocks,
// errors and ambiguous wraps, and return an observed absolute DMA position.
// guard must include the hardware's proven prefetch allowance and scheduling
// margin. It is NOT yet established for the physical Dell. The caller holds
// its stream lock, owns the logical queue interval and all touched lines, and
// retains a zeroed silent trailer. Non-null samples must point to bytes stable
// and readable for the operation, and not overlap a memcpy destination.
// The post-check can detect an overrun, but cannot undo fetched samples: any
// Uncertain result requires immediate mute/verified stop and quarantine.
enum class HdaStreamPublishResult : uint8_t { Published, Invalid, Unsafe, Uncertain };

inline bool HdaStreamSafeWindow(uint64_t cursor,uint64_t base,uint64_t begin,
                                uint64_t end,uint32_t ring,uint32_t line,uint32_t guard) {
    if(!ring || (ring&4095) || line<16 || line>4096 || (line&(line-1)) ||
       (4096%line) || (base&(line-1)) || guard<line || guard>=ring/2 ||
       cursor<base || begin<cursor || end<=begin || end-begin>ring ||
       cursor>~uint64_t(0)-ring || end>~uint64_t(0)-(line-1))return false;
    const uint64_t first=begin&~uint64_t(line-1);
    const uint64_t last=(end+line-1)&~uint64_t(line-1);
    return first>=cursor+guard && last<=cursor+ring-guard;
}

// write(offset,nullptr,n) writes silence; publish flushes every intersecting
// cache line; fence orders all prior stores/flushes before the second sample.
// The backing allocation is already zeroed and flushed before RUN. No logical
// queue receipt may be committed until Published is returned.
template<class Io>
HdaStreamPublishResult HdaStreamPublish(Io& io,uint64_t base,uint64_t begin,
                                        const void* samples,uint32_t bytes,
                                        uint32_t ring,uint32_t line,uint32_t guard) {
    if(!bytes || (bytes&3) || (begin&3) || begin>~uint64_t(0)-bytes || begin<base ||
       !ring || (ring&4095) || line<16 || line>4096 || (line&(line-1)) ||
       (4096%line) || (base&(line-1)) || guard<line || guard>=ring/2 || bytes>ring)
        return HdaStreamPublishResult::Invalid;
    uint64_t before=0;if(!io.sample(before))return HdaStreamPublishResult::Uncertain;
    const uint64_t end=begin+bytes;
    if(!HdaStreamSafeWindow(before,base,begin,end,ring,line,guard))return HdaStreamPublishResult::Unsafe;
    const uint32_t offset=uint32_t((begin-base)%ring);
    uint32_t first=ring-offset;if(first>bytes)first=bytes;
    const auto* data=static_cast<const uint8_t*>(samples);
    if(!io.write(offset,data,first) || !io.publish(offset,first))return HdaStreamPublishResult::Uncertain;
    if(bytes>first && (!io.write(0,data?data+first:nullptr,bytes-first) || !io.publish(0,bytes-first)))
        return HdaStreamPublishResult::Uncertain;
    if(!io.fence())return HdaStreamPublishResult::Uncertain;
    uint64_t after=0;if(!io.sample(after) || after<before ||
       !HdaStreamSafeWindow(after,base,begin,end,ring,line,guard))return HdaStreamPublishResult::Uncertain;
    return HdaStreamPublishResult::Published;
}
}
