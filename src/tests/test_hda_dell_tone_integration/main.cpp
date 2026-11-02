#include <cassert>
#include <cstdint>
#include "../../drivers/intel_hda/hda_dell_tone_buffer.h"
#include "../../drivers/intel_hda/hda_dell_tone_codec.h"
#include "../../drivers/intel_hda/hda_dell_tone_ici.h"
#include "../../drivers/intel_hda/hda_dell_tone_stream.h"
#include "../../drivers/intel_hda/hda_dell_tone_transaction.h"
using namespace brook;
#include "fake_hda_chip.h"
struct Adapter {
    Chip chip{};HdaDellToneIci<Chip> ici{chip,0x4011};HdaDellToneCodec<HdaDellToneIci<Chip>> codec{ici};HdaDellToneStream<Chip> stream{chip};
    alignas(4096) uint8_t bdlPage[4096]{};alignas(4096) int16_t data[3][2048]{};
    bool muteAndVerify(const HdaDellTonePlan& p){return codec.muteAndVerify(p);}
    bool configureMuted(const HdaDellTonePlan& p){return codec.configureMuted(p);}
    bool prepareDma(const HdaDellTonePlan& p){
        int16_t* pages[]={data[0],data[1],data[2]};uint64_t phys[]={0x2000,0x3000,0x4000};uint32_t v=0;
        return HdaDellBuildToneBuffer(reinterpret_cast<HdaToneBdlEntry*>(bdlPage),0x1000,pages,phys,true,p.peakSample) &&
               ici.setFormat(0x11) && ici.getFormat(v) && v==0x11 && ici.set8(3,0x706,0x10) &&
               ici.get8(3,0xf06,v) && v==0x10 && stream.prepare(0x4401,0x1000);
    }
    bool startDma(){return stream.start();}bool checkProgress(){return stream.progress();}
    bool unmuteAndVerify(const HdaDellTonePlan& p){assert(stream.state()==HdaToneStreamState::Running);return codec.unmuteAndVerify(p);}
    bool playBounded(){return chip.pin[0]==0 && chip.pin[1]==0 && stream.progress();}
    bool stopAndVerify(){return stream.stop();}
    bool disconnectAndVerify(const HdaDellTonePlan&){uint32_t v=0;return ici.set8(3,0x706,0) && ici.get8(3,0xf06,v) && v==0;}
    bool restoreAndVerify(const HdaDellTonePlan& p,const HdaSessionReport& s){uint32_t v=0;
        return ici.setFormat(uint16_t(s.converterFormat)) && ici.getFormat(v) && v==s.converterFormat && codec.restoreAndVerify(p,s);}
    void quarantine(){chip.quarantined=true;}
    bool releaseStoppedDma(){assert(stream.state()==HdaToneStreamState::Stopped && !chip.quarantined);if(!stream.retire())return false;chip.released=true;return true;}
};
int main(){
    auto s=makeSnapshot();Adapter a;assert(HdaDellToneTransaction(a,s,true)==HdaToneResult::Complete);
    assert(a.chip.released && !a.chip.quarantined && a.chip.selection==0 && a.chip.dac[0]==127 && a.chip.dac[1]==127 &&
           a.chip.pin[0]==0 && a.chip.pin[1]==0 && a.chip.converter==0 && a.chip.format==0x4011);
    Adapter b;b.chip.dropBdlClear=true;assert(HdaDellToneTransaction(b,s,true)==HdaToneResult::CleanupFailed);
    assert(b.chip.quarantined && !b.chip.released && b.chip.pin[0]==0x80 && b.chip.pin[1]==0x80);
    return 0;
}
