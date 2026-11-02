#include <cassert>
#include <cstdint>
#include "../../drivers/intel_hda/hda_dell_tone_stream.h"
using namespace brook;
struct Fake {
    uint8_t mem[4096]{};uint64_t time=0;unsigned writes=0,delays=0,wrapReads=0,lpibReads=0;bool published=false,stuckReset=false,stuckClear=false,stuckStop=false,noProgress=false,dropBdl=false,dropRun=false,delayFails=false,roPriority=false,needsReset=false,wrapAtEnd=false,badSecond=false;
    static constexpr uint32_t base=0x100; // GCAP advertises four input streams
    uint8_t read8(uint32_t o){assert(o<sizeof(mem));return mem[o];}
    uint16_t read16(uint32_t o){return uint16_t(read8(o))|(uint16_t(read8(o+1))<<8);}
    uint32_t read32(uint32_t o){if(o==base+4 && (read16(base)&2) && badSecond && ++lpibReads==2)return 12289;
        if(o==base+4 && (read16(base)&2) && wrapAtEnd){++wrapReads;if(wrapReads==1)return 12288;
            if(wrapReads==2){mem[o]=mem[o+1]=0;return 0;}wrapAtEnd=false;}
        if(o==base+4 && (read16(base)&2) && !noProgress)mem[o]+=4;return uint32_t(read16(o))|(uint32_t(read16(o+2))<<16);}
    void write8(uint32_t o,uint8_t v){assert(o<sizeof(mem));++writes;if(o==base+3)mem[o]&=~v;else mem[o]=uint8_t(v|((o==base+2 && roPriority)?4:0));}
    void write16(uint32_t o,uint16_t v){++writes;if(o==base && v==2)assert(published);
        if(o==base && stuckReset && v==1)return;if(o==base && stuckStop && v==0 && (read16(base)&2))return;
        if(o==base && stuckClear && v==0 && (read16(base)&1))return;if(o==base && dropRun && v==2)return;
        if(o==base && v==2)needsReset=true;if(o==base && v==0 && (read16(base)&1))needsReset=false;
        mem[o]=uint8_t(v);mem[o+1]=uint8_t(v>>8);}
    void write32(uint32_t o,uint32_t v){++writes;if(o==base+8)assert(!needsReset);if(dropBdl && o==base+0x18)return;for(unsigned i=0;i<4;++i)mem[o+i]=uint8_t(v>>(8*i));}
    bool publishDma(){published=true;return true;}
    uint64_t ticks(){return time++;}void pause(){}bool delayUs(unsigned us){assert(us>=3);++delays;return !delayFails;}
};
int main(){
    {Fake f;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000));
     assert(f.read16(Fake::base+0x12)==0x11 && f.read16(Fake::base+0x0c)==2 && f.read32(Fake::base+8)==12288);
     assert(f.delays==2 && s.start() && s.progress() && s.stop() && s.state()==HdaToneStreamState::Stopped);
     assert(s.retire() && s.state()==HdaToneStreamState::Idle && f.read32(Fake::base+0x18)==0);}
    {Fake f;f.roPriority=true;f.mem[Fake::base+2]=4;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000));
     assert(f.read8(Fake::base+2)==0x14 && s.start() && s.stop() && s.retire() && f.read8(Fake::base+2)==4 && f.delays==4);}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(!s.prepare(0x0401,0x1000) && !f.writes);}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(!s.prepare(0x4400,0x100000000ULL) && !f.writes);}
    {Fake f;f.mem[Fake::base]=2;HdaDellToneStream<Fake> s(f);assert(!s.prepare(0x4401,0x1000) && !f.writes);}
    {Fake f;f.stuckReset=true;HdaDellToneStream<Fake> s(f);assert(!s.prepare(0x4401,0x1000) && s.state()==HdaToneStreamState::Unknown);}
    {Fake f;f.stuckClear=true;HdaDellToneStream<Fake> s(f);assert(!s.prepare(0x4401,0x1000) && s.state()==HdaToneStreamState::Unknown);}
    {Fake f;f.delayFails=true;HdaDellToneStream<Fake> s(f);assert(!s.prepare(0x4401,0x1000) && s.state()==HdaToneStreamState::Unknown);}
    {Fake f;f.dropBdl=true;HdaDellToneStream<Fake> s(f);assert(!s.prepare(0x4401,0x1000) && s.state()==HdaToneStreamState::Unknown && !f.published);}
    {Fake f;f.dropRun=true;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000));assert(!s.start() && s.state()==HdaToneStreamState::Unknown && s.stop());}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000) && s.start());f.noProgress=true;
     assert(!s.progress() && s.progressReason()==7);f.stuckStop=true;assert(!s.stop() && s.state()==HdaToneStreamState::Unknown);}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000) && s.start());f.noProgress=true;
     f.mem[Fake::base+4]=1;f.mem[Fake::base+5]=0x30;assert(!s.progress() && s.progressReason()==2 && s.stop());}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000) && s.start());f.wrapAtEnd=true;
     assert(s.progress() && f.wrapReads==3 && s.progressFirst()==12288 && s.progressLast()==4 && s.stop());}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000) && s.start());f.noProgress=true;
     f.mem[Fake::base+5]=0x30;assert(!s.progress() && s.progressReason()==7 && s.stop());}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000) && s.start());f.badSecond=true;
     assert(!s.progress() && s.progressReason()==5 && s.progressLast()==12289 && s.stop());}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000) && s.start());
     f.mem[Fake::base+3]=0x10;assert(!s.progress() && s.progressReason()==3 && s.progressStatus()==0x10 && s.stop());}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000));f.mem[Fake::base+3]=8;
     assert(!s.start() && s.state()==HdaToneStreamState::Unknown && s.stop());}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000) && s.start() && s.stop());
     f.dropBdl=true;assert(!s.retire() && s.state()==HdaToneStreamState::Unknown);}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000) && s.start() && s.stop());
     f.mem[Fake::base]=2;assert(!s.retire() && s.state()==HdaToneStreamState::Unknown);}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000) && s.start() && s.stop());
     f.stuckReset=true;assert(!s.retire() && s.state()==HdaToneStreamState::Unknown && f.read32(Fake::base+8)==12288);}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000) && s.start() && s.stop());
     f.stuckClear=true;assert(!s.retire() && s.state()==HdaToneStreamState::Unknown && f.read32(Fake::base+8)==12288);}
    {Fake f;HdaDellToneStream<Fake> s(f);assert(s.prepare(0x4401,0x1000) && s.start() && s.stop());
     f.delayFails=true;assert(!s.retire() && s.state()==HdaToneStreamState::Unknown && f.read32(Fake::base+8)==12288);}
}
