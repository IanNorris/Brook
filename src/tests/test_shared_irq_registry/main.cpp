#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <initializer_list>
#include "../../kernel/src/shared_irq_registry.h"

static void SharedIrqDispatch(unsigned slot);
static void stub0(){SharedIrqDispatch(0);}
static void stub1(){SharedIrqDispatch(1);}
static void stub2(){SharedIrqDispatch(2);}
static void stub3(){SharedIrqDispatch(3);}
using StubFn = void (*)();
static StubFn g_sharedIrqStubs[]={stub0,stub1,stub2,stub3};
static void* installed[256]{};
static uint8_t routed[256]{};
static bool masked[256]{};
static unsigned installs=0,unmasks=0,masks=0,eois=0,callsA=0,callsB=0;
void SerialPrintf(const char*,...){}
[[noreturn]] void KernelPanic(const char*,...){throw std::runtime_error("registration rejected");}
static bool SharedIrqMayInstall(uint8_t vector,void* handler);
void IdtInstallHandler(uint8_t vector,void* handler){if(!SharedIrqMayInstall(vector,handler))KernelPanic("pool conflict");installed[vector]=handler;++installs;}
void IoApicUnmaskIrq(uint8_t irq,uint8_t vector){routed[irq]=vector;masked[irq]=false;++unmasks;}
void IoApicMaskIrq(uint8_t irq){masked[irq]=true;++masks;}
void ApicSendEoi(){++eois;}
#include "../../kernel/src/shared_irq_registration.inc"

