#include <cassert>
#include <cstring>
#include <string>
#include "platform_power.h"
namespace {brook::AcpiResetInfo firmware;std::string mode;}
namespace brook {
const AcpiResetInfo& AcpiGetResetInfo(){return firmware;}
uint32_t FwCfgReadFile(const char*,void* out,uint32_t capacity){unsigned n=mode.size()<capacity?mode.size():capacity;std::memcpy(out,mode.data(),n);return n;}
extern "C" void SerialPrintf(const char*,...){}
}
struct Trace {
    unsigned count=0,width=0;uint16_t port=0,value=0;bool accept=true;
    static bool byte(void* p,uint16_t port,uint8_t value){auto& t=*static_cast<Trace*>(p);++t.count;t.width=8;t.port=port;t.value=value;return t.accept;}
    static bool word(void* p,uint16_t port,uint16_t value){auto& t=*static_cast<Trace*>(p);++t.count;t.width=16;t.port=port;t.value=value;return t.accept;}
    brook::PowerIo io(){return {this,byte,word};}
};
int main(){
    using namespace brook;Trace trace;
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
}
