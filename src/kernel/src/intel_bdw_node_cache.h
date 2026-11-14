#pragma once
#include <stdint.h>
namespace brook {
// BdwIntelUserNodeEnabled() gates every Intel GPU ioctl and re-read about
// twenty PCI config registers (identity, BARs, GGC, command, PM D0) each
// time, which showed up in yquake's kernel samples. The integrated GPU at
// 00:02.0 cannot be hot-removed and Brook never moves it out of D0 or
// reprograms its BARs, so a passing check is reused for a short window.
//
// What invalidates it: the window expiring (BdwNodeCacheTtlMs), any failing
// check (never cached), and `gpu node cache off`. The safety-critical live
// checks are unaffected: the submission worker still verifies the device
// identity and D0 state around every job (ExecuteRetainedWorker) and panics
// on a mismatch; this cache only spares the ioctl admission gate.
inline constexpr uint64_t BdwNodeCacheTtlMs=100;
struct BdwNodeCache { uint64_t okTick; bool valid; bool disabled; uint64_t hits,checks; };
template<class Check>
inline bool BdwNodeCachedCheck(BdwNodeCache& c,uint64_t nowMs,Check check){
    const bool disabled=__atomic_load_n(&c.disabled,__ATOMIC_RELAXED);
    if(!disabled && __atomic_load_n(&c.valid,__ATOMIC_ACQUIRE)){
        const uint64_t ok=__atomic_load_n(&c.okTick,__ATOMIC_RELAXED);
        if(nowMs>=ok && nowMs-ok<BdwNodeCacheTtlMs){__atomic_add_fetch(&c.hits,1,__ATOMIC_RELAXED);return true;}
    }
    __atomic_add_fetch(&c.checks,1,__ATOMIC_RELAXED);
    __atomic_store_n(&c.valid,false,__ATOMIC_RELEASE);
    const bool ok=check();
    if(ok && !disabled){__atomic_store_n(&c.okTick,nowMs,__ATOMIC_RELAXED);__atomic_store_n(&c.valid,true,__ATOMIC_RELEASE);}
    return ok;
}
inline void BdwNodeCacheSetEnabled(BdwNodeCache& c,bool on){
    __atomic_store_n(&c.disabled,!on,__ATOMIC_RELAXED);__atomic_store_n(&c.valid,false,__ATOMIC_RELEASE);
}
BdwNodeCache& BdwIntelNodeCache();
}
