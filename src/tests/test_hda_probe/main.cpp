#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "../../drivers/intel_hda/hda_probe.h"
using namespace brook;
struct Fake {
    uint8_t mem[4096]{};unsigned writes=0, pauses=0;bool timeout=false,staleStuck=false,clockAdvances=false;uint64_t clock=0;
    uint8_t read8(unsigned o) {assert(o<sizeof(mem));return mem[o];}
    uint16_t read16(unsigned o) {assert(o+2<=sizeof(mem));return uint16_t(mem[o])|(uint16_t(mem[o+1])<<8);}
    uint32_t read32(unsigned o) {return uint32_t(read16(o))|(uint32_t(read16(o+2))<<16);}
    void set16(unsigned o,uint16_t v) {mem[o]=v;mem[o+1]=v>>8;}
    void set32(unsigned o,uint32_t v) {set16(o,v);set16(o+2,v>>16);}
    void write16(unsigned o,uint16_t v) {
        assert(o==0x68);++writes;
        if(v==2 && !staleStuck)set16(o,read16(o)&~2u);
        else if(v==1)set16(o,timeout?1:2);
    }
    void write32(unsigned o,uint32_t v) {assert(o==0x60);++writes;set32(o,v);}
    uint64_t ticks() {return clockAdvances?clock++:0;} // stopped-clock fallback is tested too
    void pause() {++pauses;}
};
static Fake idle() {Fake f{};f.set16(0,0x2100);f.mem[3]=1;f.set32(8,1);return f;}
int main() {
    uint64_t base=0;HdaProbePci pci{0x8086,0x9ca0,0x1028,0x0665,0,0x1b,0,4,3,0xf7418004,0};
    assert(HdaProbePciCheck(pci,base)==HdaProbeResult::Ready && base==0xf7418000);
    pci.slot=3;assert(HdaProbePciCheck(pci,base)==HdaProbeResult::InvalidIdentity && base==0);pci.slot=0x1b;
    pci.bar0=0xf7418001;assert(HdaProbePciCheck(pci,base)==HdaProbeResult::InvalidBar);pci.bar0=0xf7418004;
    pci.bar1=0xffffffff;assert(HdaProbePciCheck(pci,base)==HdaProbeResult::InvalidBar);
    pci.bar1=1;assert(HdaProbePciCheck(pci,base)==HdaProbeResult::Ready && base==0x1f7418000ULL);
    pci.bar1=0xfffffffe;pci.bar0=0xfffff004;assert(HdaProbePciCheck(pci,base)==HdaProbeResult::Ready);
    pci.bar1=0xffffffff;pci.bar0=0xfffff004;assert(HdaProbePciCheck(pci,base)==HdaProbeResult::InvalidBar);
    HdaProbeController state{};uint32_t reply=0;
    {auto f=idle();HdaReadOnlyProbe<Fake> probe(f);assert(probe.read(0,0,0xf00,0,reply)==HdaProbeResult::Failed && !f.writes);}
    {auto f=idle();f.set32(8,0);HdaReadOnlyProbe<Fake> probe(f);assert(probe.inspect(state)==HdaProbeResult::ControllerReset && !f.writes);}
    {auto f=idle();f.mem[0x4c]=2;HdaReadOnlyProbe<Fake> probe(f);assert(probe.inspect(state)==HdaProbeResult::ActiveRing && !f.writes);}
    {auto f=idle();f.mem[0x5c]=2;HdaReadOnlyProbe<Fake> probe(f);assert(probe.inspect(state)==HdaProbeResult::ActiveRing && !f.writes);}
    {auto f=idle();f.mem[0x80+2*0x20]=2;HdaReadOnlyProbe<Fake> probe(f);assert(probe.inspect(state)==HdaProbeResult::ActiveStream && !f.writes);}
    {auto f=idle();f.set16(0x68,1);HdaReadOnlyProbe<Fake> probe(f);assert(probe.inspect(state)==HdaProbeResult::Busy && !f.writes);}
    {auto f=idle();f.set32(0x20,1);HdaReadOnlyProbe<Fake> probe(f);assert(probe.inspect(state)==HdaProbeResult::ActiveInterrupts && !f.writes);}
    {auto f=idle();f.set16(0,0xffff);HdaReadOnlyProbe<Fake> probe(f);assert(probe.inspect(state)==HdaProbeResult::InvalidCapabilities && !f.writes);}
    {auto f=idle();f.set16(0,0xfff8);HdaReadOnlyProbe<Fake> probe(f);assert(probe.inspect(state)==HdaProbeResult::InvalidCapabilities && !f.writes);}
    {auto f=idle();f.set16(0,0x1108);f.mem[0x80+2*0x20]=2;HdaReadOnlyProbe<Fake> probe(f);assert(probe.inspect(state)==HdaProbeResult::ActiveStream && !f.writes);}
    {auto f=idle();f.set16(0x68,2);f.set32(0x64,0x10ec0288);HdaReadOnlyProbe<Fake> probe(f);
     assert(probe.inspect(state)==HdaProbeResult::Ready && state.streams==3);
     assert(probe.read(0,1,0x705,0,reply)==HdaProbeResult::DisallowedVerb && !f.writes);
     for(uint16_t verb: {uint16_t(0x701),uint16_t(0x706),uint16_t(0x707),uint16_t(0x70c),uint16_t(0x7ff)})
       assert(probe.read(0,1,verb,0,reply)==HdaProbeResult::DisallowedVerb && !f.writes);
     assert(probe.read(15,0,0xf00,0,reply)==HdaProbeResult::DisallowedVerb && !f.writes);
     assert(probe.read(0,0,0xf00,0xff,reply)==HdaProbeResult::DisallowedVerb && !f.writes);
     assert(probe.read(0,0,0xf00,0,reply)==HdaProbeResult::Ready && reply==0x10ec0288 && f.writes==3);
     assert(f.read32(0x60)==0x000f0000);}
    {auto f=idle();f.timeout=true;HdaReadOnlyProbe<Fake> probe(f);assert(probe.inspect(state)==HdaProbeResult::Ready);
     assert(probe.read(0,0,0xf00,0,reply)==HdaProbeResult::Timeout && f.pauses==100000);
     const auto writes=f.writes;assert(probe.read(0,0,0xf00,0,reply)==HdaProbeResult::Failed && f.writes==writes);}
    {auto f=idle();f.timeout=true;f.clockAdvances=true;HdaReadOnlyProbe<Fake> probe(f);assert(probe.inspect(state)==HdaProbeResult::Ready);
     assert(probe.read(0,0,0xf00,0,reply)==HdaProbeResult::Timeout && f.pauses<100 && f.clock>=50);}
    {auto f=idle();f.set16(0x68,2);f.staleStuck=true;HdaReadOnlyProbe<Fake> probe(f);assert(probe.inspect(state)==HdaProbeResult::Ready);
     assert(probe.read(0,0,0xf00,0,reply)==HdaProbeResult::Timeout && f.writes==1);}
    std::puts("HDA probe: PASS");
}
