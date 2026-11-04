#include "intel_vtd_observe.h"
#include "hda_dell_inventory.h"
#include "acpi.h"
#include "vfs.h"
#include "../test_intel_dmar_gpu/dell_fixture.h"
#include <cassert>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
using namespace brook;
static int scenario;static unsigned reads;static Vnode marker;
namespace brook {
extern "C" void SerialPrintf(const char*,...){}
extern "C" void SerialPuts(const char*){}
extern "C" Vnode* VfsOpen(const char* p,int){return !std::strcmp(p,"/etc/dell-gpu-vtd-probe")?&marker:nullptr;}
void VfsRelease(Vnode*){}
int VfsStat(Vnode*,VnodeStat* s){*s={};s->size=scenario==1?3:2;return 0;}
extern "C" int VfsRead(Vnode*,void* out,uint64_t n,uint64_t*){if(n!=2)return -1;std::memcpy(out,"1\n",2);return 2;}
uint32_t SmpCurrentCpuIndex(){return scenario==6?1:0;}
bool AcpiUniqueDmarCopy(void* out,uint32_t capacity,uint32_t& size){size=0;if(scenario==2 || scenario==12 || capacity<sizeof(dellDmarFixture))return false;std::memcpy(out,dellDmarFixture,sizeof(dellDmarFixture));size=sizeof(dellDmarFixture);return true;}
uint32_t PciBootInventoryCount(){return 16;}
uint32_t PciBootInventorySeen(){return scenario==4 || scenario==13?17:16;}
bool PciBootInventoryRead(uint32_t index,PciBootEntry* out){
    if(index>=16)return false;*out={};const auto& e=HdaDellPciReferenceEntries[index];auto& d=out->device;
    d.bus=e.bus;d.dev=e.dev;d.fn=e.fn;d.vendorId=e.vendor;d.deviceId=e.device;
    d.classCode=e.cls;d.subclass=e.sub;d.progIf=e.prog;out->headerType=e.header;
    for(unsigned i=0;i<6;++i)d.bar[i]=e.bars[i];
    if(index==0 || index==1){out->revision=9;out->subsystemVendor=0x1028;out->subsystemDevice=scenario==3?0x0666:0x0665;}
    return true;
}
bool AcpiClassifyBootRange(uint64_t address,uint32_t bytes,BootResourceClassification& c){
    assert(address==DellGpuVtdPage && bytes==4096);c={scenario==8?3u:scenario==9?16u:scenario>=10?0u:2u,scenario==5 || scenario>=10};return scenario!=11;
}
bool VmmDellGpuVtdCriticalRead(DellGpuVtdStatus& s){++reads;if(scenario==7)return false;s={0x10,0};return true;}
}
static void run(int n){scenario=n;DellGpuVtdObserveConfigure();const auto& r=DellGpuVtdObserveRead();
    if(n==0 || n==9 || n==10){assert(r.requested && r.topology && r.inventory && r.bootMap && r.sampled && reads==1 && r.status.version==0x10);}
    else {assert(!r.sampled && reads==(n==7?1u:0u));}
    if(n==5)assert(r.bootMapClassified && r.bootMapUnreported && r.bootMapTypes==2);
    if(n==8)assert(r.bootMapClassified && !r.bootMapUnreported && r.bootMapTypes==3);
    if(n==10)assert(r.auditedDmarHole);
    DellGpuVtdObserveConfigure();assert(reads==(n==0 || n==7 || n==9 || n==10?1u:0u));
}
int main(){for(int n=0;n<=13;++n){pid_t p=fork();assert(p>=0);if(!p){run(n);_exit(0);}int status=0;assert(waitpid(p,&status,0)==p && WIFEXITED(status) && WEXITSTATUS(status)==0);}}
