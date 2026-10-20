#include <cstdio>
#include "../../kernel/src/audio_delivery_timing.h"
static int failures;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"delivery timing line %d: %s\n",__LINE__,#x);++failures;}}while(0)
int main(){
 using namespace brook;AudioDeliveryTimingModel m;
 AudioDeliveryEvent e{1,1000,50000,51000,744,11544,12288,3072,1024,true,true,true};
 CHECK(!m.Observe(0,e));m.Enable(true);auto token=m.Token();CHECK(m.Observe(token,e));
 auto s=m.Snapshot();CHECK(s.observations==1 && s.reservations==1 && s.reservedBytes==11544);
 CHECK(s.maxCopyNs==49000 && s.maxSubmissionNs==50000 && s.maxAdvanceBytes==1024 && s.minHeadroomBytes==11264);
 CHECK(s.copyBins[2]==1 && !s.unsafe && !s.invalidClock);
 e.hardwareEpoch=99;e.leadBeforeBytes=744;e.publishSafe=false;CHECK(m.Observe(token,e));CHECK(m.Snapshot().minHeadroomBytes==0 && m.Snapshot().unsafe==1);
 CHECK(m.Snapshot().maxAdvanceEvent.hardwareEpoch==1 && m.Snapshot().minHeadroomEvent.hardwareEpoch==99);
 e.positionValid=false;e.afterCopyNs=999;CHECK(m.Observe(token,e));s=m.Snapshot();CHECK(s.invalidClock==1 && s.invalidPosition==1 && s.runningSamples==2);
 m.Reset();CHECK(!m.Observe(token,e));token=m.Token();s=m.Snapshot();CHECK(!s.observations && s.minHeadroomBytes==~uint64_t(0));
 e={2,100,100,0,0,0,0,4,0,false,false,true};CHECK(m.Observe(token,e));s=m.Snapshot();CHECK(s.copyBins[0]==1 && !s.invalidClock && !s.invalidPosition && !s.runningSamples);
 for(unsigned i=0;i<40;++i){e.hardwareEpoch=i+3;CHECK(m.Observe(token,e));}
 s=m.Snapshot();CHECK(!s.maxAdvanceEvent.hardwareEpoch && !s.minHeadroomEvent.hardwareEpoch);
 CHECK(s.retained==16 && s.observations==41 && s.events[(s.next+15)%16].hardwareEpoch==42);
 m.Enable(false);CHECK(!m.Observe(token,e));m.Enable(true);token=m.Token();
 e.reservedBytes=~uint64_t(0);CHECK(m.Observe(token,e));CHECK(m.Observe(token,e));CHECK(m.Snapshot().saturated && m.Snapshot().reservedBytes==~uint64_t(0));
 std::printf("AUDIO_DELIVERY_TIMING %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
