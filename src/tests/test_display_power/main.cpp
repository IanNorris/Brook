#include <cassert>
#include "intel_backlight.h"
using namespace brook;
struct Device {
    BacklightRegisters registers{IntelBacklight::Enable|0x60000000,300,IntelBacklight::Enable,(1000u<<16)|200};
    uint32_t writes=0,lastOffset=0,lastValue=0;
    bool corruptWrite=false;
    uint32_t& Reg(uint32_t offset){
        switch(offset){case IntelBacklight::CpuControl:return registers.cpuControl;
            case IntelBacklight::CpuDuty:return registers.cpuDuty;
            case IntelBacklight::PchControl:return registers.pchControl;
            case IntelBacklight::PchDuty:return registers.pchDuty;}
        assert(false);return registers.cpuControl;
    }
    BacklightIo io(){return {this,[](void* c,uint32_t offset){return static_cast<Device*>(c)->Reg(offset);},
        [](void* c,uint32_t offset,uint32_t value){auto& d=*static_cast<Device*>(c);
            ++d.writes;d.lastOffset=offset;d.lastValue=value;d.Reg(offset)=value^(d.corruptWrite?1:0);}};}
};
int main(){
    Device d;IntelBacklight b;
    assert(b.Probe(d.io()) && b.cpuRoute && !d.writes);b.PanicRestore(d.io());assert(!d.writes);
    const auto saved=d.registers;
    assert(b.SetOn(d.io(),false) && !b.on && d.writes==1 && d.lastOffset==IntelBacklight::CpuDuty && d.lastValue==0);
    assert(d.registers.pchDuty==saved.pchDuty && d.registers.pchControl==saved.pchControl && d.registers.cpuControl==saved.cpuControl);
    assert(b.SetOn(d.io(),false) && d.writes==1);
    assert(b.SetOn(d.io(),true) && b.on && d.registers.cpuDuty==saved.cpuDuty);
    assert(b.SetOn(d.io(),false));b.PanicRestore(d.io());assert(d.registers.cpuDuty==saved.cpuDuty);
    // Separate PCH override path, including active-low polarity, preserves all
    // firmware bits outside the duty field. Zero logical duty is off.
    d={};d.registers.cpuControl=0;d.registers.pchControl|=IntelBacklight::Override|(1u<<29);
    b={};assert(b.Probe(d.io()) && !b.cpuRoute && !d.writes);
    assert(b.SetOn(d.io(),false) && d.registers.pchDuty==(1000u<<16));
    assert(b.SetOn(d.io(),true) && d.registers.pchDuty==((1000u<<16)|200));
    // Reject unsupported/disabled/ambiguous/invalid firmware states without a
    // single PWM write; no speculative frequency or routing initialization.
    for(unsigned bad=0;bad<6;++bad){
        d={};if(bad==0)d.registers.pchControl=0;
        if(bad==1)d.registers.cpuControl=0;
        if(bad==2)d.registers.pchDuty=200;
        if(bad==3)d.registers.cpuDuty=0;
        if(bad==4)d.registers.cpuDuty=1001;
        if(bad==5)d.registers.cpuControl=0xffffffff;
        b={};assert(!b.Probe(d.io()) && !b.SetOn(d.io(),false));b.PanicRestore(d.io());assert(!d.writes);
    }
    // Firmware changes after admission invalidate writes, including emergency
    // restore when the saved route no longer describes current hardware.
    d={};b={};assert(b.Probe(d.io()));d.registers.pchControl^=IntelBacklight::Override;
    assert(!b.SetOn(d.io(),false) && !d.writes);b.PanicRestore(d.io());assert(!d.writes);
    d={};b={};assert(b.Probe(d.io()));d.registers.cpuDuty=250;
    assert(!b.SetOn(d.io(),false) && !b.SetOn(d.io(),true) && !d.writes);
    d={};b={};assert(b.Probe(d.io()));assert(b.SetOn(d.io(),false));d.registers.cpuDuty=1;
    assert(!b.SetOn(d.io(),false) && d.writes==1);
    d={};b={};assert(b.Probe(d.io()));d.corruptWrite=true;
    assert(!b.SetOn(d.io(),false) && b.uncertain);d.corruptWrite=false;
    assert(b.SetOn(d.io(),true) && !b.uncertain && d.registers.cpuDuty==300);
    // Panic may interrupt after a PWM write and before normal readback.
    assert(b.SetOn(d.io(),false));b.PanicRestore(d.io());assert(d.registers.cpuDuty==300);
    DisplayIdlePolicy p;assert(!p.Observe(100,8) && !p.Expired(100000));
    p.timeoutSeconds=5;assert(!p.Expired(5099) && p.Expired(5100));
    assert(p.Observe(5100,9) && !p.Expired(5100) && !p.Expired(10099));
    p.Reset(10000,9);p.restoreAtMs=13000;
    assert(!p.Observe(12999,9) && p.Observe(13000,9));
    p.Reset(13000,9);assert(!p.restoreAtMs);
    assert(!p.Observe(500,9) && !p.Expired(500)); // clock fallback/reversal resets idle
    p.timeoutSeconds=86400;p.Reset(0,0);assert(!p.Expired(86399999) && p.Expired(86400000));
    // Production controller used by the compositor: manual timed restoration,
    // genuine input wake, timeout reset/disable and invalid mutation rejection.
    DisplayBacklightController c;d={};assert(c.pwm.Probe(d.io()));
    assert(c.SetOn(d.io(),false,10,100,11) && !c.pwm.on);
    c.Tick(d.io(),101,11);assert(!c.pwm.on);
    c.Tick(d.io(),10100,11);assert(c.pwm.on && !c.idle.restoreAtMs);
    assert(c.SetOn(d.io(),false,0,11000,11));c.Tick(d.io(),11001,12);assert(c.pwm.on);
    assert(c.SetIdle(d.io(),5,12000,12));c.Tick(d.io(),16999,12);assert(c.pwm.on);
    c.Tick(d.io(),17000,12);assert(!c.pwm.on);c.Tick(d.io(),17001,13);assert(c.pwm.on);
    assert(c.SetIdle(d.io(),0,18000,13));c.Tick(d.io(),999999,13);assert(c.pwm.on);
    const auto count=d.writes;
    assert(!c.SetOn(d.io(),false,61,1000000,13));
    assert(!c.SetOn(d.io(),true,1,1000000,13));
    assert(!c.SetIdle(d.io(),86401,1000000,13) && d.writes==count);
    d.registers.cpuDuty=0;
    const auto activity=c.idle.lastActivityMs;
    assert(!c.SetOn(d.io(),true,0,1000000,14));
    assert(!c.SetIdle(d.io(),0,1000000,14) && c.idle.lastActivityMs==activity && d.writes==count);
}