static void handlerA(){++callsA;}
static void handlerB(){++callsB;}
static void handlerC(){}
static void handlerD(){}
static void handlerE(){}
static void reset(){
    for(auto& e:g_sharedIrqs)e={};g_sharedIrqCount=0;
    for(auto& e:installed)e=nullptr;
    for(auto& e:routed)e=0;
    for(auto& e:masked)e=false;
    installs=unmasks=masks=eois=callsA=callsB=0;
}
static unsigned failures=0;
#define CHECK(c) do{if(!(c)){std::printf("FAIL line%d: %s\n",__LINE__,#c);++failures;}}while(0)
template<class Fn> static void rejects(Fn action){
    bool rejected=false;
    try{action();}catch(const std::runtime_error&){rejected=true;}
    CHECK(rejected);
}
int main(){
    reset();
    const auto first=IoApicRegisterHandler(11,50,reinterpret_cast<void*>(handlerA));
    const auto second=IoApicRegisterHandler(10,50,reinterpret_cast<void*>(handlerB));
    CHECK(first!=second);
    reinterpret_cast<StubFn>(installed[routed[11]])();
    CHECK(callsA==1 && callsB==0 && eois==1);
    reset();
    IoApicRegisterHandler(11,50,reinterpret_cast<void*>(handlerA));
    IoApicRegisterHandler(11,50,reinterpret_cast<void*>(handlerA));
    reinterpret_cast<StubFn>(installed[routed[11]])();
    CHECK(callsA==1 && eois==1);
    reset();
    const auto original=IoApicRegisterHandler(11,50,reinterpret_cast<void*>(handlerA));
    IoApicUnregisterHandler(11,reinterpret_cast<void*>(handlerA));CHECK(masked[11]);
    const auto restored=IoApicRegisterHandler(11,51,reinterpret_cast<void*>(handlerB));
    CHECK(restored==original && !masked[11] && unmasks==2);
    // Paired duplicate registrations retain one dispatch until the last owner.
    reset();
    IoApicRegisterHandler(11,50,reinterpret_cast<void*>(handlerA));
    IoApicRegisterHandler(11,51,reinterpret_cast<void*>(handlerA));
    CHECK(FindSharedIrq(11)->count==1 && FindSharedIrq(11)->references[0]==2);
    IoApicUnregisterHandler(11,reinterpret_cast<void*>(handlerA));
    CHECK(!masked[11] && masks==0);
    SharedIrqDispatch(0);CHECK(callsA==1 && eois==1);
    IoApicUnregisterHandler(11,reinterpret_cast<void*>(handlerA));
    CHECK(masked[11] && masks==1 && FindSharedIrq(11)->count==0);
    SharedIrqDispatch(0);CHECK(callsA==1 && eois==2);

    // Sharing a line preserves its actual vector and dispatches each callback once.
    reset();
    CHECK(IoApicRegisterHandler(11,50,reinterpret_cast<void*>(handlerA))==50);
    CHECK(IoApicRegisterHandler(11,63,reinterpret_cast<void*>(handlerB))==50);
    CHECK(installs==1 && unmasks==1);
    SharedIrqDispatch(0);CHECK(callsA==1 && callsB==1 && eois==1);
    IoApicUnregisterHandler(11,reinterpret_cast<void*>(handlerA));
    SharedIrqDispatch(0);CHECK(callsA==1 && callsB==2 && eois==2);

    // Empty slots keep their identities; other lines cannot steal their vector.
    reset();
    auto v=IoApicRegisterHandler(11,50,reinterpret_cast<void*>(handlerA));
    IoApicUnregisterHandler(11,reinterpret_cast<void*>(handlerA));
    CHECK(IoApicRegisterHandler(10,v,reinterpret_cast<void*>(handlerB))!=v);
    CHECK(IoApicRegisterHandler(11,63,reinterpret_cast<void*>(handlerA))==v);
    CHECK(g_sharedIrqCount==2 && !masked[11]);
    reinterpret_cast<StubFn>(installed[routed[11]])();CHECK(callsA==1 && !callsB);

    // Fixed exception/timer/input/MSI-X/IPI vectors are never selected.
    for(uint8_t reserved:{uint8_t(0),uint8_t(32),uint8_t(33),uint8_t(44),uint8_t(47),
                          uint8_t(49),uint8_t(252),uint8_t(254),uint8_t(255)}){
        reset();
        IdtInstallHandler(reserved,reinterpret_cast<void*>(handlerE));
        auto actual=IoApicRegisterHandler(11,reserved,reinterpret_cast<void*>(handlerA));
        CHECK(SharedIrqVectorAllowed(actual) && actual!=reserved);
        CHECK(installed[reserved]==reinterpret_cast<void*>(handlerE));
    }
    reset();
    rejects([]{IdtInstallHandler(50,reinterpret_cast<void*>(handlerA));});
    CHECK(!installs);
    IoApicRegisterHandler(11,50,reinterpret_cast<void*>(handlerA));
    rejects([]{IdtInstallHandler(50,reinterpret_cast<void*>(handlerB));});
    CHECK(installs==1);

    // Saturated chain/capacity and invalid input reject before partial changes.
    reset();
    for(auto fn:{handlerA,handlerB,handlerC,handlerD})
        IoApicRegisterHandler(11,50,reinterpret_cast<void*>(fn));
    rejects([]{IoApicRegisterHandler(11,50,reinterpret_cast<void*>(handlerE));});
    CHECK(FindSharedIrq(11)->count==4 && installs==1 && unmasks==1);
    IoApicUnregisterHandler(11,reinterpret_cast<void*>(handlerB));
    CHECK(FindSharedIrq(11)->handlers[1]==handlerC && FindSharedIrq(11)->references[1]==1);
    reset();
    for(uint8_t line=1;line<=4;++line)IoApicRegisterHandler(line,50,reinterpret_cast<void*>(handlerA));
    rejects([]{IoApicRegisterHandler(5,50,reinterpret_cast<void*>(handlerB));});
    CHECK(g_sharedIrqCount==4 && installs==4 && unmasks==4 && !FindSharedIrq(5));
    for(uint8_t line=1;line<=4;++line)reinterpret_cast<StubFn>(installed[routed[line]])();
    CHECK(callsA==4 && eois==4 && callsB==0);
    reset();
    rejects([]{IoApicRegisterHandler(11,50,nullptr);});
    rejects([]{IoApicRegisterHandler(255,50,reinterpret_cast<void*>(handlerA));});
    CHECK(g_sharedIrqCount==0 && !installs && !unmasks);
    IoApicRegisterHandler(11,50,reinterpret_cast<void*>(handlerA));
    FindSharedIrq(11)->references[0]=UINT32_MAX;
    rejects([]{IoApicRegisterHandler(11,50,reinterpret_cast<void*>(handlerA));});
    CHECK(FindSharedIrq(11)->references[0]==UINT32_MAX && installs==1);
    std::printf("shared IRQ registry failures=%u\n",failures);
    return failures?1:0;
}
