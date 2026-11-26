#include <cassert>
#include <cstring>
#include <string>
#include "platform_power.h"
#include "pci_quiesce.h"
#include <vector>
namespace {brook::AcpiResetInfo firmware;std::string mode;}
namespace brook {
const AcpiResetInfo& AcpiGetResetInfo(){return firmware;}
AcpiS5Info AcpiGetS5Info(){return {};}
uint32_t FwCfgReadFile(const char*,void* out,uint32_t capacity){unsigned n=mode.size()<capacity?mode.size():capacity;std::memcpy(out,mode.data(),n);return n;}
extern "C" void SerialPrintf(const char*,...){}
}
struct Trace {
    unsigned count=0,width=0;uint16_t port=0,value=0;bool accept=true;
    static bool byte(void* p,uint16_t port,uint8_t value){auto& t=*static_cast<Trace*>(p);++t.count;t.width=8;t.port=port;t.value=value;return t.accept;}
    static bool word(void* p,uint16_t port,uint16_t value){auto& t=*static_cast<Trace*>(p);++t.count;t.width=16;t.port=port;t.value=value;return t.accept;}
    static bool efi(void* p,uint64_t entry){auto& t=*static_cast<Trace*>(p);++t.count;t.width=64;t.value=entry;return t.accept;}
    brook::PowerIo io(){return {this,byte,word};}
};
namespace quiesce {
std::vector<int> order;
void a(){order.push_back(1);} void b(){order.push_back(2);}
struct Config {
    uint8_t space[256]{};std::vector<std::pair<uint8_t,uint16_t>> writes;
    uint8_t read8(uint8_t o){return space[o];}
    uint16_t read16(uint8_t o){return uint16_t(space[o]|space[o+1]<<8);}
    void write16(uint8_t o,uint16_t v){writes.push_back({o,v});space[o]=uint8_t(v);space[o+1]=uint8_t(v>>8);}
    void set16(uint8_t o,uint16_t v){space[o]=uint8_t(v);space[o+1]=uint8_t(v>>8);}
};
void Test(){
    using namespace brook;
    PowerQuiesceList list;
    assert(!list.Add(nullptr) && list.Add(a) && list.Add(b) && list.Add(a) && list.count==2);
    assert(list.Run()==2 && order==std::vector<int>({2,1})); // reverse registration order
    for(unsigned i=list.count;i<PowerQuiesceList::Capacity;++i)assert(list.Add(reinterpret_cast<PowerQuiesceFn>(uintptr_t(0x1000+i))));
    assert(!list.Add(reinterpret_cast<PowerQuiesceFn>(uintptr_t(0x2000))));
    // Bus master on, MSI enabled at 0x50, MSI-X enabled at 0x70, a PM cap first.
    Config c;c.set16(0,0x8086);c.set16(4,0x0006);c.set16(6,0x0010);c.space[0x34]=0x40;
    c.space[0x40]=0x01;c.space[0x41]=0x50;c.space[0x50]=0x05;c.space[0x51]=0x70;c.set16(0x52,0x0081);
    c.space[0x70]=0x11;c.space[0x71]=0x00;c.set16(0x72,0x8003);
    assert(PciQuiesceForReset(c)==3);
    assert(c.read16(4)==0x0402 && c.read16(0x52)==0x0080 && c.read16(0x72)==0x0003);
    assert(PciQuiesceForReset(c)==0 && c.writes.size()==3); // idempotent
    Config gone;gone.set16(0,0xffff);assert(PciQuiesceForReset(gone)==0 && gone.writes.empty());
    Config loop;loop.set16(0,0x10de);loop.set16(4,0x0406);loop.set16(6,0x0010);loop.space[0x34]=0x40;
    loop.space[0x40]=0x09;loop.space[0x41]=0x40; // self-referencing list stays bounded
    assert(PciQuiesceForReset(loop)==1 && loop.read16(4)==0x0402);
}
}
struct SleepTrace {
    struct Write {uint16_t port,value;};
    Write writes[4]{};
    unsigned count=0;
    uint16_t oldA=0x0803,oldB=0x0010;
    bool reads=true;
    static bool read(void* context,uint16_t port,uint16_t& value) {
        const auto& t=*static_cast<SleepTrace*>(context);
        value=port==0x404?t.oldA:t.oldB;
        return t.reads;
    }
    static bool write(void* context,uint16_t port,uint16_t value) {
        auto& t=*static_cast<SleepTrace*>(context);
        assert(t.count<4);t.writes[t.count++]={port,value};return true;
    }
    brook::PowerIo io(){return {this,nullptr,write,nullptr,read};}
};

