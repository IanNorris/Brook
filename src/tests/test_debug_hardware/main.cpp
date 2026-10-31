#include "debug_hardware.h"
#include "bridge_reply.h"
#include "shutdown.h"
#include "fs/compressed_vfs.h"
#include "vfs.h"
#include "pci.h"
#include "display_power.h"
#include "writable_desktop.h"
#include "ahci_readonly.h"
#include "ahci_observe.h"
#include "acpi.h"
#include "dell_audio_probe.h"
#include "memory/virtual_memory.h"
#include "cpu.h"
#include <cassert>
#include <cstring>
#include <string>
std::string reply,dispatched;
unsigned powerCalls=0;std::string powerVerb;
unsigned backlightCalls=0;uint32_t backlightSeconds=0;bool backlightOn=true;
bool audioProbePresent=false;
bool CpuObserveMemoryTypes(uint64_t,uint64_t,brook::CpuMemoryTypeObservation& out){out={};out.patSlot0=6;out.page0.type=0;out.page1.type=0;return true;}
namespace brook {
bool DellAudioProbeRead(DellAudioFirmwareSnapshot& out,const char*& reason){
    reason=audioProbePresent?"ok":"not provisioned";out.i2cBase0=0xfe103000;out.i2cBase1=0xfe104000;return audioProbePresent;
}
uint32_t AcpiInventoryCount(){return 1;}
bool AcpiMmioMapValid(){return true;}
bool AcpiMmioContains(uint64_t address,uint32_t bytes){return address==0xfe103000 && bytes==4096;}
bool AcpiClassifyBootRange(uint64_t,uint32_t,BootResourceClassification& result){result={2,false};return true;}
bool VmmInspectKernelAliases(uint64_t physical,KernelAliasMapping& a,KernelAliasMapping& b){a={true,21,0,physical};b=a;return true;}
bool AcpiInventoryRead(uint32_t index,uint32_t offset,void* out,uint32_t bytes,AcpiSdtHeader& h){
    if(index || offset>1024 || bytes>1024-offset)return false;
    h={};h.length=1024;if(bytes)std::memset(out,0xab,bytes);return true;
}
AppSnapshotBootInfo snapshot;
WritableDesktopInfo writable;
WritableDesktopInfo WritableDesktopRead(){return writable;}
AhciReadOnlyStatus ahciStatus{};
AhciObservation ahciObservation{};
AhciReadOnlyStatus AhciReadOnlyGetStatus(){return ahciStatus;}
const AhciObservation& AhciObserveRead(){return ahciObservation;}
int AhciReadOnlySector(uint64_t lba,void* out){if(lba!=3)return -22;std::memset(out,0x5a,512);return 512;}
Vnode binaryNode{},markerNode{};
bool binaryPresent=false,markerPresent=false,directory=false,statError=false;
unsigned openNodes=0;
AppSnapshotBootInfo CompressedVfsAppSnapshotInfo(){return snapshot;}
extern "C" Vnode* VfsOpen(const char* path,int){
    if(!std::strcmp(path,"/boot/HWBULK.ELF")&&binaryPresent){++openNodes;return &binaryNode;}
    if(!std::strcmp(path,"/etc/hardware-bulk")&&markerPresent){++openNodes;return &markerNode;}
    return nullptr;
}
int VfsStat(Vnode*,VnodeStat* out){*out={};out->size=1;out->isDir=directory;return statError?-5:0;}
extern "C" void VfsClose(Vnode*){assert(openNodes);--openNodes;}

void DebugChannelSend(const char* s){reply+=s;}
void DebugEpochCopy(char out[33]){std::memset(out,'a',32);out[32]=0;}
bool DebugEpochMatches(const char* s){return std::string(s)==std::string(32,'a');}
void DebugHandleCommand(const char* s,uint32_t n){assert(n==std::strlen(s));dispatched=s;}
const char* BuildGitHash(){return "test+build";}
const char* BuildDate(){return "now";}
bool AudioAvailable(){return false;}
DisplayPowerStatus DisplayPowerRead(){return {};}
bool DisplayPowerSetOn(bool on,uint32_t seconds){++backlightCalls;backlightSeconds=seconds;backlightOn=on;return false;}
bool DisplayPowerSetIdle(uint32_t seconds){++backlightCalls;backlightSeconds=seconds;return false;}
ShutdownPowerCapabilities ShutdownGetPowerCapabilities(){return {true,true,false,"acpi-io8","manual","firmware","S5 unavailable"};}
const char* ShutdownRequestStatus(const char*){return "Safe to power off manually";}
void ShutdownCommand(const char*,const char* verb,char state[24],int* error){++powerCalls;powerVerb=verb;std::strcpy(state,"ready");*error=std::strcmp(verb,"commit")?0:-95;}
uint32_t SmpGetCpuCount(){return 4;}
uint32_t PciBootInventoryCount(){return 2;}
uint32_t PciBootInventorySeen(){return 2;}
bool PciBootInventoryRead(uint32_t index,PciBootEntry* out){
    assert(out && index<2);*out={};out->device.bus=0;out->device.dev=index+2;
    out->device.vendorId=0x8086;out->device.deviceId=0x1616;
    out->device.classCode=3;out->device.bar[0]=0xf0000004;
    out->command=7;return true;
}
int DebugOverlayReadRange(uint32_t* cursor,char* out,uint32_t limit,uint32_t stride,uint32_t* total,uint32_t* dropped){
    *total=2;*dropped=0;if(*cursor>2)return -22;
    unsigned count=2-*cursor;if(count>limit)count=limit;
    for(unsigned i=0;i<count;++i)std::strcpy(out+i*stride,i?"next":"quote\" slash\\ newline\n");
    *cursor+=count;return count;
}
}
bool handle(const std::string& command){reply.clear();dispatched.clear();return brook::DebugHardwareHandleCommand(command.c_str(),command.size());}
int main(){
    assert(!handle("bridge unrelated"));
    assert(handle("bridge hardware caps") && reply.find("\"hardware_api\":1")!=std::string::npos);
    assert(reply.find("\"panic_transport\":false")!=std::string::npos);
    assert(reply.find("\"pci_inventory_api\":1")!=std::string::npos);
    assert(reply.find("\"backlight_api\":1")!=std::string::npos);
    assert(reply.find("\"writable_desktop_api\":1")!=std::string::npos);
    brook::ahciStatus.ready=true;std::strcpy(brook::ahciStatus.serial,"serial-123");
    assert(handle("bridge hardware ahci-ro") && reply.find("\"serial\":\"serial-123\"")!=std::string::npos);
    assert(handle("bridge hardware ahci-sector 3") && reply.find("5a5a5a5a")!=std::string::npos);
    assert(handle("bridge hardware ahci-sector 4") && reply.find("ahci_ro_error")!=std::string::npos);
    assert(handle("bridge hardware ahci") && reply.find("ahci_observe_api")!=std::string::npos);
    assert(handle("bridge hardware backlight status") && !backlightCalls && reply.find("\"available\":false")!=std::string::npos);
    assert(handle("bridge hardware backlight off 10") && backlightCalls==1 && backlightSeconds==10 && !backlightOn);
    assert(reply.find("\"ok\":false")!=std::string::npos);
    assert(handle("bridge hardware backlight on") && backlightCalls==2 && backlightOn);
    assert(handle("bridge hardware backlight idle 300") && backlightCalls==3 && backlightSeconds==300);
    for(const auto* bad:{"off","off -1","off 61","off 1 more","idle 86401","idle 4294967296","unknown"}){
        assert(handle(std::string("bridge hardware backlight ")+bad));assert(backlightCalls==3 && reply.find("\"ok\":false")!=std::string::npos);
    }
    assert(handle("bridge hardware pci 0 1"));
    assert(reply.find("\"source\":\"boot_snapshot\"")!=std::string::npos);
    assert(reply.find("\"vendor_id\":32902")!=std::string::npos);
    assert(reply.find("\"raw_bars\":[4026531844,0,0,0,0,0]")!=std::string::npos);
    assert(reply.find("\"next_cursor\":1")!=std::string::npos && reply.find("\"more\":true")!=std::string::npos);
    assert(handle("bridge hardware pci 1 16") && reply.find("\"device\":3")!=std::string::npos && reply.find("\"more\":false")!=std::string::npos);
    assert(handle("bridge hardware pci 2 16") && reply.find("\"devices\":[]")!=std::string::npos);
    for(const auto* bad:{"0 0","0 17","3 1","4294967296 1","0 1 more","0 -1"}){
        assert(handle(std::string("bridge hardware pci ")+bad));assert(reply.find("pci_error")!=std::string::npos);
    }
    assert(handle("bridge hardware acpi-info") && reply.find("\"tables\":1")!=std::string::npos);
    assert(handle("bridge hardware dell-audio-firmware") && reply.find("\"ok\":false")!=std::string::npos);
    audioProbePresent=true;
    assert(handle("bridge hardware dell-audio-firmware") && reply.find("\"i2c_base0_mmio_declared\":true")!=std::string::npos &&
           reply.find("\"i2c_base1_mmio_declared\":false")!=std::string::npos && reply.find("\"types\":2")!=std::string::npos &&
           reply.find("\"i2c_base0_kernel_aliases\":{\"valid\":true")!=std::string::npos &&
           reply.find("\"current_cpu_memory_types\":{\"valid\":true")!=std::string::npos);
    assert(handle("bridge hardware acpi 0 4 2") && reply.find("\"hex\":\"abab\"")!=std::string::npos);
    assert(handle("bridge hardware acpi 0 0 512"));
    assert(reply.find("\"hex\":\""+[](){std::string s;for(int i=0;i<512;++i)s+="ab";return s;}()+"\"")!=std::string::npos);
    for(const auto* bad:{"0 0 513","1 0 1","0 1024 1","0 0 -1","0 0 1 extra","4294967296 0 0"}){
        assert(handle(std::string("bridge hardware acpi ")+bad) && reply.find("acpi_error")!=std::string::npos);
    }
    assert(handle("bridge hardware caps"));
    assert(reply.find("\"power_api\":1")!=std::string::npos && reply.find("\"power_off\":false")!=std::string::npos);
    assert(reply.find("\"bulk_helper_api\":0")!=std::string::npos && !brook::openNodes);
    brook::binaryPresent=true;
    assert(handle("bridge hardware caps") && reply.find("\"bulk_helper_api\":0")!=std::string::npos && !brook::openNodes);
    brook::markerPresent=true;
    assert(handle("bridge hardware caps") && reply.find("\"bulk_helper_api\":1")!=std::string::npos && !brook::openNodes);
    brook::directory=true;
    assert(handle("bridge hardware caps") && reply.find("\"bulk_helper_api\":0")!=std::string::npos && !brook::openNodes);
    brook::directory=false;brook::statError=true;
    assert(handle("bridge hardware caps") && reply.find("\"bulk_helper_api\":0")!=std::string::npos && !brook::openNodes);
    brook::statError=false;brook::snapshot.configured=true;brook::snapshot.sourceClosed=true;
    brook::snapshot.error=brook::AppSnapshotBootError::Hash;
    assert(handle("bridge hardware caps") && reply.find("\"ready\":false")!=std::string::npos);
    brook::snapshot.error=brook::AppSnapshotBootError::None;brook::snapshot.ready=true;brook::snapshot.bytes=1234;
    assert(handle("bridge hardware caps") && reply.find("\"ready\":true")!=std::string::npos && reply.find("\"bytes\":1234")!=std::string::npos);
    const std::string request(32,'1'),boot(32,'a');
    assert(handle("bridge hardware power "+request+" shutdown "+boot) && powerCalls==1 && powerVerb=="shutdown");
    assert(handle("bridge hardware power "+request+" commit "+boot) && powerCalls==2 && reply.find("\"error\":-95")!=std::string::npos);
    for(const auto& bad : {"bridge hardware power short shutdown "+boot,"bridge hardware power "+request+" exec "+boot,"bridge hardware power "+request+" restart "+std::string(32,'b'),"bridge hardware power "+request+" restart "+boot+" more"}){
        assert(handle(bad) && powerCalls==2);
    }
    assert(handle("bridge guard "+std::string(32,'a')+" mem") && dispatched=="mem");
    for(const auto& command:{std::string("bridge guard short"),"bridge guard "+std::string(32,'b')+" mem","bridge guard "+std::string(32,'a')+" @1 mem","bridge guard "+std::string(32,'a')+" bridge guard "+std::string(32,'a')+" mem"}){
        assert(handle(command));assert(dispatched.empty());assert(reply.find("boot_error")!=std::string::npos);
    }
    assert(handle("bridge hardware logs 0 64"));
    assert(reply.find("quote\\\" slash\\\\ newline\\u000a")!=std::string::npos);
    assert(reply.find("\"next_cursor\":2")!=std::string::npos);
    for(const auto* bad:{"0 0","0 65","4294967296 1","2 1 more","3 1","-1 2"}){
        assert(handle(std::string("bridge hardware logs ")+bad));assert(reply.find("logs_error")!=std::string::npos);
    }
    reply.clear();brook::BridgeReplyBuffer<brook::DebugChannelSend> r;
    const std::string longText(2000,'x');r.quoted(longText.c_str());r.send();assert(reply=='"'+longText+'"');
}
