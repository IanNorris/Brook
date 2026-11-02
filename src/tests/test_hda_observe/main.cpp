#include "hda_observe.h"
#include "vfs.h"
#include "pci.h"
#include "cpu.h"
#include "memory/virtual_memory.h"
#include "memory/physical_memory.h"
#include <cassert>
#include <cstring>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace brook;
namespace {
int scenario=0;uint16_t cmd=0;unsigned writes=0,allocs=0,frees=0,pmmFrees=0;
void* page=nullptr;Vnode marker{};
constexpr uint64_t bar=0xf7418000,ram=0x12345000;
}
bool CpuObserveMemoryTypes(uint64_t,uint64_t,CpuMemoryTypeObservation& out){out={};out.patSlot0=6;out.page0.type=0;return scenario!=7;}
namespace brook {
extern "C" Vnode* VfsOpen(const char*,int){return &marker;}
extern "C" void VfsClose(Vnode*){}
uint32_t SmpCurrentCpuIndex(){return 0;}
uint32_t PciBootInventoryCount(){return scenario>=9?2:1;}
uint32_t PciBootInventorySeen(){return PciBootInventoryCount()+(scenario==15?1:0);}
bool PciBootInventoryRead(uint32_t i,PciBootEntry* out){if(i>=PciBootInventoryCount())return false;*out={};auto& d=out->device;
 if(i==1){d.dev=3;d.bar[0]=uint32_t(bar);out->headerType=scenario==10 || scenario==14?1:0;
  if(scenario==11)d.bar[0]=0xffffffff;
  if(scenario==12)d.bar[0]=0xf7000006;
  if(scenario==13){d.bar[0]=0xf7000004;d.bar[1]=0xffffffff;}
  if(scenario==14){d.bar[0]=0xf7000000;d.bar[2]=uint32_t(bar);}
  if(scenario==15)d.bar[0]=0xf7000000;
  return true;}
 d.bus=0;d.dev=0x1b;d.fn=0;d.vendorId=0x8086;d.deviceId=scenario==1?0x1111:0x9ca0;d.classCode=4;d.subclass=3;d.bar[0]=uint32_t(bar)|4;
 out->subsystemVendor=0x1028;out->subsystemDevice=0x0665;return true;}
extern "C" uint32_t PciConfigRead32(uint8_t,uint8_t,uint8_t,uint8_t o){switch(o){case 0:return 0x9ca08086;case 8:return 0x04030003;case 0x2c:return 0x06651028;case 0x10:return uint32_t(bar)|4;case 0x14:return 0;default:return 0;}}
extern "C" uint16_t PciConfigRead16(uint8_t,uint8_t,uint8_t,uint8_t o){if(o==4)return cmd;if(o==6)return 0x10;if(o==0x44)return 0;return 0xffff;}
extern "C" uint8_t PciConfigRead8(uint8_t,uint8_t,uint8_t,uint8_t o){if(o==0x0e)return 0;if(o==0x34)return 0x40;if(o==0x40)return scenario==5?5:1;if(o==0x41)return scenario==5?0x40:0;return 0;}
extern "C" void PciConfigWrite32(uint8_t,uint8_t,uint8_t,uint8_t o,uint32_t v){assert(o==4 && (v==0 || v==2));++writes;
 if((v==2 && scenario==3) || (v==0 && scenario==6))return;cmd=uint16_t(v);}
bool VmmInspectKernelAliases(uint64_t p,KernelAliasMapping& a,KernelAliasMapping& b){a={true,21,0,p};b=a;return true;}
VirtualAddress VmmAllocPages(uint64_t n,uint64_t,MemTag,uint16_t){assert(n==1);++allocs;if(scenario==4)return VirtualAddress{};return VirtualAddress(reinterpret_cast<uint64_t>(page));}
uint64_t VmmDetachPage(PageTable,VirtualAddress){return ram|1;}
PhysicalAddress VmmLeafPhysical(uint64_t){return PhysicalAddress(ram);}
void PmmFreePage(PhysicalAddress p){assert(p.raw()==ram && p.raw()!=bar);++pmmFrees;}
bool VmmMapPage(PageTable,VirtualAddress,PhysicalAddress p,uint64_t,MemTag,uint16_t){assert(p.raw()==bar);if(scenario==2)return false;
 if(scenario==3)assert(mprotect(page,4096,PROT_NONE)==0);return true;}
void VmmFreePages(VirtualAddress,uint64_t){++frees;}
uint64_t ApicTickCount(){return 0;}
extern "C" void SerialPrintf(const char*,...){}
}
static void one(int which){
 scenario=which;page=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(page!=MAP_FAILED);
 auto* p=static_cast<unsigned char*>(page);p[1]=0x21;p[3]=1; // valid GCAP and version, GCTL remains reset
 BootProtocol boot{};MemoryDescriptor descriptor{};
 if(which==8){descriptor.physicalStart=bar;descriptor.pageCount=1;descriptor.type=MemoryType::Free;boot.memoryMap=&descriptor;boot.memoryMapCount=1;}
 HdaObserveConfigure(&boot);const auto& o=HdaObserveRead();assert(o.requested && o.attempted);
 if(which==1 || which==5 || which==7 || which==8 || (which>=9 && which<=13) || which==15){assert(!writes && !allocs && !pmmFrees);}
 if(which==2){assert(!writes && allocs==1 && frees==1 && pmmFrees==1 && !o.mapped);}
 if(which==3){assert(writes==2 && cmd==0 && o.session.result==HdaSessionResult::DecodeEnableFailed);}
 if(which==4){assert(!writes && allocs==1 && !frees && !pmmFrees);}
 if(which==6){assert(writes==2 && cmd==2 && o.session.result==HdaSessionResult::RestoreFailed);}
 if(which==0 || which==14){assert(writes==2 && cmd==0 && o.mapped && pmmFrees==1 && !frees && o.session.result==HdaSessionResult::InspectionFailed && o.session.probe==HdaProbeResult::ControllerReset);}
 const auto oldWrites=writes;HdaObserveConfigure(&boot);assert(writes==oldWrites);munmap(page,4096);
}
int main(){for(int i=0;i<=15;++i){pid_t pid=fork();assert(pid>=0);if(!pid){one(i);_exit(0);}int status=0;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);}return 0;}
