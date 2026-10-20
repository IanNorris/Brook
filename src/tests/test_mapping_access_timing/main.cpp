#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>
#include "memory/mapping_access_timing.h"
namespace {int failures=0;std::atomic<uint64_t> clockCalls{0};}
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"mapping timing line %d: %s\n",__LINE__,#x);++failures;}}while(0)
namespace brook {
bool KvmClockReadStableNs(uint64_t* result){*result=clockCalls.fetch_add(1)+1;return true;}
}
int main(){
    using namespace brook;MappingAccessTimingModel m;
    CHECK(!(m.Token()&1));CHECK(!m.Observe(0,false,1,2,3,1,5));
    m.Enable(true);auto token=m.Token();CHECK(token&1);
    CHECK(m.Observe(token,false,1,101,601,17,90));
    auto s=m.Snapshot();CHECK(s.read.samples==1 && s.write.samples==0);
    CHECK(s.read.waitNs==100 && s.read.holdNs==500 && s.read.waitBins[0]==1 && s.read.holdBins[1]==1);
    CHECK(s.read.maxWaitPid==17 && s.read.maxHoldIncarnation==90);
    CHECK(!m.Observe(token,false,0,1,2,1,1));CHECK(!m.Observe(token,false,5,4,6,1,1));
    CHECK(!m.Observe(token,false,5,6,4,1,1));CHECK(m.Snapshot().read.invalidClock==3);
    m.Reset();CHECK(m.Token()!=token && (m.Token()&1));
    CHECK(!m.Observe(token,false,1,2,3,1,1));CHECK(m.Snapshot().read.samples==0);
    token=m.Token();m.Enable(false);CHECK(!m.Observe(token,false,1,2,3,1,1));
    m.Enable(true);token=m.Token();
    CHECK(m.Observe(token,true,1,~uint64_t(0),~uint64_t(0),9,100));
    CHECK(m.Observe(token,true,1,100,100,10,101));s=m.Snapshot();
    CHECK(s.write.samples==2 && s.write.saturated && s.write.waitNs==~uint64_t(0));
    CHECK(s.write.waitBins[MappingAccessTimingBins-1]==1 && s.write.maxWaitIncarnation==100);
    // Boundary buckets are inclusive; overflow uses the last bucket.
    m.Reset();token=m.Token();
    for(unsigned i=0;i+1<MappingAccessTimingBins;++i)
        CHECK(m.Observe(token,false,1,1+MappingAccessTimingBounds[i],1+2*MappingAccessTimingBounds[i],1,1));
    s=m.Snapshot();for(unsigned i=0;i+1<MappingAccessTimingBins;++i)CHECK(s.read.waitBins[i]==1 && s.read.holdBins[i]==1);
    // Exercise the actual locked wrapper with simultaneous records and coherent
    // snapshots. Reset/off cancels already started intervals, rather than
    // attributing them to the new epoch.
    CHECK(MappingAccessTimingToken()==0 && clockCalls==0);
    MappingAccessTimingEnable(true);MappingAccessTimingReset();token=MappingAccessTimingToken();
    std::vector<std::thread> workers;
    for(unsigned p=1;p<=4;++p)workers.emplace_back([=]{
        for(unsigned i=0;i<1000;++i){
            const auto a=MappingAccessTimingClock(),b=MappingAccessTimingClock(),c=MappingAccessTimingClock();
            MappingAccessTimingRecord(token,p&1,a,b,c,p,p+100);
        }
    });
    for(auto& worker:workers)worker.join();s=MappingAccessTimingRead();
    CHECK(s.read.samples==2000 && s.write.samples==2000 && !s.read.invalidClock && !s.write.invalidClock);
    for(const auto* counts:{&s.read,&s.write}){
        uint64_t wait=0,hold=0;for(unsigned i=0;i<MappingAccessTimingBins;++i){wait+=counts->waitBins[i];hold+=counts->holdBins[i];}
        CHECK(wait==counts->samples && hold==counts->samples);
    }
    MappingAccessTimingReset();MappingAccessTimingRecord(token,false,1,2,3,1,1);
    CHECK(MappingAccessTimingRead().read.samples==0);
    MappingAccessTimingEnable(false);CHECK(!(MappingAccessTimingToken()&1));
    std::printf("MAPPING_ACCESS_TIMING %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
