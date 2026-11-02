#include <cassert>
#include "../../drivers/intel_hda/hda_dell_tone_plan.h"
using namespace brook;
static HdaSessionReport observed() {
    HdaSessionReport s{};s.result=HdaSessionResult::Complete;s.probe=HdaProbeResult::Ready;
    s.vendor=0x10ec0288;s.subsystem=0x10280665;s.completed=12;s.stateCompleted=13;s.ampCompleted=6;s.formatCompleted=true;
    const uint32_t values[]={0x40050d,0x10010,0x90170110,2,0x0d0c,0,0x40,2,0x20010b,1,3,0x41d};
    const uint32_t states[]={0x400,0xe0060,1,0,0,0x400,0,0xe0060,1,0x17f7f,0x80000000,0,0x80000000};
    for(unsigned i=0;i<12;++i)s.values[i]=values[i];for(unsigned i=0;i<13;++i)s.stateValues[i]=states[i];
    s.ampValues[0]=s.ampValues[1]=0x7f;
    return s;
}
int main(){
    HdaDellTonePlan p{};auto s=observed();assert(HdaDellMakeTonePlan(s,p));
    assert(p.pin==0x14 && p.mixer==0x0d && p.dac==3 && p.pinSelection==1 && p.dacGain==80 && p.peakSample==9830);
    uint16_t format=0;assert(HdaDellMixerFormat(s,format) && format==0x4011);
    s=observed();s.stateValues[7]&=~(1u<<5);assert(!HdaDellMixerFormat(s,format) && !format);
    s=observed();s.stateValues[7]&=~(1u<<17);assert(!HdaDellMixerFormat(s,format) && !format);
    s=observed();s.stateValues[8]&=~1u;assert(!HdaDellMixerFormat(s,format) && !format);
    s=observed();s.stateValues[0]|=0x100;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.stateValues[5]|=0x10;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.stateValues[7]&=~(1u<<6);assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.stateValues[12]=0;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.stateValues[9]=0;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.stateValues[9]=0x017f00;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.stateValues[9]=0x7f7f7f;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.values[0]|=0x200;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.values[3]|=0x80;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.values[8]&=~8u;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.values[4]=0x0c0d;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.ampCompleted=5;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.formatCompleted=false;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.stateValues[6]=0x10;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.ampValues[2]=1;assert(!HdaDellMakeTonePlan(s,p));
    s=observed();s.subsystem++;assert(!HdaDellMakeTonePlan(s,p));
}
