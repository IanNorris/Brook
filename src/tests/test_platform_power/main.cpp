#include <cassert>
#include <cstring>
#include <string>
#include "platform_power.h"
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
