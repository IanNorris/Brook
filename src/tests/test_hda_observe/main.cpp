#include "hda_observe.h"
#include "hda_dell_inventory.h"
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
#include "../test_hda_dell_tone_integration/fake_hda_chip.h"

using namespace brook;
namespace {
int scenario=0;uint16_t cmd=0;unsigned writes=0,allocs=0,frees=0,pmmFrees=0;
void* page=nullptr;Vnode marker{};
Chip chip{};unsigned toneAllocations=0,toneFrees=0,toneFlushes=0;uint64_t fakeTicks=0,toneFlushMask[4]{};void* tonePages[4]{};
constexpr uint64_t bar=0xf7418000,ram=0x12345000;
}
bool CpuObserveMemoryTypes(uint64_t,uint64_t,CpuMemoryTypeObservation& out){out={};out.patSlot0=6;out.page0.type=0;return scenario!=7;}
namespace brook {
extern "C" Vnode* VfsOpen(const char* path,int){
 if(std::strcmp(path,"/etc/dell-hda-observe")==0)return scenario==19?nullptr:&marker;
 if(std::strcmp(path,"/etc/dell-hda-muted-dma")==0 && (scenario==16 || scenario==17 || (scenario>=20 && scenario<=25) || (scenario>=27 && scenario<=36)))return &marker;
 if(std::strcmp(path,"/etc/dell-hda-quiet-tone")==0 && (scenario==17 || scenario==18 || scenario==26 || scenario==37))return &marker;
 if(std::strcmp(path,"/etc/dell-hda-mixer-format-probe")==0 && (scenario==36 || scenario==37))return &marker;
 return nullptr;}
extern "C" void VfsClose(Vnode*){}
uint32_t SmpCurrentCpuIndex(){return 0;}
uint32_t PciBootInventoryCount(){return scenario>=20?16:scenario>=9 && scenario<=15?2:1;}
uint32_t PciBootInventorySeen(){return PciBootInventoryCount()+(scenario==15?1:0);}
bool PciBootInventoryRead(uint32_t i,PciBootEntry* out){if(i>=PciBootInventoryCount())return false;*out={};auto& d=out->device;
 if(scenario>=20){const auto& r=HdaDellPciReferenceEntries[i];d.bus=r.bus;d.dev=r.dev;d.fn=r.fn;d.vendorId=r.vendor;d.deviceId=r.device;
  d.classCode=r.cls;d.subclass=r.sub;d.progIf=r.prog;out->headerType=r.header;for(unsigned b=0;b<6;++b)d.bar[b]=r.bars[b];
  if(scenario==27 && i==0)d.bar[0]=0x1000;
  if(d.bus==0 && d.dev==0x1b && d.fn==0){out->subsystemVendor=0x1028;out->subsystemDevice=0x0665;}return true;}
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
extern "C" void PciConfigWrite32(uint8_t,uint8_t,uint8_t,uint8_t o,uint32_t v){assert(o==4 && (v==0 || v==2 || (scenario>=20 && v==6)));++writes;
 if(scenario>=20 && v==6){assert(toneFlushes>=4*64);for(auto mask:toneFlushMask)assert(mask==~uint64_t(0));chip.published=true;}
 if((v==2 && scenario==3) || (v==0 && scenario==6))return;cmd=uint16_t(v);}
bool VmmInspectKernelAliases(uint64_t p,KernelAliasMapping& a,KernelAliasMapping& b){a={true,21,0,p};b=a;return true;}
VirtualAddress VmmAllocPages(uint64_t n,uint64_t,MemTag,uint16_t){assert(n==1);++allocs;if(scenario==4)return VirtualAddress{};return VirtualAddress(reinterpret_cast<uint64_t>(page));}
uint64_t VmmDetachPage(PageTable,VirtualAddress){return ram|1;}
PhysicalAddress VmmLeafPhysical(uint64_t){return PhysicalAddress(ram);}
void PmmFreePage(PhysicalAddress p){if(p.raw()==ram){++pmmFrees;return;}assert(scenario>=20 && !(cmd&4));
 if(chip.published)assert(chip.read32(Chip::base+0x18)==0 && chip.read32(Chip::base+0x1c)==0 && !(chip.read16(Chip::base)&2));
 assert(munmap(reinterpret_cast<void*>(p.raw()),4096)==0);++toneFrees;}
PhysicalAddress PmmAllocPage(MemTag,uint16_t){if(scenario<20 || (scenario==22 && toneAllocations==2))return PhysicalAddress{};
 void* p=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);assert(p!=MAP_FAILED && toneAllocations<4);tonePages[toneAllocations++]=p;return PhysicalAddress(reinterpret_cast<uint64_t>(p));}
// Mirrors the real 4 KiB-only translator's failure on a huge direct-map leaf.
PhysicalAddress VmmVirtToPhys(PageTable,VirtualAddress){return PhysicalAddress{};}
bool VmmMapPage(PageTable,VirtualAddress,PhysicalAddress p,uint64_t,MemTag,uint16_t){assert(p.raw()==bar);if(scenario==2)return false;
 if(scenario==3)assert(mprotect(page,4096,PROT_NONE)==0);return true;}
void VmmFreePages(VirtualAddress,uint64_t){++frees;}
uint64_t ApicTickCount(){return scenario>=20 && scenario!=25?++fakeTicks:0;}
uint8_t HdaObserveTestRead8(uint32_t o){if(scenario>=20){assert(cmd&2);return chip.read8(o);}return *reinterpret_cast<volatile uint8_t*>(static_cast<uint8_t*>(page)+o);}
uint16_t HdaObserveTestRead16(uint32_t o){if(scenario>=20){assert(cmd&2);return chip.read16(o);}return *reinterpret_cast<volatile uint16_t*>(static_cast<uint8_t*>(page)+o);}
uint32_t HdaObserveTestRead32(uint32_t o){if(scenario>=20){assert(cmd&2);return chip.read32(o);}return *reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(page)+o);}
void HdaObserveTestWrite8(uint32_t o,uint8_t v){if(scenario>=20){assert(cmd&2);chip.write8(o,v);}else *reinterpret_cast<volatile uint8_t*>(static_cast<uint8_t*>(page)+o)=v;}
void HdaObserveTestWrite16(uint32_t o,uint16_t v){if(scenario>=20){assert(cmd&2);chip.write16(o,v);}else *reinterpret_cast<volatile uint16_t*>(static_cast<uint8_t*>(page)+o)=v;}
void HdaObserveTestWrite32(uint32_t o,uint32_t v){if(scenario>=20){assert(cmd&2);chip.write32(o,v);}else *reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(page)+o)=v;}
bool HdaObserveTestFlushInfo(uint32_t& line){line=scenario==33?0:64;return scenario!=31;}
void HdaObserveTestFlushLine(void* p){const auto address=reinterpret_cast<uintptr_t>(p);bool found=false;
 for(unsigned i=0;i<4;++i){const auto base=reinterpret_cast<uintptr_t>(tonePages[i]);if(base && address>=base && address<base+4096){
  assert(!((address-base)&63));toneFlushMask[i]|=uint64_t(1)<<((address-base)/64);found=true;break;}}
 assert(found);++toneFlushes;}
