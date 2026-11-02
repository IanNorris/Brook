#pragma once
#include <stdint.h>

namespace brook {
// Static lifetime, one BSP requester and one designated AP responder. The BSP
// must never reuse an abandoned mailbox: a timed-out AP may still publish later.
// The responder checks CPUID before issuing RDTSCP. No scheduler or GS state is
// required. Neither a successful exchange nor a narrow interval proves that
// offsets remain stable after migration, power-state changes or suspend.
struct alignas(64) NativeTscMailbox {
    uint32_t request=0,response=0;uint64_t timestamp=0;uint32_t capable=0;
    bool abandoned=false; // BSP-owned only
};
struct NativeTscInterval {int64_t lower=0,upper=0;uint64_t roundTrip=0;};
enum class NativeTscExchangeResult:uint8_t {Complete,Unavailable,Timeout,Abandoned,Invalid};
inline bool NativeTscDifference(uint64_t x,uint64_t y,int64_t& out){
    constexpr uint64_t Max=uint64_t(~uint64_t(0)>>1);
    if(x>=y){const auto d=x-y;if(d>Max)return false;out=int64_t(d);return true;}
    const auto d=y-x;if(d>Max)return false;out=-int64_t(d);return true;
}
inline bool NativeTscIntervalFrom(uint64_t before,uint64_t ap,uint64_t after,NativeTscInterval& out){
    if(after<before)return false;NativeTscInterval v{};
    if(!NativeTscDifference(ap,after,v.lower) || !NativeTscDifference(ap,before,v.upper))return false;
    v.roundTrip=after-before;out=v;return true;
}
inline bool NativeTscIntersect(NativeTscInterval& a,const NativeTscInterval& b){
    if(a.lower>a.upper || b.lower>b.upper)return false;
    const auto lo=a.lower>b.lower?a.lower:b.lower,hi=a.upper<b.upper?a.upper:b.upper;
    if(lo>hi)return false;a.lower=lo;a.upper=hi;if(b.roundTrip<a.roundTrip)a.roundTrip=b.roundTrip;return true;
}
// Called only by the designated AP. A release response publishes both fields.
template<class Io> bool NativeTscRespond(NativeTscMailbox& box,Io& io){
    const auto request=__atomic_load_n(&box.request,__ATOMIC_ACQUIRE);
    if(!request || request==__atomic_load_n(&box.response,__ATOMIC_RELAXED))return false;
    uint64_t tick=0;const bool capable=io.capable() && io.tsc(tick);
    box.timestamp=tick;box.capable=capable?1:0;
    __atomic_store_n(&box.response,request,__ATOMIC_RELEASE);return true;
}
// BSP io.tsc must be ordered and execute on the same CPU for both reads.
// The response can arrive after timeout; abandon permanently on every timeout.
template<class Io> NativeTscExchangeResult NativeTscExchange(NativeTscMailbox& box,Io& io,uint32_t limit,NativeTscInterval& out){
    if(box.abandoned)return NativeTscExchangeResult::Abandoned;
    if(!limit || limit>100000000)return NativeTscExchangeResult::Invalid;
    const auto last=__atomic_load_n(&box.request,__ATOMIC_RELAXED);
    if(last!=__atomic_load_n(&box.response,__ATOMIC_ACQUIRE) || last==~uint32_t(0)){
        box.abandoned=true;return NativeTscExchangeResult::Abandoned;}
    uint64_t before=0,after=0;if(!io.tsc(before))return NativeTscExchangeResult::Unavailable;
    const uint32_t next=last+1;__atomic_store_n(&box.request,next,__ATOMIC_RELEASE);
    bool ready=false;for(uint32_t i=0;i<limit;++i){
        if(__atomic_load_n(&box.response,__ATOMIC_ACQUIRE)==next){ready=true;break;}io.pause();}
    if(!ready){box.abandoned=true;return NativeTscExchangeResult::Timeout;}
    const uint64_t ap=box.timestamp;const bool capable=box.capable!=0;
    if(!io.tsc(after) || !capable)return NativeTscExchangeResult::Unavailable;
    return NativeTscIntervalFrom(before,ap,after,out)?NativeTscExchangeResult::Complete:NativeTscExchangeResult::Invalid;
}
}
