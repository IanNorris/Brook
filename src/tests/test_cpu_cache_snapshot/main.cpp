#include "../../kernel/src/cpu_cache_snapshot.h"
#include <cassert>
using namespace brook;
int main(){
    CpuCacheSnapshot s{};s.valid=true;s.pat=0x0007040100070406ULL;
    s.mtrrs.capability=(1ull<<10)|1;s.mtrrs.count=1;s.mtrrs.physicalBits=36;s.mtrrs.defaults=(1ull<<11)|6;
    constexpr uint64_t mmio=0xf7400000;
    s.mtrrs.base[0]=mmio;s.mtrrs.mask[0]=((~((1ull<<21)-1))&((1ull<<36)-1)&~4095ull)|(1ull<<11);
    uint64_t pages[17];for(unsigned i=0;i<17;++i)pages[i]=0x200000+uint64_t(i)*4096;
    assert(CpuCacheHdaPageTypes(s,mmio,pages,17));assert(CpuCacheSnapshotsMatch(s,s));
    auto ap=s;ap.pat^=1ull<<24;assert(!CpuCacheSnapshotsMatch(s,ap) && !CpuCacheHdaPageTypes(ap,mmio,pages,17));
    ap=s;ap.pat^=1;assert(!CpuCacheSnapshotsMatch(s,ap) && !CpuCacheHdaPageTypes(ap,mmio,pages,17));
    ap=s;ap.mtrrs.defaults^=4;assert(!CpuCacheSnapshotsMatch(s,ap));
    ap=s;ap.cacheDisabled=true;assert(!CpuCacheSnapshotsMatch(s,ap) && !CpuCacheHdaPageTypes(ap,mmio,pages,17));
    ap=s;ap.valid=false;assert(!CpuCacheSnapshotsMatch(s,ap));
    pages[3]=mmio;assert(!CpuCacheHdaPageTypes(s,mmio,pages,17));pages[3]=0x203000;
    assert(!CpuCacheHdaPageTypes(s,mmio,pages,16) && !CpuCacheHdaPageTypes(s,mmio+1,pages,17));
    assert(!CpuCacheHdaPageTypes(s,mmio,nullptr,17));
}