extern "C" void SerialPrintf(const char*,...){}
}
static void one(int which){
 scenario=which;page=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(page!=MAP_FAILED);
 auto* p=static_cast<unsigned char*>(page);p[1]=0x21;p[3]=1; // valid GCAP and version, GCTL remains reset
 if(which>=20){chip.snapshotMode=true;chip.mem[0]=1;chip.mem[1]=0x44;chip.mem[3]=1;chip.mem[8]=1;chip.mem[0x0e]=1;
  if(which==21)chip.mem[0x70]=1;if(which==23)chip.dropBdlClear=true;if(which==24)chip.dropStop=true;
  if(which==28)chip.mem[Chip::base+2]=0x10;if(which==29)chip.corruptNewFormat=true;
  if(which==30 || which==32){chip.roPriority=true;chip.mem[Chip::base+2]=4;}if(which==32)chip.dropRetireReset=true;
  if(which==34)chip.failStatusAfter=3;if(which==35)chip.reportEndpoint=true;if(which==36){chip.format=0x31;chip.mem[Chip::base+0x10]=0xc0;}}
 BootProtocol boot{};MemoryDescriptor descriptor{};
 if(which==8){descriptor.physicalStart=bar;descriptor.pageCount=1;descriptor.type=MemoryType::Free;boot.memoryMap=&descriptor;boot.memoryMapCount=1;}
 HdaObserveConfigure(&boot);const auto& o=HdaObserveRead();
 if(which==19){assert(!o.requested && !o.attempted && !writes && !allocs);munmap(page,4096);return;}
 assert(o.requested && o.attempted);
 if(which==1 || which==5 || which==7 || which==8 || (which>=9 && which<=13) || which==15){assert(!writes && !allocs && !pmmFrees);}
 if(which==2){assert(!writes && allocs==1 && frees==1 && pmmFrees==1 && !o.mapped);}
 if(which==3){assert(writes==2 && cmd==0 && o.session.result==HdaSessionResult::DecodeEnableFailed);}
 if(which==4){assert(!writes && allocs==1 && !frees && !pmmFrees);}
 if(which==6){assert(writes==2 && cmd==2 && o.session.result==HdaSessionResult::RestoreFailed);}
 if(which==0 || which==14){assert(writes==2 && cmd==0 && o.mapped && pmmFrees==1 && !frees && o.session.result==HdaSessionResult::InspectionFailed && o.session.probe==HdaProbeResult::ControllerReset);}
 if(which>=16 && which<=18){assert(o.toneRequested && !o.toneAttempted && writes==2 && cmd==0);
  assert(o.toneAudible==(which==17 || which==18));assert(std::strcmp(o.toneReason,which==17?"conflicting markers":"observation incomplete")==0);}
 if(which==20){assert(o.toneRequested && o.toneAttempted && !o.toneAudible && o.toneFinished && !o.toneQuarantined &&
  toneAllocations==4 && toneFrees==4 && o.toneAdmission==31 && o.tonePageStatus==3 && cmd==0 && !chip.sawQuietUnmute && chip.selection==0 && chip.format==0x4011);}
 if(which==21){assert(o.toneAttempted && !o.toneFinished && !o.toneQuarantined && toneAllocations==4 && toneFrees==4 && cmd==0 && !chip.published);}
 if(which==22){assert(o.toneAttempted && !o.toneFinished && !o.toneQuarantined && toneAllocations==2 && toneFrees==2 && o.tonePageStatus==4 && !o.tonePage && cmd==0);}
 if(which==23){assert(o.toneAttempted && !o.toneFinished && o.toneQuarantined && toneAllocations==4 && !toneFrees && cmd==2 && chip.pin[0]==0x80 && chip.pin[1]==0x80);}
 if(which==24){assert(o.toneAttempted && !o.toneFinished && o.toneQuarantined && toneAllocations==4 && !toneFrees && cmd==6 && chip.pin[0]==0x80 && chip.pin[1]==0x80);}
 if(which==25){assert(o.toneAttempted && !o.toneFinished && !o.toneQuarantined && !toneAllocations && cmd==0 && std::strcmp(o.toneReason,"clock unavailable")==0);}
 if(which==26){assert(o.toneRequested && o.toneAttempted && o.toneAudible && o.toneFinished && !o.toneQuarantined &&
  toneAllocations==4 && toneFrees==4 && cmd==0 && chip.sawQuietUnmute);}
 if(which==27){assert(o.toneRequested && o.toneAttempted && !o.toneFinished && !o.toneQuarantined &&
  o.toneAdmission==30 && !toneAllocations && !toneFrees && cmd==0 && !chip.published);}
 if(which==28){assert(o.toneAttempted && o.toneQuarantined && o.toneResult==4 && o.tonePrepareStage==6 &&
  o.toneStreamStage==2 && o.toneStreamDetail==0x100000 && !o.toneIciFailed && o.toneCommand==2 && !chip.published);}
 if(which==29){assert(o.toneAttempted && o.toneQuarantined && o.toneResult==4 && o.tonePrepareStage==3 &&
  o.tonePrepareValue==0x10 && !o.toneIciFailed && o.toneCommand==2 && !chip.published);}
 if(which==30){assert(o.toneFinished && !o.toneQuarantined && toneFlushes==8*64 && toneFrees==4 && cmd==0 && chip.mem[Chip::base+2]==4);}
 if(which==31){assert(o.toneAttempted && !o.toneFinished && !o.toneQuarantined && !toneAllocations && cmd==0 && std::strcmp(o.toneReason,"cache flush unavailable")==0);}
 if(which==32){assert(o.toneAttempted && !o.toneFinished && o.toneQuarantined && !toneFrees && cmd==2 && chip.pin[0]==0x80 && chip.pin[1]==0x80);}
 if(which==33){assert(o.toneAttempted && !o.toneFinished && !toneAllocations && !toneFlushes && cmd==0 && std::strcmp(o.toneReason,"cache flush unavailable")==0);}
 if(which==34){assert(o.toneAttempted && !o.toneAudible && o.toneQuarantined && o.toneResult==8 &&
  o.tonePlayReason==3 && o.tonePlayLoops==1 && o.tonePlayProgress==0 && o.toneProgressReason==3 && o.toneProgressStatus==8 && cmd==2);}
 if(which==35){assert(o.toneFinished && !o.toneAudible && !o.toneQuarantined && chip.sawEndpoint && o.tonePlayElapsed>=2000 && cmd==0);}
 if(which==36){assert(o.toneFinished && !o.toneAudible && !o.toneQuarantined && o.tonePlayElapsed>=2000 && cmd==0 &&
  chip.runConverterFormat==0x4011 && chip.runStreamFormat==0x4011 && chip.format==0x31 && o.toneFifoSampled && o.toneFifoSize==0xc0 && !chip.sawQuietUnmute);}
 if(which==37){assert(o.toneRequested && !o.toneAttempted && !chip.published && cmd==0 &&
  std::strcmp(o.toneReason,"mixer format probe requires mute")==0);}
 const auto oldWrites=writes;HdaObserveConfigure(&boot);assert(writes==oldWrites);munmap(page,4096);
}
int main(){for(int i=0;i<=37;++i){pid_t pid=fork();assert(pid>=0);if(!pid){one(i);_exit(0);}int status=0;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);}return 0;}
