#include "native_tsc_observe.h"
#include "vfs.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
using namespace brook;
namespace {int scenario=0;Vnode marker{};uint8_t gate=0x13;bool output=true,running=false;unsigned loaded=0,countdown=0,controls=0,tscs=0;}
namespace brook {
extern "C" Vnode* VfsOpen(const char* path,int){assert(!std::strcmp(path,"/etc/native-tsc-observe"));return scenario==0?nullptr:&marker;}
extern "C" void VfsClose(Vnode*){}
uint32_t SmpCurrentCpuIndex(){return scenario==1?1:0;}
void NativeTscTestCpuid(uint32_t leaf,uint32_t& a,uint32_t& b,uint32_t& c,uint32_t& d){
    a=b=c=d=0;switch(leaf){case 0:a=1;break;case 1:d=1u<<4;c=scenario==2?1u<<31:0;break;
    case 0x80000000:a=0x80000007;break;case 0x80000001:d=1u<<27;break;case 0x80000007:d=1u<<8;break;default:assert(false);}}
uint8_t NativeTscTestReadGate(){if(running && scenario!=3 && ++countdown>=2){output=true;running=false;}
    return uint8_t((gate&~0x20u)|(output?0x20:0));}
void NativeTscTestWriteGate(uint8_t v){if((v&1) && !(gate&1) && loaded==2 && !output && !(v&2)){running=true;countdown=0;}gate=v;}
void NativeTscTestWriteControl(uint8_t v){assert(v==0xb0 && !(gate&3));++controls;loaded=0;output=false;running=false;}
void NativeTscTestWriteCounter(uint8_t v){assert(loaded<2 && v==(loaded?0x74:0x85));++loaded;}
bool NativeTscTestRead(uint64_t& v){++tscs;v=100+uint64_t(tscs)*50000000;return true;}
void NativeTscTestPause(){}
extern "C" void SerialPrintf(const char*,...){}
}
int main(){for(int i=0;i<5;++i){const auto pid=fork();assert(pid>=0);if(!pid){scenario=i;NativeTscObserveConfigure();const auto& o=NativeTscObserveRead();
    if(i==0)assert(!o.requested && !o.attempted && !controls);
    if(i==1)assert(o.requested && o.attempted && !o.capable && !controls && !std::strcmp(o.reason,"not BSP"));
    if(i==2)assert(o.attempted && !o.capable && !controls);
    if(i==3)assert(o.capable && !o.measured && !o.consistent && !o.khz && (gate&~0x20u)==0x13);
    if(i==4){assert(o.capable && o.measured && o.consistent && o.khz>1999000 && o.khz<2001000 && controls==5);
        for(auto sample:o.samples)assert(sample==50000000);}
    const auto before=controls;NativeTscObserveConfigure();assert(controls==before);_exit(0);}
    int status=0;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);}return 0;}
