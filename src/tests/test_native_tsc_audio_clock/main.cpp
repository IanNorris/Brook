#include <cassert>
#include <cstdint>
#include "../../kernel/src/native_tsc_audio_clock.h"
using namespace brook;
static NativeTscObservation good(){NativeTscObservation o{};o.capable=o.measured=o.consistent=true;o.khz=2400000;o.coreExpected=o.coreCount=3;
    for(unsigned i=0;i<3;++i){o.cores[i].cpu=i+1;o.cores[i].exchanges=5;o.cores[i].result=NativeTscExchangeResult::Complete;
        o.cores[i].interval={-200,1200,1400};}return o;}
int main(){
    auto o=good();NativeTscAudioClock clock;assert(clock.init(o,4,100));uint64_t ns=0;
    assert(clock.read(0,100,ns) && ns==1);assert(clock.read(3,2400100,ns) && ns==1000001);
    assert(!clock.read(2,2400099,ns) && clock.failed() && !clock.read(0,2500000,ns));
    for(unsigned i=0;i<11;++i){auto bad=good();switch(i){case 0:bad.capable=false;break;case 1:bad.consistent=false;break;
        case 2:bad.coreCount=2;break;case 3:bad.cores[1].cpu=1;break;case 4:bad.cores[2].exchanges=4;break;
        case 5:bad.cores[0].result=NativeTscExchangeResult::Timeout;break;case 6:bad.cores[0].interval.lower=-24001;break;
        case 7:bad.cores[0].interval.roundTrip=240001;break;case 8:bad.cores[0].interval={1,-1,10};break;
        case 9:bad.measured=false;break;case 10:bad.cores[0].interval.upper=24001;break;}
        NativeTscAudioClock c;assert(!c.init(bad,4,100));}
    auto edge=good();edge.cores[0].interval={-24000,24000,240000};NativeTscAudioClock exact;assert(exact.init(edge,4,100));
    NativeTscAudioClock badCpu;assert(badCpu.init(o,4,100));assert(!badCpu.read(4,200,ns) && badCpu.failed());
    NativeTscObservation single=good();single.coreExpected=single.coreCount=0;NativeTscAudioClock one;
    assert(one.init(single,1,100) && one.read(0,101,ns));
    NativeTscAudioClock overflow;assert(overflow.init(single,1,1));ns=77;
    // This valid 2.4 GHz conversion stays within uint64; an invalid CPU must
    // leave the caller's output intact and permanently fault the clock.
    assert(!overflow.read(1,~uint64_t(0),ns) && ns==77 && overflow.failed());
}
