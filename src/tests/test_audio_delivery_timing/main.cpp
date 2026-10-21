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
 // A rare reservation must survive many ordinary submissions overwriting the
 // recent-event ring, together with its actual preceding observed submission.
 AudioDeliveryTimingModel history;history.Enable(true);auto epoch=history.Token();
 e={7,100,101,102,16000,0,16000,1024,0,true,true,true};CHECK(history.Observe(epoch,e));
 auto previous=e;e.beforeCopyNs=30000000;e.afterCopyNs=e.beforeCopyNs+1;e.afterPositionNs=e.beforeCopyNs+2;
 e.pendingBeforeReserve=512;e.reservedBytes=11776;e.leadBeforeBytes=12288;
 CHECK(history.Observe(epoch,e));auto reservation=e;
 e.reservedBytes=0;
 for(unsigned i=0;i<64;++i){e.beforeCopyNs+=10000000;e.afterCopyNs=e.beforeCopyNs+1;e.afterPositionNs=e.beforeCopyNs+2;CHECK(history.Observe(epoch,e));}
 s=history.Snapshot();CHECK(s.reservationsRetained==1 && !s.reservationsOverwritten);
 auto pair=s.reservationEvents[0];CHECK(pair.hasPrevious && pair.controlEpoch==epoch && pair.observation==2);
 CHECK(pair.previous.beforeCopyNs==previous.beforeCopyNs && pair.previous.acceptedBytes==1024);
 CHECK(pair.current.beforeCopyNs==reservation.beforeCopyNs && pair.current.reservedBytes==11776);
 CHECK(s.events[(s.next+AudioDeliveryEventSlots-s.retained)%AudioDeliveryEventSlots].beforeCopyNs>reservation.beforeCopyNs);
 // Bounded reservation retention exposes exact overwrite counts and keeps
 // chronological observation numbers even when the ordinary ring wraps.
 e.reservedBytes=4;
 for(unsigned i=0;i<32;++i){e.beforeCopyNs+=10000000;e.afterCopyNs=e.beforeCopyNs+1;e.afterPositionNs=e.beforeCopyNs+2;CHECK(history.Observe(epoch,e));}
 s=history.Snapshot();CHECK(s.reservations==33 && s.reservationsRetained==AudioDeliveryReservationSlots && s.reservationsOverwritten==17);
 uint64_t sequence=0;
 for(unsigned i=0;i<s.reservationsRetained;++i){const auto& p=s.reservationEvents[(s.reservationNext+AudioDeliveryReservationSlots-s.reservationsRetained+i)%AudioDeliveryReservationSlots];CHECK(p.hasPrevious && p.observation>sequence);sequence=p.observation;}
 CHECK(sequence==s.observations);
 // Controls fence adjacency even without clearing cumulative observations.
 history.Enable(true);CHECK(!history.Observe(epoch,e));epoch=history.Token();CHECK(history.Observe(epoch,e));
 s=history.Snapshot();pair=s.reservationEvents[(s.reservationNext+AudioDeliveryReservationSlots-1)%AudioDeliveryReservationSlots];CHECK(!pair.hasPrevious && pair.controlEpoch==epoch);
 CHECK(history.Observe(epoch,e));s=history.Snapshot();CHECK(s.reservationEvents[(s.reservationNext+AudioDeliveryReservationSlots-1)%AudioDeliveryReservationSlots].hasPrevious);
 // Preserve an epoch change visibly; callers must not compare those clocks.
 e.hardwareEpoch=8;CHECK(history.Observe(epoch,e));s=history.Snapshot();pair=s.reservationEvents[(s.reservationNext+AudioDeliveryReservationSlots-1)%AudioDeliveryReservationSlots];CHECK(pair.hasPrevious && pair.previous.hardwareEpoch==7 && pair.current.hardwareEpoch==8);
 history.Reset();CHECK(!history.Observe(epoch,e));epoch=history.Token();CHECK(history.Observe(epoch,e));s=history.Snapshot();CHECK(s.reservationsRetained==1 && !s.reservationsOverwritten && !s.reservationEvents[0].hasPrevious);
 std::printf("AUDIO_DELIVERY_TIMING %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
