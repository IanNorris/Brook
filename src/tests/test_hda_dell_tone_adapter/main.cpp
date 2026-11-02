#include <cassert>
#include <cstring>
#include "../../drivers/intel_hda/hda_dell_tone_adapter.h"
#include "../test_hda_dell_tone_integration/fake_hda_chip.h"
using namespace brook;
struct Platform:Chip {
    alignas(4096) uint8_t pages[4][4096]{};unsigned allocated=0,freed=0;uint16_t pci=0;
    int failAllocation=-1;bool identity=true,denyDecode=false,denyMaster=false,denyDisable=false,denyFinish=false,stuckStop=false,stallPlaybackClock=false,badIdle=false,everMaster=false;
    bool identityStable(){return identity;}uint16_t command(){return pci;}
    void setCommand(uint16_t c){if(c==6){assert(published);everMaster=true;}if((c==2 && pci==0 && denyDecode) || (c==6 && denyMaster) ||
        (c==2 && pci==6 && denyDisable) || (c==0 && pci==2 && denyFinish))return;
        pci=c;}
    bool controllerIdle(){return !badIdle && !(read32(0x70)&1) && read16(base)==0;}
    bool allocatePage(uint64_t& physical,void*& virt){if(int(allocated)==failAllocation)return false;
        assert(allocated<4);physical=0x1000+allocated*4096;virt=pages[allocated++];return true;}
    bool pageAllowed(uint64_t physical,void* virt){return physical && virt && !(physical&4095) && !(reinterpret_cast<uintptr_t>(virt)&4095);}
    void clearPage(void* p){std::memset(p,0,4096);}
    void freePage(uint64_t){assert((pci&4)==0);if(published)assert(read16(base)==0 && read32(base+0x18)==0 && read32(base+0x1c)==0);++freed;}
    void quarantine(){quarantined=true;}
    uint64_t ticks(){if(stallPlaybackClock && pci==6 && published && pin[0]==0 && pin[1]==0)return time;return time++;}
    void write16(uint32_t o,uint16_t v){if(stuckStop && o==base && v==0 && (read16(base)&2))return;Chip::write16(o,v);}
};
int main(){
    const auto s=makeSnapshot();
    {Platform p;HdaDellToneAdapter<Platform> a(p,s);assert(a.allocate() && a.begin());
     assert(a.run(true)==HdaToneResult::Complete);assert(a.run(true)==HdaToneResult::InvalidSnapshot);assert(a.finish());
     assert(p.freed==4 && p.pci==0 && !p.quarantined && p.sawQuietUnmute && p.pin[0]==0 && p.pin[1]==0);}
    {Platform p;HdaDellToneAdapter<Platform> a(p,s);assert(a.allocate() && a.begin());
     assert(a.run(false)==HdaToneResult::Complete && a.finish() && !p.sawQuietUnmute && p.freed==4);}
    for(int fail=0;fail<4;++fail){Platform p;p.failAllocation=fail;HdaDellToneAdapter<Platform> a(p,s);
     assert(!a.allocate() && p.freed==unsigned(fail) && p.pci==0);}
    {Platform p;HdaDellToneAdapter<Platform> a(p,s);assert(a.allocate());p.identity=false;
     assert(!a.begin() && p.freed==4 && !p.quarantined);}
    {Platform p;p.denyDecode=true;HdaDellToneAdapter<Platform> a(p,s);assert(a.allocate());
     assert(!a.begin() && p.freed==4 && p.pci==0);}
    {Platform p;p.badIdle=true;p.denyFinish=true;HdaDellToneAdapter<Platform> a(p,s);assert(a.allocate());
     assert(!a.begin() && p.quarantined && p.freed==0 && p.pci==2);}
    {Platform p;p.mem[0x70]=1;HdaDellToneAdapter<Platform> a(p,s);assert(a.allocate());
     assert(!a.begin() && p.freed==4 && p.pci==0 && !p.everMaster && !(p.read16(Platform::base)&2));}
    {Platform p;HdaDellToneAdapter<Platform> a(p,s);HdaDellTonePlan plan{};
     assert(HdaDellMakeTonePlan(s,plan) && a.allocate() && a.begin() && a.muteAndVerify(plan) && a.configureMuted(plan) && a.prepareDma(plan));
     assert(!a.begin() && p.freed==0 && p.pci==2);}
    {Platform p;p.denyMaster=true;HdaDellToneAdapter<Platform> a(p,s);assert(a.allocate() && a.begin());
     assert(a.run(true)==HdaToneResult::StartFailed && p.quarantined && p.freed==0);}
    {Platform p;p.stuckStop=true;HdaDellToneAdapter<Platform> a(p,s);assert(a.allocate() && a.begin());
     assert(a.run(true)==HdaToneResult::CleanupFailed && p.quarantined && p.freed==0 && p.pci==6);}
    {Platform p;p.denyDisable=true;HdaDellToneAdapter<Platform> a(p,s);assert(a.allocate() && a.begin());
     assert(a.run(true)==HdaToneResult::CleanupFailed && p.quarantined && p.freed==0 && p.pci==6);}
    {Platform p;p.dropBdlClear=true;HdaDellToneAdapter<Platform> a(p,s);assert(a.allocate() && a.begin());
     assert(a.run(true)==HdaToneResult::CleanupFailed && p.quarantined && p.freed==0 && p.pci==2);}
    {Platform p;p.stallPlaybackClock=true;HdaDellToneAdapter<Platform> a(p,s);assert(a.allocate() && a.begin());
     assert(a.run(true)==HdaToneResult::PlaybackFailed && p.quarantined && p.freed==0 && p.pin[0]==0x80 && p.pin[1]==0x80);}
    {Platform p;p.denyFinish=true;HdaDellToneAdapter<Platform> a(p,s);assert(a.allocate() && a.begin());
     assert(a.run(true)==HdaToneResult::Complete && !a.finish() && p.quarantined && p.freed==4 && p.pci==2);}
}
