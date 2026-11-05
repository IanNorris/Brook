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
#include "dell_i2c_probe.h"
#include "intel_vtd_observe.h"
#include "intel_bdw_scanout.h"
#include "hda_observe.h"
#include "native_tsc_observe.h"
#include "memory/virtual_memory.h"
#include "cpu.h"
#if defined(BROOK_SSD_AUTO_RECOVERY)
#include "guard_checker_runner.h"
#endif
#include <cassert>
#include <cstring>
#include <string>
std::string reply,dispatched;
unsigned powerCalls=0;std::string powerVerb;
unsigned backlightCalls=0;uint32_t backlightSeconds=0;bool backlightOn=true;
bool audioProbePresent=false,hdaPciPresent=false;
bool pchPresent=false;
bool CpuObserveMemoryTypes(uint64_t,uint64_t,brook::CpuMemoryTypeObservation& out){out={};out.patSlot0=6;out.page0.type=0;out.page1.type=0;return true;}
namespace brook {
#if defined(BROOK_SSD_AUTO_RECOVERY)
bool recoveryPanicAvailable=false;
uint8_t recoveryPanicBytes[]={0x00,0xab,0xff};
bool GuardRecoveryPanicSnapshot(GuardPanicSnapshot& out){
    out={};if(!recoveryPanicAvailable)return false;
    out={recoveryPanicBytes,42,3,0x11223344};return true;
}
#endif
DellGpuVtdObservation gpuVtd{};
const DellGpuVtdObservation& DellGpuVtdObserveRead(){return gpuVtd;}
BdwGopObservation gpuGop{};
const BdwGopObservation& BdwGopObserveRead(){return gpuGop;}
HdaObservation hdaObservation{};
const HdaObservation& HdaObserveRead(){return hdaObservation;}
NativeTscObservation tscObservation{};
const NativeTscObservation& NativeTscObserveRead(){return tscObservation;}
DellI2cIdResult DellI2cIdProbe(uint32_t& value){value=0;return DellI2cIdResult::Disabled;}
DellI2cIdStatus DellI2cIdProbeStatus(){return {};}
bool DellAudioProbeRead(DellAudioFirmwareSnapshot& out,const char*& reason){
    reason=audioProbePresent?"ok":"not provisioned";out.i2cBase0=0xfe103000;out.i2cBase1=0xfe104000;return audioProbePresent;
}
uint32_t AcpiInventoryCount(){return 1;}
bool AcpiMmioMapValid(){return true;}
bool AcpiMmioContains(uint64_t address,uint32_t bytes){return address==0xfe103000 && bytes==4096;}
uint64_t classifiedAddress=0;uint32_t classifiedBytes=0;
bool AcpiClassifyBootRange(uint64_t address,uint32_t bytes,BootResourceClassification& result){classifiedAddress=address;classifiedBytes=bytes;result={2,false};return true;}
bool VmmInspectKernelAliases(uint64_t physical,KernelAliasMapping& a,KernelAliasMapping& b){a={true,21,0,physical};b=a;return true;}
uint32_t HdaRuntimeFailureStage(){return 0;}
uint32_t HdaRuntimeStreamFailureStage(){return 0;}
uint32_t HdaRuntimeStreamFailureDetail(){return 0;}
HdaDellQueueDiagnostics queueDiagnostics{};
HdaDellQueueDiagnostics HdaRuntimeQueueDiagnostics(){return queueDiagnostics;}
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
    if(index==0)out->bdwGraphics={true,0x01c1,0xdd800001};
    if(index==1)out->pchWatchdog={true,0x1801,0x80};
    if(index==1 && pchPresent){out->device.dev=31;out->device.fn=0;out->device.deviceId=0x9cc3;}
    if(hdaPciPresent && index==1){out->device.dev=0x1b;out->device.deviceId=0x9ca0;
        out->device.classCode=4;out->device.subclass=3;out->device.bar[0]=0xf7418004;
        out->subsystemVendor=0x1028;out->subsystemDevice=0x0665;}
    out->command=7;return true;
}
extern "C" uint32_t PciConfigRead32(uint8_t bus,uint8_t dev,uint8_t fn,uint8_t offset){
    assert(bus==0 && dev==31 && fn==0);return offset==0?0x9cc38086u:offset==0x40?0x1801u:0x80u;
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
#if defined(BROOK_SSD_AUTO_RECOVERY)
    assert(handle("bridge hardware recovery-panic 0 0") && reply.find("No validated panic record")!=std::string::npos);
    brook::recoveryPanicAvailable=true;
    assert(handle("bridge hardware recovery-panic 0 3") && reply.find("\"sequence\":42")!=std::string::npos &&
           reply.find("\"crc32\":287454020")!=std::string::npos && reply.find("\"hex\":\"00abff\"")!=std::string::npos);
    assert(handle("bridge hardware recovery-panic 1 2") && reply.find("\"hex\":\"abff\"")!=std::string::npos);
    assert(handle("bridge hardware recovery-panic 3 1") && reply.find("Invalid byte range")!=std::string::npos);
    assert(handle("bridge hardware recovery-panic 0 513") && reply.find("Expected offset")!=std::string::npos);
#endif
    assert(!handle("bridge unrelated"));
    assert(handle("bridge hardware gpu-gop-plan") && reply.find("\"planned\":false")!=std::string::npos && reply.find("first_index")==std::string::npos);
    brook::gpuGop.otherEnginesRequested=true;brook::gpuGop.wake.otherEnginesSampled=true;
    for(unsigned i=0;i<3;++i){brook::gpuGop.wake.other[i].sampled=true;brook::gpuGop.wake.other[i].first.head=i+10;}
    assert(handle("bridge hardware gpu-gop-plan") && reply.find("\"other_engines_sampled\":true")!=std::string::npos &&
        reply.find("\"name\":\"rcs0\"")!=std::string::npos && reply.find("\"name\":\"vcs0\"")!=std::string::npos &&
        reply.find("\"name\":\"vecs0\"")!=std::string::npos && reply.find("\"head\":12")!=std::string::npos && reply.back()=='\n');
    brook::gpuGop={};
    assert(handle("bridge hardware gpu-vtd") && reply.find("\"sampled\":false")!=std::string::npos && reply.find("global_status")==std::string::npos);
    brook::gpuVtd.sampled=true;brook::gpuVtd.status={0x10,0x80000000};
    assert(handle("bridge hardware gpu-vtd") && reply.find("\"translation_enabled\":true")!=std::string::npos);
    assert(handle("bridge hardware caps") && reply.find("\"hardware_api\":1")!=std::string::npos);
    assert(reply.find("\"panic_transport\":false")!=std::string::npos);
    assert(reply.find("\"pci_inventory_api\":1")!=std::string::npos);
    assert(reply.find("\"backlight_api\":1")!=std::string::npos);
    assert(reply.find("\"writable_desktop_api\":1")!=std::string::npos);
    assert(handle("bridge hardware native-tsc-observe") && reply.find("\"requested\":false")!=std::string::npos);
    brook::tscObservation.requested=true;brook::tscObservation.samples[2]=123456;
    assert(handle("bridge hardware native-tsc-observe") && reply.find("\"samples\":[0,0,123456,0,0]")!=std::string::npos);
    brook::tscObservation.coreCount=1;brook::tscObservation.cores[0].cpu=1;
    brook::tscObservation.cores[0].interval.lower=-12;brook::tscObservation.cores[0].interval.upper=34;
    assert(handle("bridge hardware native-tsc-observe") && reply.find("\"lower\":-12,\"upper\":34")!=std::string::npos);
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
    assert(reply.find("\"bdw_firmware_graphics\":{\"graphics_control_raw\":449,\"stolen_base_raw\":3716153345,\"aperture_control_raw\":0}")!=std::string::npos);
    assert(reply.find("\"bdw_stolen_range\":{\"valid\":true,\"base\":3716153344,\"size\":33554432,\"end_exclusive\":3749707776")!=std::string::npos);
    assert(brook::classifiedAddress==0xdd800000ull && brook::classifiedBytes==33554432);
    assert(reply.find("\"boot_map_classified\":true,\"boot_map_types\":2,\"boot_map_unreported\":false")!=std::string::npos);
    assert(reply.find("\"next_cursor\":1")!=std::string::npos && reply.find("\"more\":true")!=std::string::npos);
    assert(handle("bridge hardware pci 1 16") && reply.find("\"device\":3")!=std::string::npos && reply.find("\"more\":false")!=std::string::npos);
    assert(reply.find("bdw_firmware_graphics")==std::string::npos);
    assert(reply.find("\"pch_watchdog_pci\":{\"raw_40\":6145,\"raw_44\":128}")!=std::string::npos);
    assert(handle("bridge hardware pch-watchdog-pci") && reply.find("pch_watchdog_error")!=std::string::npos);
    pchPresent=true;
    assert(handle("bridge hardware pch-watchdog-pci") && reply.find("\"live_raw_40\":6145,\"live_raw_44\":128,\"changed\":false")!=std::string::npos);
    pchPresent=false;
    assert(handle("bridge hardware pci 2 16") && reply.find("\"devices\":[]")!=std::string::npos);
    for(const auto* bad:{"0 0","0 17","3 1","4294967296 1","0 1 more","0 -1"}){
        assert(handle(std::string("bridge hardware pci ")+bad));assert(reply.find("pci_error")!=std::string::npos);
    }
    assert(handle("bridge hardware acpi-info") && reply.find("\"tables\":1")!=std::string::npos);
    assert(handle("bridge hardware dell-i2c-id start") && reply.find("\"result\":\"disabled\"")!=std::string::npos);
    assert(handle("bridge hardware dell-i2c-id status") && reply.find("\"started\":false")!=std::string::npos);
    assert(handle("bridge hardware dell-audio-firmware") && reply.find("\"ok\":false")!=std::string::npos);
    assert(handle("bridge hardware dell-hda-observe") && reply.find("\"requested\":false")!=std::string::npos);
    assert(handle("bridge hardware dell-hda-stream") && reply.find("\"hda_stream_api\":1")!=std::string::npos && reply.find("\"requested\":false")!=std::string::npos);
    brook::hdaObservation.requested=true;brook::hdaObservation.session.completed=2;
    brook::hdaObservation.session.values[0]=123;brook::hdaObservation.session.values[1]=456;
    assert(handle("bridge hardware dell-hda-observe") && reply.find("\"values\":[123,456]")!=std::string::npos);
    brook::queueDiagnostics.enabled=true;brook::queueDiagnostics.startupGapBytes=5184;
    brook::queueDiagnostics.emptyAfterBytes=1200;brook::queueDiagnostics.publishedBytes=4096;
    assert(handle("bridge hardware dell-hda-queue") && reply.find("\"enabled\":true")!=std::string::npos &&
           reply.find("\"startup_gap_bytes\":5184")!=std::string::npos &&
           reply.find("\"empty_after_bytes\":1200")!=std::string::npos &&
           reply.find("\"published_bytes\":4096")!=std::string::npos);
    assert(handle("bridge hardware dell-hda-memory") && reply.find("\"present\":false")!=std::string::npos);
    hdaPciPresent=true;
    assert(handle("bridge hardware dell-hda-memory") && reply.find("\"present\":true")!=std::string::npos &&
           reply.find("\"base\":4148264960")!=std::string::npos && reply.find("\"matches\":true")!=std::string::npos &&
           reply.find("\"mtrr_type\":0")!=std::string::npos);
    hdaPciPresent=false;
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
#if defined(BROOK_SSD_AUTO_RECOVERY)
    assert(reply.find("\"power_api\":0")!=std::string::npos && reply.find("\"recovery_only\":true")!=std::string::npos);
#else
    assert(reply.find("\"power_api\":1")!=std::string::npos && reply.find("\"power_off\":false")!=std::string::npos);
#endif
    assert(reply.find("\"bulk_helper_api\":0")!=std::string::npos && !brook::openNodes);
    brook::binaryPresent=true;
    assert(handle("bridge hardware caps") && reply.find("\"bulk_helper_api\":0")!=std::string::npos && !brook::openNodes);
    brook::markerPresent=true;
#if defined(BROOK_SSD_AUTO_RECOVERY)
    assert(handle("bridge hardware caps") && reply.find("\"bulk_helper_api\":0")!=std::string::npos && !brook::openNodes);
#else
    assert(handle("bridge hardware caps") && reply.find("\"bulk_helper_api\":1")!=std::string::npos && !brook::openNodes);
#endif
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
