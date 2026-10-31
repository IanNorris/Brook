#include "dell_i2c_probe.h"
#include "dell_audio_probe.h"
#include "acpi.h"
#include "pci.h"
#include "vfs.h"
#include <cassert>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
using namespace brook;
static int scenario=0;static unsigned reads=0;static Vnode marker;
namespace brook {
extern "C" Vnode* VfsOpen(const char* path,int){return !std::strcmp(path,"/etc/dell-i2c-id-probe")?&marker:nullptr;}
void VfsRelease(Vnode*){}
int VfsStat(Vnode*,VnodeStat* s){*s={};s->size=scenario==1?3:2;return 0;}
extern "C" int VfsRead(Vnode*,void* out,uint64_t n,uint64_t*){if(n!=2)return -1;std::memcpy(out,"1\n",2);return 2;}
bool DellAudioProbeRead(DellAudioFirmwareSnapshot& s,const char*& reason){
    reason="ok";s={};s.pchGeneration=2;s.codecSelection=1;s.i2cMode=1;
    s.i2cBase0=DellI2c0Page;s.i2cBase1=DellI2c0PrivatePage;return scenario!=5;
}
uint32_t PciBootInventoryCount(){return 2;}
uint32_t PciBootInventorySeen(){return scenario==3?3:2;}
bool PciBootInventoryRead(uint32_t i,PciBootEntry* e){
    *e={};auto& d=e->device;d.vendorId=0x8086;
    if(i==0){d.dev=2;d.deviceId=scenario==2?0x1234:0x1616;e->subsystemVendor=0x1028;e->subsystemDevice=0x0665;
        if(scenario==4)d.bar[0]=uint32_t(DellI2c0Page);}
    else{d.dev=31;d.fn=0;d.deviceId=0x9cc3;}return true;
}
bool AcpiClassifyBootRange(uint64_t,uint32_t,BootResourceClassification& c){
    c={};c.unreported=scenario!=6;if(scenario==6)c.types=1;return true;
}
bool VmmDellI2cIdCriticalRead(uint32_t& value){++reads;value=DellI2cComponentValue;return true;}
}
static void run(int n){
    scenario=n;uint32_t value=0;const auto result=DellI2cIdProbe(value);const auto status=DellI2cIdProbeStatus();
    if(n==1){assert(result==DellI2cIdResult::Disabled && !status.started && reads==0);return;}
    if(n){assert(result==DellI2cIdResult::Refused && status.completed && status.result==result && reads==0);return;}
    assert(result==DellI2cIdResult::Identified && value==DellI2cComponentValue && status.completed && status.value==value && reads==1);
    assert(DellI2cIdProbe(value)==DellI2cIdResult::AlreadyUsed && reads==1);
}
int main(){for(int n=0;n<=6;++n){const pid_t pid=fork();assert(pid>=0);if(!pid){run(n);_exit(0);}int status=0;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);}}
