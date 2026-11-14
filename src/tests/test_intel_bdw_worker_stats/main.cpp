#include <cstdio>
#include "../../kernel/src/intel_bdw_worker_stats.h"
using namespace brook;
static unsigned failures;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);++failures;}}while(0)
int main() {
    const unsigned expected[]={0,1,2,3,3,4,4,4,4,5,5};
    for(unsigned i=0;i<sizeof(expected)/sizeof(expected[0]);++i)CHECK(BdwWorkerYieldBucket(i)==expected[i]);
    CHECK(BdwWorkerYieldBucket(~uint64_t{0})==5);
    CHECK(BdwRpstatMhz(10u<<7)==500 && BdwRpstatMhz(0xffffffffu)==127*50 && BdwRpstatMhz(0x7f)==0);
    CHECK(BdwWorkerTicksToUs(2000,2000)==1000 && BdwWorkerTicksToUs(5,0)==0);
    CHECK(BdwWorkerTicksToUs(~uint64_t{0},1000)==~uint64_t{0}/1000*1000);

    static BdwWorkerStatsModel m;BdwWorkerStats s{};
    CHECK(m.Read(s) && s.elapsedTicks==0 && s.classes[BdwWorkerIdle].count==0);
    m.End(5); // no boundary yet: ignored
    m.Begin(BdwWorkerIdle,100);m.End(200);                 // idle 100
    m.Begin(BdwWorkerApplication,250);                      // other 50
    m.Render(3,2,40,true,6u<<7,10u<<7);m.End(400);          // app 150
    m.Begin(BdwWorkerCompositor,400);                       // other 0
    m.Render(0,0,0,false,0,0);m.End(450);                   // compositor 50
    m.Begin(BdwWorkerPresent,460);                          // other 10
    const uint64_t ticks[BdwPresentStageCount]={1,2,3,4,10};
    m.Present(false,64,ticks,0);
    m.Render(9,9,9,true,0,0); // ignored outside an RCS interval
    m.PresentFailed();m.End(470);                           // present 10
    CHECK(m.Read(s));
    CHECK(s.elapsedTicks==370);
    uint64_t sum=0;for(const auto& c:s.classes)sum+=c.ticks;CHECK(sum==s.elapsedTicks);
    CHECK(s.classes[BdwWorkerIdle].ticks==100 && s.classes[BdwWorkerApplication].ticks==150 &&
          s.classes[BdwWorkerCompositor].ticks==50 && s.classes[BdwWorkerPresent].ticks==10 &&
          s.classes[BdwWorkerOther].ticks==60 && s.classes[BdwWorkerOther].count==3);
    CHECK(s.yields[0].jobs==1 && s.yields[0].yields==3 && s.yields[0].breadcrumbYields==2 &&
          s.yields[0].sleepTicks==40 && s.yields[0].buckets[3]==1);
    CHECK(s.yields[1].jobs==1 && s.yields[1].buckets[0]==1 && s.frequency[1].missing==1);
    CHECK(s.frequency[0].issue.samples==1 && s.frequency[0].issue.minMhz==300 && s.frequency[0].end.maxMhz==500 &&
          s.frequency[0].rising==1);
    CHECK(s.present[1][BdwPresentTotal].count==1 && s.present[1][BdwPresentTotal].ticks==10 &&
          s.present[1][BdwPresentBlit].maxTicks==3 && s.presentPixels[1]==64 && s.present[0][BdwPresentTotal].count==0);
    CHECK(s.presentFailed==1 && s.yields[2].jobs==1 && s.yields[2].buckets[0]==1);

    m.Begin(BdwWorkerApplication,500);m.Render(0,0,0,true,8u<<7,8u<<7);m.End(600);
    CHECK(m.Read(s) && s.frequency[0].issue.samples==2 && s.frequency[0].issue.minMhz==300 &&
          s.frequency[0].issue.maxMhz==400 && s.frequency[0].issue.sumMhz==700 && s.frequency[0].rising==1);

    // A reset is applied by the worker at its next boundary and discards
    // the interval before it.
    m.RequestReset();
    CHECK(m.Read(s) && s.resets==0 && s.elapsedTicks==500);
    m.Begin(BdwWorkerIdle,1000);m.End(1300);
    CHECK(m.Read(s) && s.resets==1 && s.elapsedTicks==300 && s.classes[BdwWorkerIdle].ticks==300 &&
          s.classes[BdwWorkerOther].count==0 && s.classes[BdwWorkerApplication].count==0 && s.yields[0].jobs==0);
    // A backwards clock never charges a negative interval.
    m.Begin(BdwWorkerApplication,1200);m.End(1100);
    CHECK(m.Read(s) && s.elapsedTicks==300 && s.classes[BdwWorkerApplication].count==1 &&
          s.classes[BdwWorkerApplication].ticks==0);
    if(failures)std::fprintf(stderr,"%u failure(s)\n",failures);
    return failures?1:0;
}