void TestedSleep()
{
    using namespace brook;
    PlatformPowerPlan plan{};
    plan.acpiS5={{0x404,0x504},5,2,"fixture"};
    SleepTrace trace;
    assert(plan.CanPowerOff() && !strcmp(plan.PowerOffMethod(),"acpi-s5"));
    assert(plan.Issue(false,trace.io()) && trace.count==4);
    assert(trace.writes[0].port==0x404 && trace.writes[0].value==0x1403);
    assert(trace.writes[1].port==0x504 && trace.writes[1].value==0x0810);
    assert(trace.writes[2].port==0x404 && trace.writes[2].value==0x3403);
    assert(trace.writes[3].port==0x504 && trace.writes[3].value==0x2810);
    trace={};trace.oldB=0xffff;
    assert(!plan.Issue(false,trace.io()) && !trace.count);
    trace={};trace.oldA=0;
    assert(!plan.Issue(false,trace.io()) && !trace.count);
    trace={};trace.reads=false;
    assert(!plan.Issue(false,trace.io()) && !trace.count);
    trace={};plan.acpiS5.controls.pm1b=0;
    assert(plan.Issue(false,trace.io()) && trace.count==2 && trace.writes[1].value==0x3403);
    trace={};plan.acpiS5.typeA=8;
    assert(!plan.Issue(false,trace.io()) && !trace.count);
    plan.acpiS5.typeA=5;plan.efiEntry=0x9876;
    Trace efi;
    assert(plan.Issue(false,{&efi,Trace::byte,Trace::word,Trace::efi}) &&
           efi.count==1 && efi.width==64 && efi.value==0x9876);
}

int main(){
    using namespace brook;Trace trace;
    quiesce::Test();
    TestedSleep();
    PlatformPowerConfigure();auto plan=PlatformPowerGetPlan();assert(!plan.CanReset() && !plan.CanPowerOff());
    assert(!plan.Issue(true,trace.io()) && !plan.Issue(false,trace.io()) && !trace.count);
    firmware={true,0xb2,0xa5,"fixture advertised register"};PlatformPowerConfigure();plan=PlatformPowerGetPlan();
    assert(plan.CanReset() && !plan.CanPowerOff() && !strcmp(plan.ResetMethod(),"acpi-io8"));
    assert(plan.Issue(true,trace.io()) && trace.count==1 && trace.width==8 && trace.port==0xb2 && trace.value==0xa5);
    mode="q35-reset";PlatformPowerConfigure();plan=PlatformPowerGetPlan();trace={};trace.accept=false;
    assert(!plan.Issue(true,trace.io()) && trace.count==1 && trace.port==0xb2); // no fallback after firmware write fails
    trace={};assert(plan.Issue(false,trace.io()) && trace.count==1 && trace.width==16 && trace.port==0x604 && trace.value==0x2000);
    firmware={};PlatformPowerConfigure();plan=PlatformPowerGetPlan();trace={};
    assert(plan.CanReset() && plan.CanPowerOff() && plan.Issue(true,trace.io()) && trace.port==0xcf9 && trace.value==6);
    firmware={true,0xcf9,15,"firmware supports reset"};
    mode="q35-stop";PlatformPowerConfigure();plan=PlatformPowerGetPlan();trace={};
    assert(!plan.CanReset() && plan.CanPowerOff() && !plan.Issue(true,trace.io()) && !trace.count);
    firmware={};mode="q35-reset-extra";PlatformPowerConfigure();plan=PlatformPowerGetPlan();trace={};
    assert(!plan.CanReset() && !plan.CanPowerOff() && !plan.Issue(false,trace.io()) && !trace.count);
    firmware={true,0xcf9,6,"fixture"};mode.clear();PlatformPowerConfigure();plan=PlatformPowerGetPlan();
    assert(!plan.Issue(true,{nullptr,nullptr,nullptr}));
    trace={};plan.efiEntry=0x1234;
    assert(plan.CanPowerOff() && plan.Issue(false,{&trace,Trace::byte,Trace::word,Trace::efi}) && trace.width==64 && trace.value==0x1234);
    trace={};assert(!plan.Issue(false,trace.io()) && !trace.count); // no speculative fallback
    assert(plan.CanForceEfiRestart());
    assert(plan.IssueForcedRestart({&trace, Trace::byte, Trace::word, nullptr, nullptr, Trace::efi}));
    assert(trace.count==1 && trace.width==64 && trace.value==0x1234);
    trace={};
    assert(plan.IssueForcedRestart(trace.io()) && trace.count==1 && trace.port==0xcf9);
    trace={};
    plan.resetDisabledByLaunch=true;
    assert(!plan.CanForceEfiRestart() && !plan.IssueForcedRestart(trace.io()) && !trace.count);

    // EFI power-off handoff: version 5 with cache and MTRR fingerprints, with or
    // without the appended timing extension. Never an exact-version match.
    BootProtocol boot{};boot.magic=BootProtocolMagic;boot.version=5;
    boot.efiRuntimeCacheValid=1;boot.efiRuntimeMtrrValid=1;boot.memoryMapCount=64;
    assert(PlatformPowerEfiHandoffAccepted(&boot,true));              // older loader, marker 0
    boot.extensions=BootExtensionTimingMagic;
    assert(PlatformPowerEfiHandoffAccepted(&boot,true));              // timing loader
    assert(!PlatformPowerEfiHandoffAccepted(&boot,false));
    boot.version=6;assert(PlatformPowerEfiHandoffAccepted(&boot,true)); // forward compatible
    boot.version=4;assert(!PlatformPowerEfiHandoffAccepted(&boot,true));
    boot.version=5;boot.efiRuntimeMtrrValid=0;assert(!PlatformPowerEfiHandoffAccepted(&boot,true));
    boot.efiRuntimeMtrrValid=1;boot.memoryMapCount=4097;assert(!PlatformPowerEfiHandoffAccepted(&boot,true));
    assert(!PlatformPowerEfiHandoffAccepted(nullptr,true));
}
