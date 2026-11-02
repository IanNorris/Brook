#include <cassert>
#include <string>
#include "../../drivers/intel_hda/hda_dell_tone_transaction.h"
using namespace brook;
static HdaSessionReport snapshot() {
    HdaSessionReport s{};s.result=HdaSessionResult::Complete;s.probe=HdaProbeResult::Ready;
    s.vendor=0x10ec0288;s.subsystem=0x10280665;s.completed=12;s.stateCompleted=13;s.ampCompleted=6;s.formatCompleted=true;
    const uint32_t v[]={0x40050d,0x10010,0x90170110,2,0x0d0c,0,0x40,2,0x20010b,1,3,0x41d};
    const uint32_t p[]={0x400,0xe0060,1,0,0,0x400,0,0xe0060,1,0x17f7f,0x80000000,0,0x80000000};
    for(unsigned i=0;i<12;++i)s.values[i]=v[i];for(unsigned i=0;i<13;++i)s.stateValues[i]=p[i];
    s.ampValues[0]=s.ampValues[1]=0x7f;return s;
}
struct Fake {
    std::string events;char fail=0;unsigned muteCount=0;bool failCleanupMute=false,dmaActive=false,quarantined=false;
    bool step(char c){events+=c;return c!=fail;}
    bool muteAndVerify(const HdaDellTonePlan&){events+='M';++muteCount;return fail!='M' && !(failCleanupMute && muteCount>1);}
    bool configureMuted(const HdaDellTonePlan&){assert(!dmaActive);return step('C');}
    bool prepareDma(const HdaDellTonePlan&){assert(!dmaActive);return step('P');}
    bool startDma(){dmaActive=true;return step('S');} // failure may still assert RUN
    bool checkProgress(){return step('G');}
    bool unmuteAndVerify(const HdaDellTonePlan&){return step('U');}
    bool playBounded(){return step('T');}
    bool stopAndVerify(){bool ok=step('X');if(ok)dmaActive=false;return ok;}
    bool disconnectAndVerify(const HdaDellTonePlan&){return step('D');}
    bool restoreAndVerify(const HdaDellTonePlan&,const HdaSessionReport&){return step('R');}
    void quarantine(){events+='Q';quarantined=true;}
    bool releaseStoppedDma(){assert(!dmaActive && !quarantined);return step('F');}
};
int main(){
    auto s=snapshot();
    {Fake f;assert(HdaDellToneTransaction(f,s,true)==HdaToneResult::Complete);assert(f.events=="MCPSGUTMXDRF");}
    {Fake f;assert(HdaDellToneTransaction(f,s,false)==HdaToneResult::Complete);assert(f.events=="MCPSGMXDRF");}
    {Fake f;auto bad=s;bad.vendor=0;assert(HdaDellToneTransaction(f,bad,true)==HdaToneResult::InvalidSnapshot && f.events.empty());}
    {Fake f;f.fail='M';assert(HdaDellToneTransaction(f,s,true)==HdaToneResult::MuteFailed && f.events=="MQ");}
    {Fake f;f.fail='C';assert(HdaDellToneTransaction(f,s,true)==HdaToneResult::ConfigureFailed && f.events=="MCMQ");}
    {Fake f;f.fail='P';assert(HdaDellToneTransaction(f,s,true)==HdaToneResult::PrepareFailed && f.events=="MCPMQ");}
    {Fake f;f.fail='S';assert(HdaDellToneTransaction(f,s,true)==HdaToneResult::StartFailed && f.events=="MCPSMXQ");}
    {Fake f;f.fail='G';assert(HdaDellToneTransaction(f,s,true)==HdaToneResult::NoProgress && f.events=="MCPSGMXQ");}
    {Fake f;f.fail='U';assert(HdaDellToneTransaction(f,s,true)==HdaToneResult::UnmuteFailed && f.events=="MCPSGUMXQ");}
    {Fake f;f.fail='T';assert(HdaDellToneTransaction(f,s,true)==HdaToneResult::PlaybackFailed && f.events=="MCPSGUTMXQ");}
    {Fake f;f.fail='X';assert(HdaDellToneTransaction(f,s,true)==HdaToneResult::CleanupFailed && f.events=="MCPSGUTMXQ" && f.dmaActive && f.quarantined);}
    {Fake f;f.failCleanupMute=true;assert(HdaDellToneTransaction(f,s,true)==HdaToneResult::CleanupFailed && f.events=="MCPSGUTMXQ");}
    {Fake f;f.fail='D';assert(HdaDellToneTransaction(f,s,true)==HdaToneResult::CleanupFailed && f.events=="MCPSGUTMXDMQ");}
    {Fake f;f.fail='R';assert(HdaDellToneTransaction(f,s,true)==HdaToneResult::CleanupFailed && f.events=="MCPSGUTMXDRMQ");}
    {Fake f;f.fail='F';assert(HdaDellToneTransaction(f,s,true)==HdaToneResult::CleanupFailed && f.events=="MCPSGUTMXDRFMQ");}
}
