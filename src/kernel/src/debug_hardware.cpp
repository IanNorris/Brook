#include "debug_hardware.h"
#include "debug_epoch.h"
#include "debug_bridge_v3.h"
#include "debug_overlay.h"
#include "bridge_reply.h"
#include "build_info.h"
#include "desktop_build_info.h"
#include "audio.h"
#include "../../drivers/xhci/usb_uac2.h"
#include "scheduler.h"
#include "boot_stick.h"
#include "device.h"
#include "smp.h"
#include "shutdown.h"
#include "usb_boot_health.h"
#include "net.h"
#include "string.h"
#include "fs/compressed_vfs.h"
#include "vfs.h"
#include "pci.h"
#include "portio.h"
#include "display_power.h"
#include "idle_inhibit.h"
#include "ahci_observe.h"
#include "ahci_readonly.h"
#include "writable_desktop.h"
#include "usb_system.h"
#include "ssd_install_qemu.h"
#include "acpi.h"
#include "boot_resource_map.h"
#include "memory/physical_memory.h"
#include "hw_inventory.h"
#include "dell_audio_probe.h"
#include "dell_i2c_probe.h"
#include "intel_vtd_observe.h"
#include "intel_bdw_scanout.h"
#include "intel_bdw_bootstrap_gate.h"
#include "hda_observe.h"
#include "native_tsc_observe.h"
#include "tsc_clock.h"
#include "kprintf.h"
#include "spinlock.h"
#include "platform_power.h"
extern "C" void NvDisplayMarkWritten(); // debug_hardware.cpp: pre-reset GPU reset trigger
#include "nv_disp_live.h"
#include "nv_aux_reply.h"
#include "input.h"
#include "process.h"
#include "memory/virtual_memory.h"
#include "cpu.h"
#include "panic_snapshot.h"
#include "intel_bdw_gtt_probe.h"
#include "intel_bdw_display_probe.h"
#include "intel_bdw_forcewake_adapter.h"
#include "intel_bdw_pat.h"
#include "intel_bdw_render_gt_init.h"
#include "intel_bdw_oneshot.h"
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
#include "process.h"
#include "scheduler.h"
#include "apic.h"
#include "boot_timing_runtime.h"
#include "../../shared/boot_protocol/boot_protocol.h"
#endif
#if defined(BROOK_SSD_GUARDED_DESKTOP)
#include "ac_shutdown_record.h"
#endif
#if defined(BROOK_SSD_AUTO_RECOVERY)
#include "guard_checker_runner.h"
#endif
#if !defined(BROOK_HOST_TEST)
#include "apic.h"
#endif
#include "../../drivers/intel_hda/hda_probe.h"
namespace brook {
#if defined(BROOK_HOST_TEST)
uint16_t PchWatchdogTestRead16(uint16_t port);
uint32_t PchWatchdogTestRead32(uint16_t port);
#endif
// Read a small complete marker from the boot initrd without caching file data.
static unsigned ReadBootMetadata(const char* path, char* bytes, unsigned capacity)
{
    auto* file = VfsOpen(path, VFS_O_STREAM);
    if (!file) return 0;
    VnodeStat stat{};
    uint64_t offset = 0;
    const bool complete = !VfsStat(file, &stat) && !stat.isDir && !stat.isSymlink &&
                          stat.size >= 2 && stat.size <= capacity &&
                          VfsRead(file, bytes, stat.size, &offset) == static_cast<int>(stat.size) &&
                          bytes[stat.size - 1] == '\n';
    VfsRelease(file);
    return complete ? unsigned(stat.size) : 0;
}

// Same newline-terminated initrd marker used by HWSTATUS.ELF.
bool HardwarePayloadIdRead(char* id, uint32_t capacity)
{
    if (!id || capacity < 2) return false;
    const unsigned length = ReadBootMetadata("/etc/hardware-payload-id", id, capacity);
    if (length) {
        bool valid = true;
        for (unsigned i = 0; i + 1 < length; ++i) {
            const char c = id[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') continue;
            valid = false;
            break;
        }
        if (valid) {
            id[length - 1] = 0;
            return true;
        }
    }
    id[0] = 0;
    return false;
}

bool HardwareBuildProvenanceRead(DesktopBuildProvenance& provenance)
{
    char bytes[BuildProvenanceLimit];
    const unsigned length = ReadBootMetadata("/etc/build-provenance", bytes, sizeof(bytes));
    return ParseDesktopBuildProvenance(bytes, length, provenance);
}
namespace {
using Reply=BridgeReplyBuffer<DebugChannelSend>;
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
void RecoverySlotsReply()
{
    GuardRecoverySlotState state{};
    const int result = GuardRecoverySlotStatus(state);
    Reply reply;
    reply.text("{\"slot_api\":1,\"available\":");
    if (result) {
        reply.text("false,\"errno\":");
        reply.number(-int64_t(result));
        reply.text("}\n");
        reply.send();
        return;
    }

    reply.text("true,");
    reply.field("sequence", state.sequence);
    reply.append(',');
    reply.field("newest_generation", state.newestGeneration);
    const auto running=BootTimingHandoffSlot();
    if(running.generation) {
        reply.append(',');
        reply.field("running_generation",running.generation);
        reply.append(',');
        reply.field("running_slot",running.slot);
        reply.text(",\"running_previous\":");
        reply.text(running.generation < state.newestGeneration ? "true" : "false");
    }
    reply.text(",\"slots\":[");
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (slot) reply.append(',');
        reply.append('{');
        reply.field("generation", state.generations[slot]);
        reply.append(',');
        reply.field("starts", state.starts[slot]);
        reply.text(",\"profile_rejected\":");
        reply.text(state.profileRejected[slot] ? "true" : "false");
        reply.text(",\"selector_sha256\":");
        if (state.generations[slot]) {
            reply.append('"');
            for (auto byte : state.selectorDigests[slot]) {
                reply.append(hex[byte >> 4]);
                reply.append(hex[byte & 15]);
            }
            reply.append('"');
        } else {
            reply.text("null");
        }
        reply.append('}');
    }
    reply.text("]}\n");
    reply.send();
}
#endif
// Dell HDA fault record JSON (read-only copies taken under the bridge lock).
void HdaRegistersJson(Reply& r,const char* name,const HdaDellStreamRegisters& g){
    r.text("\"");r.text(name);r.text("\":{\"valid\":");r.text(g.valid?"true":"false");
    r.text(",");r.field("pci_command",g.command);r.text(",");r.field("sdctl",g.control);r.text(",");r.field("sdsts",g.status);
    r.text(",\"run\":");r.text(g.control&2?"true":"false");r.text(",\"fifoe\":");r.text(g.status&8?"true":"false");
    r.text(",\"dese\":");r.text(g.status&0x10?"true":"false");r.text(",\"bcis\":");r.text(g.status&4?"true":"false");
    r.text(",");r.field("lpib",g.lpib);r.text(",");r.field("cbl",g.cbl);r.text(",");r.field("lvi",g.lvi);
    r.text(",");r.field("fifos",g.fifos);r.text(",");r.field("format",g.format);r.text(",");r.field("bdpl",g.bdpl);
    r.text(",");r.field("bdpu",g.bdpu);r.text(",");r.field("intctl",g.intctl);r.text(",");r.field("intsts",g.intsts);
    r.text(",");r.field("wallclock",g.wallclock);r.text(",");r.field("dplbase",g.dplbase);r.text("}");
}
void HdaFaultJson(Reply& r,const char* name,const HdaDellQueueFault& f){
    r.text("\"");r.text(name);r.text("\":");if(!f.recorded){r.text("null");return;}
    r.text("{");r.field("ordinal",f.ordinal);r.text(",\"stage\":");r.quoted(HdaDellFaultStageName(f.stage));
    r.text(",\"reason\":");r.quoted(HdaDellFaultReasonName(f.reason));
    r.text(",\"sample_reason\":");r.quoted(HdaDellFaultReasonName(f.sampleReason));
    r.text(",");r.field("epoch",f.epoch);r.text(",");r.field("at_ns",f.atNs);
    r.text(",");r.field("sample_before_ns",f.sampleBeforeNs);r.text(",");r.field("sample_after_ns",f.sampleAfterNs);
    r.text(",");r.field("previous_sample_ns",f.previousSampleNs);r.text(",");r.field("last_movement_ns",f.lastMovementNs);
    r.text(",");r.field("position",f.position);r.text(",");r.field("previous_position",f.previousPosition);
    r.text(",");r.field("delta",f.delta);r.text(",");r.field("allowed",f.allowed);
    r.text(",\"sampled_running\":");r.text(f.sampledRunning?"true":"false");
    r.text(",\"sampled_error\":");r.text(f.sampledError?"true":"false");
    r.text(",");r.field("raw_completed",f.rawCompleted);r.text(",");r.field("submitted",f.submitted);
    r.text(",");r.field("completed",f.completed);r.text(",");r.field("cleared",f.cleared);
    r.text(",\"muted\":");r.text(f.muted?"true":"false");r.text(",\"stopped\":");r.text(f.stopped?"true":"false");
    r.text(",\"recoverable\":");r.text(f.recoverable?"true":"false");r.text(",");
    HdaRegistersJson(r,"at_fault",f.atFault);r.text(",");HdaRegistersJson(r,"after_stop",f.afterStop);r.text("}");
}
void HdaRecoveryJson(Reply& r,const HdaRuntimeFaultSnapshot& s){
    const auto& v=s.recovery;r.text("\"recovery\":{\"enabled\":");r.text(v.enabled?"true":"false");
    r.text(",\"pending\":");r.text(v.pending?"true":"false");r.text(",\"permanent\":");r.text(v.permanent?"true":"false");
    r.text(",");r.field("budget",v.budget);r.text(",");r.field("attempts",v.attempts);r.text(",");r.field("successes",v.successes);
    r.text(",");r.field("failed_step",uint8_t(v.lastFailure));r.text(",");r.field("last_recovery_ns",v.lastRecoveryNs);
    r.text(",");r.field("min_spacing_ns",v.minSpacingNs);r.text("}");
}

void HardwarePayloadId(Reply& reply) {
    char id[96]{};
    const bool valid=HardwarePayloadIdRead(id,sizeof(id));
    reply.text(",\"payload_id\":");
    if(valid)reply.quoted(id);else reply.text("null");
}
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
// One persistent receipt per test variant. A timed-out bridge observation
// must never start another worker while the original may still be running.
struct RcsDebugJob {
    uint32_t state=0; // 0 unused, 1 worker published/running, 2 finished
    uint32_t mode=0;bool ok=false,sampleVtd=false,sampledVtd=false;
    BdwForcewakeAdapterResult forcewake{};
    BdwRenderRunResult render{};
};
RcsDebugJob rcsDebugJobs[3];
void RcsDebugWorker(void* opaque) {
    auto* job=static_cast<RcsDebugJob*>(opaque);
    // Firmware table addresses are mapped in the kernel-thread address
    // space, not necessarily in the application that issued the ioctl.
    if(job->sampleVtd){
        job->sampledVtd=DellGpuVtdObserveRead().sampled || DellGpuVtdObserveDeferred();
        if(!job->sampledVtd){__atomic_store_n(&job->state,2,__ATOMIC_RELEASE);return;}
    }
    job->ok=job->mode==2?BdwTestRenderRepeat(job->forcewake,job->render):job->mode==1?BdwTestRebasedRenderRun(job->forcewake,job->render):
        BdwTestRenderRun(job->forcewake,job->render);
    __atomic_store_n(&job->state,2,__ATOMIC_RELEASE);
}
bool RcsDebugRun(unsigned mode,BdwForcewakeAdapterResult& forcewake,BdwRenderRunResult& render,bool& pending,bool sampleVtd=false) {
    auto& job=rcsDebugJobs[mode];pending=false;
    uint32_t expected=0;
    if(__atomic_compare_exchange_n(&job.state,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
        job.mode=mode;job.sampleVtd=sampleVtd;
        auto* worker=KernelThreadCreate("gpu-rcs-test",RcsDebugWorker,&job);
        if(!worker){__atomic_store_n(&job.state,0,__ATOMIC_RELEASE);return false;}
        worker->cpuAffinity=0;SchedulerAddProcess(worker);
    }
    const uint64_t deadline=ApicTickCount()+5000;
    while(__atomic_load_n(&job.state,__ATOMIC_ACQUIRE)!=2 && ApicTickCount()<deadline)SchedulerSleepMs(5);
    if(__atomic_load_n(&job.state,__ATOMIC_ACQUIRE)!=2){pending=true;return false;}
    forcewake=job.forcewake;render=job.render;return job.ok;
}
struct SubmissionDebugJob {uint32_t state=0;bool ok=false,contexts=false;BdwWorkerProbeResult result{};};
SubmissionDebugJob submissionDebugJobs[2];
void SubmissionDebugWorker(void* opaque){
    auto* job=static_cast<SubmissionDebugJob*>(opaque);
    job->ok=job->contexts?BdwTestContextSubmissionWorker(job->result):BdwTestSubmissionWorker(job->result);
    __atomic_store_n(&job->state,2,__ATOMIC_RELEASE);
}
bool SubmissionDebugRun(BdwWorkerProbeResult& result,bool& pending,bool contexts){
    auto& job=submissionDebugJobs[contexts?1:0];pending=false;uint32_t expected=0;
    if(__atomic_compare_exchange_n(&job.state,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
        job.contexts=contexts;
        auto* thread=KernelThreadCreate("gpu-worker-test",SubmissionDebugWorker,&job);
        if(!thread){__atomic_store_n(&job.state,0,__ATOMIC_RELEASE);return false;}
        thread->cpuAffinity=0;SchedulerAddProcess(thread);
    }
    const uint64_t deadline=ApicTickCount()+5000;
    while(__atomic_load_n(&job.state,__ATOMIC_ACQUIRE)!=2 && ApicTickCount()<deadline)SchedulerSleepMs(5);
    if(__atomic_load_n(&job.state,__ATOMIC_ACQUIRE)!=2){pending=true;return false;}
    result=job.result;return job.ok;
}
#endif
bool Prefix(const char* text,const char* prefix){while(*prefix)if(*text++!=*prefix++)return false;return true;}
bool Decimal(const char*& p,uint32_t& value){
    if(*p<'0'||*p>'9')return false;value=0;
    while(*p>='0'&&*p<='9'){
        unsigned digit=*p++-'0';if(value>(0xffffffffu-digit)/10)return false;value=value*10+digit;
    }
    return true;
}
bool Decimal64(const char*& p,uint64_t& value){
    if(*p<'0'||*p>'9')return false;value=0;
    while(*p>='0'&&*p<='9'){
        unsigned digit=*p++-'0';if(value>(~uint64_t{0}-digit)/10)return false;
        value=value*10+digit;
    }
    return true;
}
bool ReadId(const char*& p,char out[33]){
    for(unsigned i=0;i<32;++i){char c=p[i];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;out[i]=c;}
    out[32]=0;p+=32;return true;
}
#if !defined(BROOK_SSD_AUTO_RECOVERY) || defined(BROOK_SSD_GUARDED_DESKTOP)
void PowerCaps(Reply& r){
    const auto power=ShutdownGetPowerCapabilities();
    r.text("\"graceful_quiescence\":");r.text(power.gracefulQuiescence?"true":"false");
    r.text(",\"reset\":");r.text(power.reset?"true":"false");r.text(",\"power_off\":");r.text(power.powerOff?"true":"false");
    r.text(",\"reset_method\":");r.quoted(power.resetMethod);r.text(",\"power_off_method\":");r.quoted(power.powerOffMethod);
    r.text(",\"reset_reason\":");r.quoted(power.resetReason);r.text(",\"power_off_reason\":");r.quoted(power.powerOffReason);
}
void DesktopCaps(Reply& r){
    r.text(",\"power_api\":1,\"power\":{");PowerCaps(r);r.text("}");
#if !defined(BROOK_SSD_GUARD_CHECK) || defined(BROOK_SSD_GUARDED_DESKTOP)
    r.text(",\"recovery_panic_api\":1");
#endif
    VnodeStat helper{},optin{};
    auto* binary=VfsOpen("/boot/HWBULK.ELF",VFS_O_STREAM);
    auto* marker=VfsOpen("/etc/hardware-bulk",VFS_O_STREAM);
    const bool bulk=binary&&marker&&!VfsStat(binary,&helper)&&!helper.isDir&&helper.size>0&&
        !VfsStat(marker,&optin)&&!optin.isDir&&optin.size>0;
    if(binary)VfsClose(binary);
    if(marker)VfsClose(marker);
    r.text(",\"completed_frame_copy_api\":1,\"bulk_helper_api\":");r.number(bulk?1:0);
    r.text(",\"panic_transport\":false,\"raw_frame_transfer\":");r.text(bulk?"true":"false");
}
#endif
const char* I2cProbeResult(DellI2cIdResult result) {
    switch(result){case DellI2cIdResult::Disabled:return "disabled";case DellI2cIdResult::AlreadyUsed:return "already_used";
    case DellI2cIdResult::Refused:return "refused";case DellI2cIdResult::Unknown:return "unknown";
    case DellI2cIdResult::Identified:return "identified";}
    return "invalid";
}

}
bool DebugHardwareAutoInitializeIntel(){
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    if(BdwRenderExperimentEnabled())return true;
    if(!BdwAutomaticRenderRequested())return false;
    // A single boot-lifetime owner performs the deferred observation and
    // delegates the one-shot operation to the same pinned worker used by the
    // debug command. Other callers wait for that owner; they never replay it.
    static BdwBootstrapGate gate;
    if(gate.TryStart()){
        BdwForcewakeAdapterResult forcewake{};BdwRenderRunResult render{};bool pending=false;
        const bool ok=RcsDebugRun(0,forcewake,render,pending,true) && BdwRenderExperimentEnabled();
        const bool sampled=DellGpuVtdObserveRead().sampled;
        KPrintf("INTEL_AUTO_BOOTSTRAP sampled=%u ready=%u pending=%u stage=%u dma=%u\n",
                unsigned(sampled),unsigned(ok),unsigned(pending),render.preflightStage,render.dmaStatus);
        gate.Finish(ok);
        return ok;
    }
    const uint64_t deadline=ApicTickCount()+5000;
    uint32_t state;
    while((state=gate.State())==1 && ApicTickCount()<deadline)SchedulerSleepMs(5);
    return state==2 && BdwRenderExperimentEnabled();
#else
    return false;
#endif
}
// NVIDIA desktop display (nv-aux, dp-idle/link watch, pre-reset GPU reset):
// built only with BROOK_NV_DISPLAY (RTX desktop images), so it costs other
// kernels (the Dell pair's 8 MiB update limit) nothing.
#if !defined(BROOK_HOST_TEST) && defined(BROOK_NV_DISPLAY)
namespace {
// nv-aux runs from the debug command thread and from the dp-idle worker; one
// lock serialises AUX and SOR access. A silent caller gets the JSON reply in
// its own buffer instead of the debug channel.
SpinLock g_nvAuxLock;
bool g_nvMapDone=false;
char* g_nvAuxOut=nullptr;uint32_t g_nvAuxCap=0,g_nvAuxLen=0;
void NvAuxSend(const char* t){
    if(!g_nvAuxOut){DebugChannelSend(t);return;}
    while(*t&&g_nvAuxLen+1<g_nvAuxCap)g_nvAuxOut[g_nvAuxLen++]=*t++;
    g_nvAuxOut[g_nvAuxLen]=0;
}
using AuxReply=BridgeReplyBuffer<NvAuxSend>;
// BAR0 windows (AUX 0xd000, SOR 0x61c000, ARM state 0x688000, read-only) are
// mapped once, outside g_nvAuxLock, by whichever caller gets there first.
VirtualAddress g_nvAuxWin{},g_nvSorWin{},g_nvArmWin{};
SpinLock g_nvMapLock;
VirtualAddress NvMapOne(uint64_t physical,bool writable){
    auto va=VmmAllocPages(1,VMM_WRITABLE|VMM_NO_EXEC,MemTag::Device,KernelPid);
    if(!va)return va;
    const auto leaf=VmmDetachPage(KernelPageTable,va);if(leaf)PmmFreePage(VmmLeafPhysical(leaf));
    __asm__ volatile("invlpg (%0)"::"r"(va.raw()):"memory");
    if(!VmmMapPage(KernelPageTable,va,PhysicalAddress(physical),
        (writable?VMM_WRITABLE:0)|VMM_NO_EXEC|VMM_CACHE_DISABLE|VMM_WRITE_THRU,MemTag::Device,KernelPid))return VirtualAddress{};
    return va;
}
bool NvAuxMapWindows(){
    if(__atomic_load_n(&g_nvMapDone,__ATOMIC_ACQUIRE))return true;
    PciDevice gpu{};bool found=false;
    for(uint32_t i=0;i<PciBootInventoryCount() && !found;++i){PciBootEntry e{};
        if(PciBootInventoryRead(i,&e) && e.device.vendorId==0x10de && e.device.classCode==0x03 && e.device.fn==0){gpu=e.device;found=true;}}
    if(!found)return false;
    const uint32_t bar0=PciConfigRead32(gpu.bus,gpu.dev,gpu.fn,0x10);
    if((bar0&1)||!(PciConfigRead16(gpu.bus,gpu.dev,gpu.fn,4)&2))return false;
    const uint64_t base=bar0&~uint64_t(15);
    // One mapper at a time; the VMM calls are made with no spinlock held by
    // this thread other than this try-lock, and losers simply retry later.
    if(!SpinLockTryAcquire(&g_nvMapLock))return false;
    if(!g_nvAuxWin)g_nvAuxWin=NvMapOne(base+0xd000,true);
    if(!g_nvSorWin)g_nvSorWin=NvMapOne(base+0x61c000,true);
    if(!g_nvArmWin)g_nvArmWin=NvMapOne(base+0x688000,false);
    const bool ok=g_nvAuxWin&&g_nvSorWin&&g_nvArmWin;
    if(ok)__atomic_store_n(&g_nvMapDone,true,__ATOMIC_RELEASE);
    SpinLockRelease(&g_nvMapLock);
    return ok;
}
bool NvAuxHandleLocked(const char* command){
    constexpr char auxPrefix[]="bridge hardware nv-aux ";
    if(Prefix(command,auxPrefix)) {
        const char* p=command+sizeof(auxPrefix)-1;
        auto word=[&](const char* w)->bool{const char* q=p;while(*w){if(*q!=*w)return false;++q;++w;}if(*q&&*q!=' ')return false;p=q;while(*p==' ')++p;return true;};
        auto hex=[&](uint32_t& v)->bool{unsigned n=0;v=0;while(*p==' ')++p;
            while((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')){v=v*16+uint32_t(*p<='9'?*p-'0':*p-'a'+10);++p;if(++n>8)return false;}while(*p==' ')++p;return n>0;};
        const bool rdMode=word("read"),edMode=!rdMode&&word("edid"),pwMode=!rdMode&&!edMode&&word("power");
        const bool rtMode=!rdMode&&!edMode&&!pwMode&&word("retrain");
        uint32_t ch=0,addr=0,len=0;
        if((!rdMode&&!edMode&&!pwMode&&!rtMode)||!hex(ch)||ch>7){NvAuxSend("{\"aux_error\":\"usage: nv-aux read <ch> <addr> <len>|edid <ch> <block>|power <ch> d0|d3\"}\n");return true;}
        uint8_t powerValue=0;
        if(rdMode&&(!hex(addr)||!hex(len)||!len||len>16||addr>0xfffff)){NvAuxSend("{\"aux_error\":\"bad read args\"}\n");return true;}
        if(edMode&&(!hex(addr)||addr>3)){NvAuxSend("{\"aux_error\":\"block 0..3\"}\n");return true;}
        if(pwMode){if(word("d0"))powerValue=1;else if(word("d3"))powerValue=2;else{NvAuxSend("{\"aux_error\":\"d0|d3\"}\n");return true;}}
        uint32_t sor=0,link=0;
        if(rtMode&&(!hex(sor)||sor>3||!hex(link)||(link!=1&&link!=2))){NvAuxSend("{\"aux_error\":\"retrain <ch> <sor 0-3> <link 1|2>\"}\n");return true;}
        PciDevice gpu{};bool found=false;
        for(uint32_t i=0;i<PciBootInventoryCount() && !found;++i){PciBootEntry e{};
            if(PciBootInventoryRead(i,&e) && e.device.vendorId==0x10de && e.device.classCode==0x03 && e.device.fn==0){gpu=e.device;found=true;}}
        const uint32_t bar0=found?PciConfigRead32(gpu.bus,gpu.dev,gpu.fn,0x10):0;
        if(!found||(bar0&1)||!(PciConfigRead16(gpu.bus,gpu.dev,gpu.fn,4)&2)){NvAuxSend("{\"aux_error\":\"no GPU BAR0\"}\n");return true;}
        (void)bar0;
        if(!g_nvAuxWin){NvAuxSend("{\"aux_error\":\"map\"}\n");return true;}
        volatile uint32_t* reg=reinterpret_cast<volatile uint32_t*>(g_nvAuxWin.raw());
        const uint32_t base=ch*0x50;
        auto R=[&](uint32_t off)->uint32_t{return reg[(off-0xd000)/4];};
        auto Wr=[&](uint32_t off,uint32_t v){reg[(off-0xd000)/4]=v;};
        auto spin=[](uint32_t us){
            if(!TscClockAvailable()){for(volatile uint32_t i=0;i<us*300;++i)__asm__ volatile("pause");return;}
            const uint64_t t=TscClockReadNs()+uint64_t(us)*1000;while(TscClockReadNs()<t)__asm__ volatile("pause");};
        char err[64]="";
        // pad -> AUX mode, enabled
        Wr(0xd970+base,(R(0xd970+base)&~0xc003u)|0x2u); Wr(0xd97c+base,R(0xd97c+base)&~1u);
        // acquire: wait idle, request, wait grant
        uint32_t ctrl=0;bool ok=true;
        for(int i=0;i<1000;++i){ctrl=R(0xd954+base);if(!(ctrl&0x07010000))break;spin(1);if(i==999){ok=false;strcpy(err,"begin idle timeout");}}
        if(ok){Wr(0xd954+base,(R(0xd954+base)&~0x00700000u)|0x00100000u);
            for(int i=0;i<1000;++i){ctrl=R(0xd954+base);if((ctrl&0x07000000)==0x01000000)break;spin(1);if(i==999){ok=false;strcpy(err,"grant timeout");}}}
        auto xfer=[&](uint32_t type,uint32_t a,uint8_t* data,uint32_t& size)->int{
            if(!(R(0xd958+base)&0x10000000))return -6; // sink not detected
            for(uint32_t retries=0;retries<32;++retries){
                if(retries)spin(400);
                if(!(type&1))for(uint32_t i=0;i<16;i+=4){uint32_t v=0;for(uint32_t b=0;b<4&&i+b<size;++b)v|=uint32_t(data[i+b])<<(8*b);Wr(0xd930+base+i,v);}
                Wr(0xd950+base,a);
                uint32_t c=R(0xd954+base);c&=~0x0001f1ffu;c|=type<<12;c|=size?(size-1):0x100;
                Wr(0xd954+base,0x80000000u|c);Wr(0xd954+base,c);Wr(0xd954+base,0x00010000u|c);
                int t=2000;while((R(0xd954+base)&0x00010000)&&--t)spin(1);
                if(!t)return -5;
                const uint32_t st=R(0xd958+base);Wr(0xd958+base,st);
                if(st&0x00000100)continue;        // timeout -> retry
                if(st&0x00000e00)return -5;
                const uint32_t reply=(st>>16)&0xf;
                if(reply==0x2||reply==0x8)continue; // DEFER (native/I2C) -> retry
                if(reply==0x1||reply==0x4)return -6; // NACK
                // Copy only what the caller asked for and the sink returned:
                // the read window is 16 bytes but callers pass 1-, 4- and
                // 6-byte buffers (an unbounded copy smashed their stacks).
                if(type&1)size=NvAuxCopyReply(data,size,st&0x1f,[&](uint32_t o){return R(0xd940+base+o);});
                return 0;
            }
            return -11;
        };
        AuxReply r;r.text("{\"aux_api\":1,");r.field("ch",ch);r.text(",");
        uint8_t buf[128]{};int rc=ok?0:-16;uint32_t got=0;
        if(ok&&rdMode){uint32_t n=len;rc=xfer(0x9,addr,buf,n);got=rc?0:n;}
        else if(ok&&edMode){
            uint8_t off=uint8_t(addr*128);uint32_t one=1;rc=xfer(0x4,0x50,&off,one);         // I2C write MOT: offset
            for(uint32_t i=0;!rc&&i<128;i+=16){uint32_t n=16;rc=xfer(0x5,0x50,buf+i,n);if(!rc&&n!=16)rc=-5;got=i+n;} // I2C read MOT
            uint32_t z=0;(void)xfer(0x1,0x50,nullptr,z);                                    // stop (no MOT)
        }
        else if(ok&&rtMode){
            // Retrain the GOP's own DP link in place (nouveau retrain path):
            // clock, lane enables, power and drive stay as programmed; only
            // the SOR training pattern and the sink's DPCD training registers
            // change. Requires the SOR's ARM state to be DP and on a head.
            const VirtualAddress sorWin=g_nvSorWin,roWin=g_nvArmWin;
            if(!sorWin||!roWin){rc=-12;strcpy(err,"map");}
            else{
                volatile uint32_t* sr=reinterpret_cast<volatile uint32_t*>(sorWin.raw());
                const volatile uint32_t* arm=reinterpret_cast<const volatile uint32_t*>(roWin.raw());
                const uint32_t sorCtl=arm[(0x300+sor*0x20)/4]; // 0x688300 + sor*0x20 (ARM)
                const uint32_t proto=(sorCtl>>8)&0xf;
                const uint32_t soff=sor*0x800,loff=soff+(link==2?0x80:0);
                const uint32_t dpctrl=sr[(0x10c+loff)/4];
                if(!(sorCtl&0xff)||(proto!=8&&proto!=9)||(link==2)!=(proto==9)||!(dpctrl&0x000f0000)){rc=-22;strcpy(err,"SOR not an active DP link");}
                else{
                    const uint32_t patReg=(link&1)?0x110+soff:0x12c+soff; // gm107_sor_dp_pattern
                    const uint32_t patOrig=sr[patReg/4];
                    auto pattern=[&](uint32_t pat){static const uint32_t d[]={0x10101010,0x01010101,0x02020202,0x03030303};
                        sr[patReg/4]=(sr[patReg/4]&~0x1f1f1f1fu)|d[pat];};
                    const uint32_t lanes=__builtin_popcount((dpctrl>>16)&0xf);
                    NvDisplayMarkWritten();
                    KPrintf("nv-aux: retrain start SOR%u link %u dpctrl %08x pat %08x\n",sor,link,dpctrl,patOrig);
                    uint8_t caps[16]{};uint32_t n=16;rc=xfer(0x9,0x000,caps,n);
                    uint8_t conf[4]{};n=4;if(!rc)rc=xfer(0x9,0x103,conf,n);
                    uint8_t lc[2]={caps[1],uint8_t(lanes|((dpctrl&0x4000)?0x80:0))};n=2;
                    if(!rc)rc=xfer(0x8,0x100,lc,n);
                    auto status=[&](uint8_t* st)->int{uint32_t m=6;return xfer(0x9,0x202,st,m);};
                    auto lanesOk=[&](const uint8_t* st,uint8_t mask)->bool{for(uint32_t i=0;i<lanes;++i)if(((st[i>>1]>>((i&1)*4))&mask)!=mask)return false;return true;};
                    uint8_t st[6]{};bool cr=false,eq=false;
                    if(!rc){pattern(1);uint8_t tp=0x21;n=1;rc=xfer(0x8,0x102,&tp,n);n=4;if(!rc)rc=xfer(0x8,0x103,conf,n);}
                    for(int t=0;!rc&&t<5&&!cr;++t){spin(100);rc=status(st);cr=!rc&&lanesOk(st,0x1);}
                    KPrintf("nv-aux: retrain cr=%d rc=%d %02x %02x\n",cr,rc,st[0],st[1]);
                    const bool tps3=(caps[2]&0x40)!=0;
                    if(!rc&&cr){pattern(tps3?3:2);uint8_t tp=uint8_t(0x20|(tps3?3:2));n=1;rc=xfer(0x8,0x102,&tp,n);}
                    for(int t=0;!rc&&cr&&t<6&&!eq;++t){spin(400);rc=status(st);eq=!rc&&lanesOk(st,0x7)&&(st[2]&1);}
                    sr[patReg/4]=patOrig; uint8_t tp0=0;n=1;(void)xfer(0x8,0x102,&tp0,n);
                    spin(1000);(void)status(st);
                    r.field("cr",cr?1u:0u);r.text(",");r.field("eq",eq?1u:0u);r.text(",");
                    r.field("lane01",st[0]);r.text(",");r.field("lane23",st[1]);r.text(",");r.field("align",st[2]);r.text(",");
                    KPrintf("nv-aux: retrain SOR%u link %u ch %u cr=%d eq=%d status %02x %02x %02x\n",sor,link,ch,cr,eq,st[0],st[1],st[2]);
                }
            }
        }
        else if(ok&&pwMode){uint32_t one=1;rc=xfer(0x8,0x600,&powerValue,one);
            uint32_t n=1;uint8_t back=0;if(!rc&&!xfer(0x9,0x600,&back,n)){r.field("dpcd600",back);r.text(",");}
            KPrintf("nv-aux: ch %u DPCD 0x600 <- %u (%s), rc %d\n",ch,powerValue,powerValue==1?"D0":"D3",rc);}
        // release: drop the request
        Wr(0xd954+base,R(0xd954+base)&~0x00710000u);
        r.field("rc",uint32_t(rc<0?-rc:rc));r.text(",\"err\":");r.quoted(err);r.text(",");r.field("bytes",got);r.text(",\"hex\":\"");
        static const char hx[]="0123456789abcdef";for(uint32_t i=0;i<got;++i){char t[3]={hx[buf[i]>>4],hx[buf[i]&15],0};r.text(t);}
        r.text("\"}\n");r.send();return true;
    }
    return false;
}
bool NvAuxHandle(const char* command,char* out,uint32_t cap){
    if(!Prefix(command,"bridge hardware nv-aux "))return false;
    (void)NvAuxMapWindows();
    SpinLockAcquire(&g_nvAuxLock);
    g_nvAuxOut=out;g_nvAuxCap=cap;g_nvAuxLen=0;if(out&&cap)out[0]=0;
    const bool handled=NvAuxHandleLocked(command);
    g_nvAuxOut=nullptr;g_nvAuxCap=0;
    SpinLockRelease(&g_nvAuxLock);
    return handled;
}

// RTX display idle-off (gated: /etc/dp-idle-seconds "<seconds> [ch sor link]"
// or `bridge hardware dp-idle`). After <seconds> without input, DPCD 0x600 D3
// on one DP sink; any input wakes it: D0, then an in-place retrain of the
// GOP's own link. The head keeps scanning throughout. A failed wake is
// retried once a second until the link trains, so the screen is never left
// dark without another attempt.
// TODO: honour idle inhibitors (video playback) once the desktop has them.
struct DpIdleState{
    uint32_t seconds=0,ch=3,sor=1,link=2;
    bool blanked=false,threadStarted=false,wakeRequested=false,handTest=false;
    uint64_t seq=0,lastActivityMs=0,lastWakeTryMs=0;
    uint32_t blanks=0,wakes=0,wakeFails=0;
    // Link watch (part of the wake path): after a hot-plug edge or on input,
    // retrain the GOP's link if the head is active but the link is down
    // (KVM switch-back, monitor replug). Rate-limited, capped retries.
    bool watch=false,sinkPresent=false,linkCheckPending=false,linkCheckLog=false,linkCheckForce=false;
    uint64_t lastLinkCheckMs=0,edgeMs=0;
    uint8_t recheckStage=3; // after a hotplug edge: re-checks at +1 s, +3 s, +10 s
    uint32_t linkRetrains=0,linkFails=0,hotplugs=0;
    char lastReply[320]{};
};
DpIdleState g_dpIdle;
SpinLock g_dpIdleLock;
const char* Find(const char* text,const char* key){
    for(;*text;++text){const char* a=text;const char* b=key;while(*b&&*a==*b){++a;++b;}if(!*b)return text;}
    return nullptr;
}
bool JsonOne(const char* text,const char* key){
    const char* f=Find(text,key);if(!f)return false;f+=strlen(key);
    while(*f==' '||*f==':')++f;return *f=='1';
}
// "bridge hardware nv-aux <verb> <hex args...>"
bool DpIdleAux(const char* verb,uint32_t a,int b=-1,int c=-1,const char* tail=nullptr){
    char cmd[96];uint32_t n=0;
    auto put=[&](const char* t){while(*t&&n+1<sizeof(cmd))cmd[n++]=*t++;};
    auto hexv=[&](uint32_t v){char t[10];int i=9;t[i]=0;do{t[--i]="0123456789abcdef"[v&15];v>>=4;}while(v);put(" ");put(t+i);};
    put("bridge hardware nv-aux ");put(verb);hexv(a);if(b>=0)hexv(uint32_t(b));if(c>=0)hexv(uint32_t(c));
    if(tail){put(" ");put(tail);}cmd[n]=0;
    return NvAuxHandle(cmd,g_dpIdle.lastReply,sizeof(g_dpIdle.lastReply));
}
// AUX work runs outside g_dpIdleLock (it sleeps); only the worker acts on
// the sink, the command only changes configuration and requests.
bool DpIdleBlank(uint32_t ch){
    KPrintf("dp-idle: blank: D3 on aux %u\n",ch);
    if(DpIdleAux("power",ch,-1,-1,"d3")&&Find(g_dpIdle.lastReply,"\"dpcd600\":2")){
        KPrintf("dp-idle: sink on aux %u -> D3\n",ch);return true;}
    KPrintf("dp-idle: D3 failed: %s\n",g_dpIdle.lastReply);return false;
}
bool DpIdleWake(uint32_t ch,uint32_t sor,uint32_t link){
    for(int attempt=0;attempt<3;++attempt){
        KPrintf("dp-idle: wake attempt %d: D0\n",attempt+1);
        (void)DpIdleAux("power",ch,-1,-1,"d0");
        KPrintf("dp-idle: D0 reply %s\n",g_dpIdle.lastReply);
        SchedulerSleepMs(200);
        KPrintf("dp-idle: retrain\n");
        if(DpIdleAux("retrain",ch,int(sor),int(link))&&JsonOne(g_dpIdle.lastReply,"\"eq\"")){
            KPrintf("dp-idle: sink on aux %u woke (attempt %d)\n",ch,attempt+1);return true;}
    }
    KPrintf("dp-idle: wake failed: %s\n",g_dpIdle.lastReply);return false;
}
// AUX controller sink-detect (stat bit 28, tracks HPD); one read-only MMIO
// read, no AUX transaction.
bool DpIdleSinkPresent(uint32_t ch){
    if(!g_nvMapDone||ch>7)return false;
    const volatile uint32_t* reg=reinterpret_cast<const volatile uint32_t*>(g_nvAuxWin.raw());
    return (reg[(0xd958+ch*0x50-0xd000)/4]&0x10000000)!=0;
}
uint8_t HexByte(const char* t){
    auto v=[](char c)->uint8_t{return uint8_t(c>='a'?c-'a'+10:c>='A'?c-'A'+10:c-'0');};
    return uint8_t(v(t[0])<<4|v(t[1]));
}
// True when DPCD 0x202-0x204 show CR, EQ and symbol lock on all four lanes
// and inter-lane alignment. false also when the read fails.
bool DpIdleLinkUp(uint32_t ch){
    if(!DpIdleAux("read",ch,0x202,3))return false;
    const char* h=Find(g_dpIdle.lastReply,"\"hex\":\"");
    if(!h)return false;h+=7;
    for(int i=0;i<6;++i){const char c=h[i];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;}
    return HexByte(h)==0x77&&HexByte(h+2)==0x77&&(HexByte(h+4)&1);
}
void DpIdleWorker(void*){
    for(int i=0;i<10&&!NvAuxMapWindows();++i)SchedulerSleepMs(100);
    KPrintf("dp-idle: worker up, windows %s\n",g_nvMapDone?"mapped":"NOT mapped");
    // Marker autostart: wait for the root filesystem, read it once.
    for(int i=0;i<120&&!g_dpIdle.seconds;++i){
        if(auto* file=VfsOpen("/etc/dp-idle-seconds",VFS_O_STREAM)){
            char data[40]{};uint64_t off=0;VnodeStat st{};
            if(!VfsStat(file,&st)&&!st.isDir&&st.size&&st.size<sizeof(data)&&VfsRead(file,data,st.size,&off)==int(st.size)){
                const char* q=data;uint32_t secs=0,ch=3,sor=1,link=2;
                if(Decimal(q,secs)&&secs>=10&&secs<=86400){
                    while(*q==' ')++q;
                    if(*q>='0'&&*q<='9'){Decimal(q,ch);while(*q==' ')++q;Decimal(q,sor);while(*q==' ')++q;Decimal(q,link);}
                    if(ch<=7&&sor<=3&&(link==1||link==2)){
                        SpinLockAcquire(&g_dpIdleLock);
                        g_dpIdle.ch=ch;g_dpIdle.sor=sor;g_dpIdle.link=link;g_dpIdle.seconds=secs;g_dpIdle.watch=true;
                        g_dpIdle.seq=InputLatestSequence();g_dpIdle.lastActivityMs=ApicTickCount();
                        SpinLockRelease(&g_dpIdleLock);
                        KPrintf("dp-idle: marker: %u s, aux %u SOR%u link %u\n",secs,ch,sor,link);}
                }
                if(!g_dpIdle.seconds)KPrintf("dp-idle: invalid /etc/dp-idle-seconds; idle-off disabled\n");
            }
            VfsRelease(file);break;
        }
        SchedulerSleepMs(1000);
    }
    for(;;){
        SchedulerSleepMs(g_dpIdle.blanked?20:250);
        SpinLockAcquire(&g_dpIdleLock);
        const uint64_t now=ApicTickCount(),seq=InputLatestSequence();
        if(seq!=g_dpIdle.seq){g_dpIdle.seq=seq;g_dpIdle.lastActivityMs=now;if(g_dpIdle.blanked)g_dpIdle.wakeRequested=true;
            else if(g_dpIdle.watch)g_dpIdle.linkCheckPending=true;}
        if(g_dpIdle.watch){const bool sink=DpIdleSinkPresent(g_dpIdle.ch);
            if(sink!=g_dpIdle.sinkPresent)KPrintf("dp-idle: link-watch: sink on aux %u %s\n",g_dpIdle.ch,sink?"present (hotplug)":"gone");
            if(sink&&!g_dpIdle.sinkPresent){++g_dpIdle.hotplugs;g_dpIdle.linkCheckPending=true;g_dpIdle.linkCheckLog=true;
                g_dpIdle.edgeMs=now;g_dpIdle.recheckStage=0;}
            g_dpIdle.sinkPresent=sink;
            // A sink's lane status can still read "locked" right after the
            // edge (stale), so re-check after the edge, then every 5 s.
            static const uint32_t recheck[3]={1000,3000,10000};
            if(sink&&!g_dpIdle.blanked){
                if(g_dpIdle.recheckStage<3&&now-g_dpIdle.edgeMs>=recheck[g_dpIdle.recheckStage]){
                    ++g_dpIdle.recheckStage;g_dpIdle.linkCheckPending=true;g_dpIdle.linkCheckLog=true;
                    g_dpIdle.linkCheckForce=true;} // post-edge re-checks skip the 2 s rate limit
                else if(now-g_dpIdle.lastLinkCheckMs>=5000)g_dpIdle.linkCheckPending=true;
            }}
        int action=0;
        // Link watch: at most every 2 s; after 5 failed retrains in a row, every 30 s.
        const uint64_t linkGap=g_dpIdle.linkFails>=5?30000:2000;
        if(g_dpIdle.handTest){}
        else if(!g_dpIdle.blanked&&g_dpIdle.linkCheckPending&&g_dpIdle.sinkPresent&&
                ((g_dpIdle.linkCheckForce&&g_dpIdle.linkFails<5)||now-g_dpIdle.lastLinkCheckMs>=linkGap)){
            action=3;g_dpIdle.linkCheckPending=false;g_dpIdle.linkCheckForce=false;g_dpIdle.lastLinkCheckMs=now;}
        const bool logCheck=action==3&&g_dpIdle.linkCheckLog;if(action==3)g_dpIdle.linkCheckLog=false;
        else if(g_dpIdle.blanked&&(g_dpIdle.wakeRequested||!g_dpIdle.seconds)){
            if(!g_dpIdle.wakeFails||now-g_dpIdle.lastWakeTryMs>=1000){action=2;g_dpIdle.lastWakeTryMs=now;}}
        else if(!g_dpIdle.blanked&&g_dpIdle.seconds&&now-g_dpIdle.lastActivityMs>=uint64_t(g_dpIdle.seconds)*1000)action=1;
        const uint32_t ch=g_dpIdle.ch,sor=g_dpIdle.sor,link=g_dpIdle.link;
        SpinLockRelease(&g_dpIdleLock);
        if(!action)continue;
        if(action==3){
            // Head active is enforced by retrain itself (refuses unless the
            // SOR's ARM state is an active DP link). D0 first: a replugged
            // or switched-back sink may come up in any power state.
            int result=0; // 0 link up, 1 retrained, 2 failed
            const bool up=DpIdleLinkUp(ch);
            if(logCheck||!up)KPrintf("dp-idle: link-watch check aux %u: %s (%s)\n",ch,up?"up":"DOWN",g_dpIdle.lastReply);
            if(!up){
                KPrintf("dp-idle: link down on aux %u (%s); retraining\n",ch,g_dpIdle.lastReply);
                result=DpIdleWake(ch,sor,link)?1:2;
            }
            SpinLockAcquire(&g_dpIdleLock);
            if(result)KPrintf("dp-idle: link-watch retrain %s\n",result==1?"ok":"FAILED");
            if(result==1){++g_dpIdle.linkRetrains;g_dpIdle.linkFails=0;}
            else if(result==2){++g_dpIdle.linkFails;g_dpIdle.linkCheckPending=true;}
            else g_dpIdle.linkFails=0;
            SpinLockRelease(&g_dpIdleLock);
            continue;
        }
        const bool done=action==1?DpIdleBlank(ch):DpIdleWake(ch,sor,link);
        SpinLockAcquire(&g_dpIdleLock);
        if(action==1){if(done){g_dpIdle.blanked=true;++g_dpIdle.blanks;}
            else g_dpIdle.lastActivityMs=ApicTickCount(); // retry after a full timeout
        }else if(done){g_dpIdle.blanked=false;g_dpIdle.wakeRequested=false;g_dpIdle.wakeFails=0;++g_dpIdle.wakes;
            g_dpIdle.lastActivityMs=ApicTickCount();}
        else ++g_dpIdle.wakeFails;
        SpinLockRelease(&g_dpIdleLock);
    }
}
void DpIdleCommand(const char* p){
    uint32_t secs=0,ch=0,sor=0,link=0;bool ok=true;
    SpinLockAcquire(&g_dpIdleLock);
    if(!strcmp(p,"status")){}
    else if(!strcmp(p,"off")){g_dpIdle.seconds=0;if(g_dpIdle.blanked)g_dpIdle.wakeRequested=true;}
    else if(!strcmp(p,"wake")){if(g_dpIdle.blanked)g_dpIdle.wakeRequested=true;}
    else if(!strcmp(p,"test-blank")||!strcmp(p,"test-wake")){
        // Hand-driven: run the worker's own blank/wake functions on the bridge
        // thread (worker must be idle: seconds 0 and nothing pending).
        const bool blank=p[5]=='b';
        if(g_dpIdle.seconds||g_dpIdle.wakeRequested){ok=false;}
        else{const uint32_t tch=g_dpIdle.ch,tsor=g_dpIdle.sor,tlink=g_dpIdle.link;
            g_dpIdle.handTest=true;SpinLockRelease(&g_dpIdleLock);
            const bool done=blank?DpIdleBlank(tch):DpIdleWake(tch,tsor,tlink);
            SpinLockAcquire(&g_dpIdleLock);g_dpIdle.handTest=false;ok=done;
            if(done){g_dpIdle.blanked=blank;if(blank)++g_dpIdle.blanks;else ++g_dpIdle.wakes;}}
    }
    else if(Prefix(p,"set ")){p+=4;
        ok=Decimal(p,secs)&&secs>=10&&secs<=86400;while(ok&&*p==' ')++p;
        ok=ok&&Decimal(p,ch)&&ch<=7;while(ok&&*p==' ')++p;
        ok=ok&&Decimal(p,sor)&&sor<=3;while(ok&&*p==' ')++p;
        ok=ok&&Decimal(p,link)&&(link==1||link==2)&&!*p;
        if(ok){if(g_dpIdle.blanked&&ch!=g_dpIdle.ch)ok=false;
            else{g_dpIdle.seconds=secs;g_dpIdle.ch=ch;g_dpIdle.sor=sor;g_dpIdle.link=link;g_dpIdle.watch=true;
                g_dpIdle.seq=InputLatestSequence();g_dpIdle.lastActivityMs=ApicTickCount();}}
    }
    else ok=false;
    const bool started=g_dpIdle.threadStarted;
    Reply r;r.text("{\"dp_idle_api\":1,\"ok\":");r.text(ok?"true":"false");
    r.text(",\"thread\":");r.text(started?"true":"false");
    r.text(",\"blanked\":");r.text(g_dpIdle.blanked?"true":"false");
    r.text(",");r.field("seconds",g_dpIdle.seconds);r.text(",");r.field("ch",g_dpIdle.ch);
    r.text(",");r.field("sor",g_dpIdle.sor);r.text(",");r.field("link",g_dpIdle.link);
    r.text(",");r.field("idle_ms",ApicTickCount()-g_dpIdle.lastActivityMs);
    r.text(",");r.field("blanks",g_dpIdle.blanks);r.text(",");r.field("wakes",g_dpIdle.wakes);
    r.text(",");r.field("wake_fails",g_dpIdle.wakeFails);
    r.text(",\"watch\":");r.text(g_dpIdle.watch?"true":"false");r.text(",\"sink\":");r.text(g_dpIdle.sinkPresent?"true":"false");
    r.text(",");r.field("hotplugs",g_dpIdle.hotplugs);r.text(",");r.field("link_retrains",g_dpIdle.linkRetrains);
    r.text(",");r.field("link_fails",g_dpIdle.linkFails);r.text(",\"last\":");r.quoted(g_dpIdle.lastReply);r.text("}\n");
    SpinLockRelease(&g_dpIdleLock);
    r.send();
}
}
SpinLock* NvAuxSharedLock(){return &g_nvAuxLock;}
// nv-disp head1 sessions suspend dp-idle (idle-off and link watch) and restore it.
// Plain atomic stores, no g_dpIdleLock: the caller already holds the nv-aux lock.
void DpIdleSuspendForDisplay(bool suspend){
    static uint32_t savedSecs=0;static bool savedWatch=false;
    if(suspend){savedSecs=__atomic_load_n(&g_dpIdle.seconds,__ATOMIC_RELAXED);savedWatch=__atomic_load_n(&g_dpIdle.watch,__ATOMIC_RELAXED);
        __atomic_store_n(&g_dpIdle.seconds,0u,__ATOMIC_RELEASE);__atomic_store_n(&g_dpIdle.watch,false,__ATOMIC_RELEASE);}
    else{__atomic_store_n(&g_dpIdle.lastActivityMs,ApicTickCount(),__ATOMIC_RELEASE);
        __atomic_store_n(&g_dpIdle.watch,savedWatch,__ATOMIC_RELEASE);__atomic_store_n(&g_dpIdle.seconds,savedSecs,__ATOMIC_RELEASE);}
}
// Pre-reset GPU reset. A warm reset does not reset the display engine, and
// after nv-disp / nv-aux display writes the next POST hung with no video.
// If such a write happened this boot, the last quiesce step resets the GPU:
// FLR on fn0 when DevCap advertises it, else a Secondary Bus Reset on the
// upstream port. Registered at boot so it runs after nv-disp's own abort.
static volatile bool g_nvDisplayWritten=false;
extern "C" void NvDisplayMarkWritten(){__atomic_store_n(&g_nvDisplayWritten,true,__ATOMIC_RELEASE);}
static void NvDelayUs(uint32_t us){
    if(TscClockAvailable()){const uint64_t end=TscClockReadNs()+uint64_t(us)*1000;while(TscClockReadNs()<end)__asm__ volatile("pause");}
    else{const uint64_t end=ApicTickCount()+us/1000+1;while(ApicTickCount()<end)__asm__ volatile("pause");}
}
static void NvGpuResetQuiesce(){
    if(!__atomic_load_n(&g_nvDisplayWritten,__ATOMIC_ACQUIRE))return;
    PciDevice gpu{};bool found=false;
    for(uint32_t i=0;i<PciBootInventoryCount()&&!found;++i){PciBootEntry e{};
        if(PciBootInventoryRead(i,&e)&&e.device.vendorId==0x10de&&e.device.classCode==0x03&&e.device.fn==0){gpu=e.device;found=true;}}
    if(!found)return;
    // PCIe capability (id 0x10) on fn0.
    uint8_t cap=uint8_t(PciConfigRead32(gpu.bus,gpu.dev,0,0x34)&0xfc);
    for(int n=0;cap&&n<48;++n){const uint32_t h=PciConfigRead32(gpu.bus,gpu.dev,0,cap);if((h&0xff)==0x10)break;cap=uint8_t((h>>8)&0xfc);}
    if(cap&&(PciConfigRead32(gpu.bus,gpu.dev,0,uint8_t(cap+4))&(1u<<28))){
        KPrintf("power: GPU %02x:%02x.0 display was written this boot; FLR before reset\n",gpu.bus,gpu.dev);
        PciConfigWrite16(gpu.bus,gpu.dev,0,uint8_t(cap+8),uint16_t(PciConfigRead16(gpu.bus,gpu.dev,0,uint8_t(cap+8))|0x8000));
        NvDelayUs(100000);
        KPrintf("power: GPU FLR done\n");return;
    }
    for(uint32_t i=0;i<PciBootInventoryCount();++i){PciBootEntry e{};
        if(!PciBootInventoryRead(i,&e)||(e.headerType&0x7f)!=1)continue;
        const auto& b=e.device;
        if(((PciConfigRead32(b.bus,b.dev,b.fn,0x18)>>8)&0xff)!=gpu.bus)continue;
        KPrintf("power: GPU %02x:%02x.0 display was written this boot; SBR on %02x:%02x.%u before reset\n",
                gpu.bus,gpu.dev,b.bus,b.dev,b.fn);
        const uint16_t bc=PciConfigRead16(b.bus,b.dev,b.fn,0x3e);
        PciConfigWrite16(b.bus,b.dev,b.fn,0x3e,uint16_t(bc|0x40));
        NvDelayUs(2000); // hold the reset >= 1 ms
        PciConfigWrite16(b.bus,b.dev,b.fn,0x3e,uint16_t(bc&~0x40));
        KPrintf("power: GPU SBR done\n");return;
    }
    KPrintf("power: GPU reset skipped (no FLR, no upstream bridge found)\n");
}
void DpIdleStart(){
    if(g_dpIdle.threadStarted)return;
    bool nvidia=false;
    for(uint32_t i=0;i<PciBootInventoryCount()&&!nvidia;++i){PciBootEntry e{};
        if(PciBootInventoryRead(i,&e)&&e.device.vendorId==0x10de&&e.device.classCode==0x03)nvidia=true;}
    if(!nvidia)return;
    (void)PlatformPowerAddQuiesce(NvGpuResetQuiesce);
    Process* t=KernelThreadCreate("dp_idle",DpIdleWorker,nullptr,3);
    if(!t){KPrintf("dp-idle: thread start failed\n");return;}
    g_dpIdle.threadStarted=true;SchedulerAddProcess(t);
}
#else
void DpIdleStart(){}
#endif
static uint32_t (*volatile g_nicCommand)(const char*,char*,uint32_t)=nullptr;
static uint32_t (*volatile g_usbCommand)(const char*,char*,uint32_t)=nullptr;
bool DebugHardwareHandleCommand(const char* command,uint32_t length){
    constexpr const char* guard="bridge guard ";
    if(Prefix(command,guard)){
        const char* p=command+13;char boot[33]{};
        for(unsigned i=0;i<32;++i){if(!p[i]){DebugChannelSend("{\"boot_error\":\"Invalid boot identity\"}\n");return true;}boot[i]=p[i];}
        if(p[32]!=' ' || !p[33] || !DebugEpochMatches(boot)){
            DebugChannelSend("{\"boot_error\":\"Guest boot changed; attach explicitly again\"}\n");return true;
        }
        const char* body=p+33;
        if(*body=='@' || Prefix(body,guard)){
            DebugChannelSend("{\"boot_error\":\"Nested request envelopes are unsupported\"}\n");return true;
        }
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
        if (!strcmp(body, "bridge slot status")) {
            RecoverySlotsReply();
            return true;
        }
#endif
        DebugHandleCommand(body,length-46);return true;
    }
    if(!strcmp(command,"bridge hardware caps")){
        char boot[33];DebugEpochCopy(boot);
        Reply r;r.text("{\"hardware_api\":1,\"boot_guard\":true,\"logs_api\":1,\"boot_id\":");r.quoted(boot);
#if defined(BROOK_BRIDGE_V3)
        r.text(",\"auth_version\":3,\"auth_suite\":1,\"auth_ready\":");
        r.text(DebugBridgeV3Available()?"true":"false");
        r.text(",");r.field("operator_keys",DebugBridgeV3OperatorKeyCount());
        if(DebugChannelReplyAuthenticated()) {
            r.text(",\"operator_policy_sha256\":");r.quoted(DebugBridgeV3PolicySha256());
            const char* initrd=DebugBridgeV3InitrdSha256();
            r.text(",\"initrd_sha256\":");if(*initrd)r.quoted(initrd);else r.text("null");
            r.text(",\"initrd_sha256_source\":");
            r.quoted(*initrd?"bootloader_ram_image":"unavailable_no_bootloader_initrd");
            r.text(",\"kernel_sha256\":null,\"kernel_sha256_source\":\"unavailable_loader_elf_digest\"");
        }
#endif
        r.text(",\"backlight_api\":1,\"pci_inventory_api\":1,\"build_id\":");r.quoted(BuildGitHash());r.text(",\"build_date\":");r.quoted(BuildDate());
        HardwarePayloadId(r);
        r.text(",");r.field("logical_cpus",SmpGetCpuCount());
        r.text(",\"audio_device\":");r.text(AudioAvailable()?"true":"false");
        const auto ac=ShutdownGetAcStatus();
        r.text(",\"ac_recovery\":{\"api\":1,\"enabled\":");r.text(ac.enabled?"true":"false");
        r.text(",\"ready\":");r.text(ac.ready?"true":"false");
        r.text(",\"requested\":");r.text(ac.requested?"true":"false");
        r.text(",\"source\":");
        r.quoted(ac.source==AcPowerSource::Online?"online":ac.source==AcPowerSource::Offline?"offline":"unknown");
        r.text(",\"sample_age_ms\":");
        if(ac.sampleAgeMs==~uint64_t{0})r.text("null");else r.number(ac.sampleAgeMs);
        r.text("}");
#if defined(BROOK_SSD_GUARDED_DESKTOP)
        AcShutdownRecord prior{};
        r.text(",\"ac_shutdown_record\":");
        if(GuardCheckedAcShutdownPrior(prior)) {
            r.text("{\"boot_id\":");r.quoted(prior.bootId);
            r.text(",\"stage\":\"checked_finalization\",");r.field("sequence",prior.sequence);r.text("}");
        }else r.text("null");
#endif
        const auto snapshot=CompressedVfsAppSnapshotInfo();
        r.text(",\"app_snapshot_api\":1,\"app_snapshot\":{\"configured\":");r.text(snapshot.configured?"true":"false");
        r.text(",\"ready\":");r.text(snapshot.ready?"true":"false");
        r.text(",\"source_closed\":");r.text(snapshot.sourceClosed?"true":"false");
        r.text(",");r.field("bytes",snapshot.bytes);r.text(",");r.field("error",uint32_t(snapshot.error));
        r.text(",\"source\":");r.quoted(snapshot.source);r.text("}");
        const auto store=WritableDesktopRead();
        r.text(",\"writable_desktop_api\":1,\"writable_desktop\":{\"configured\":");r.text(store.configured?"true":"false");
        r.text(",\"ready\":");r.text(store.ready?"true":"false");r.text(",\"source_pinned\":");r.text(store.pinned?"true":"false");
        r.text(",\"stage\":");r.quoted(store.stage);
        r.text(",\"dirty_session\":");r.text(store.dirty?"true":"false");r.text(",\"clean_finalized\":");r.text(store.clean?"true":"false");
        r.text(",\"state_uncertain\":");r.text(store.stateUncertain?"true":"false");
        r.text(",");r.field("bytes",store.bytes);r.text(",");r.field("drive",store.drive);
        auto signedNumber=[&](int value){if(value<0){r.text("-");r.number(-int64_t(value));}else r.number(value);};
        r.text(",\"error\":");signedNumber(store.error);r.text(",\"final_error\":");signedNumber(store.finalError);r.text("}");
        const auto usb=UsbSystemRead();
        if(usb.configured) {
            auto boolean=[&](const char* key,bool value) {
                r.text(",");r.quoted(key);r.text(value?":true":":false");
            };
            r.text(",\"usb_system\":{\"configured\":true");
            boolean("record_valid",usb.parsed);boolean("window_open",usb.window);
            boolean("mounted",usb.mounted);boolean("dirty",usb.dirty);boolean("clean",usb.clean);
            boolean("uncertain",usb.uncertain);boolean("final_sync_attempted",usb.finalAttempted);
            r.text(",\"stage\":");r.quoted(usb.stage);
            r.text(",\"disk\":");signedNumber(usb.disk);
            r.text(",\"error\":");signedNumber(usb.error);
            r.text(",\"final_error\":");signedNumber(usb.finalError);
            r.text(",\"partition_guid_gpt_hex\":");r.quoted(usb.partitionGuid);
            r.text(",\"fs_uuid_hex\":");r.quoted(usb.fsUuid);
            r.text(",");r.field("first_lba",usb.firstLba);r.text(",");r.field("sectors",usb.sectors);
            r.text(",");r.field("reads",usb.reads);r.text(",");r.field("writes",usb.writes);
            r.text(",");r.field("flushes",usb.flushes);r.text(",");r.field("errors",usb.errors);
            UsbStorageTiming timing{};
            if(UsbSystemTiming(&timing)) {
                r.text(",\"timing\":{");r.field("commands",timing.commands);
                r.text(",");r.field("errors",timing.errors);
                r.text(",");r.field("data_bytes",timing.dataBytes);
                r.text(",");r.field("last_bytes",timing.lastBytes);
                r.text(",");r.field("direct_bytes",timing.directBytes);
                r.text(",");r.field("uas_flights",timing.uasFlights);
                r.text(",");r.field("uas_peak_depth",timing.uasPeakDepth);
                auto phase=[&](const char* label,const UsbPhaseTiming& p) {
                    r.text(",");r.quoted(label);r.text(":{");
                    r.field("total_ns",p.totalNs);r.text(",");r.field("max_ns",p.maxNs);
                    r.text(",");r.field("last_ns",p.lastNs);r.text("}");
                };
                phase("cbw",timing.command);phase("data",timing.data);phase("csw",timing.status);
                constexpr const char* roots[]={"boot","file","exec","fault","inode","indirect","directory","bitmap","bench","other"};
                static_assert(sizeof(roots)/sizeof(*roots)==UsbReadSourceCount);
                r.text(",\"read_request_max_bytes\":[512,4096,16384,65536,2147483647],\"read_sources\":{");
                for(unsigned k=0;k<UsbReadSourceCount;++k) {
                    if(k)r.text(",");
                    r.quoted(roots[k]);r.text(":{");
                    r.field("bytes",timing.readsBySource[k].bytes);r.text(",\"requests\":[");
                    for(unsigned j=0;j<UsbReadBuckets;++j) {
                        if(j)r.text(",");
                        r.number(timing.readsBySource[k].requests[j]);
                    }
                    r.text("]}");
                }
                r.text("},\"mount_timing_us\":{");
                r.field("identity",timing.admissionUs);r.text(",");
                r.field("transport",timing.transportUs);r.text(",");
                r.field("transport_identity",timing.transportIdentityUs);r.text(",");
                r.field("uas_drop_bot",timing.uasDropBotUs);r.text(",");
                r.field("uas_set_interface",timing.uasSetInterfaceUs);r.text(",");
                r.field("uas_configure",timing.uasConfigureUs);r.text(",");
                r.field("uas_identify",timing.uasIdentifyUs);r.text(",");
                r.field("session",timing.sessionUs);r.text(",");
                r.field("mount",timing.mountUs);r.text("}");
                r.text("}");
            }
            r.text("}");
        }
#if defined(BROOK_SSD_AUTO_RECOVERY)
#if defined(BROOK_SSD_GUARDED_DESKTOP)
        if(GuardedDesktopBridgeEnabled()) DesktopCaps(r);
        else
#endif
        r.text(",\"power_api\":0,\"recovery_only\":true,\"recovery_panic_api\":1,\"completed_frame_copy_api\":0,\"bulk_helper_api\":0,\"panic_transport\":false,\"raw_frame_transfer\":false");
#else
        DesktopCaps(r);
#endif
        r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware usb-reset")){
        UsbBootResetReport report{};const bool available=UsbBootReadReset(report);
        Reply r;r.text("{\"usb_reset_api\":1,\"available\":");r.text(available?"true":"false");
        if(available){r.text(",");r.field("vendor",report.vendor);r.text(",");r.field("device",report.device);
            r.text(",");r.field("phase",report.phase);r.text(",");r.field("error",report.error);
            r.text(",");r.field("command",report.command);r.text(",");r.field("status",report.status);
            r.text(",");r.field("status_now",report.statusNow);r.text(",\"status_now_valid\":");r.text(report.statusNowValid?"true":"false");
            r.text(",");r.field("elapsed_ms",report.elapsedMs);}
        r.text("}\n");r.send();return true;
    }
#if defined(BROOK_SSD_AUTO_RECOVERY) || !defined(BROOK_SSD_GUARD_CHECK)
    constexpr char priorPrefix[]="bridge hardware recovery-panic ";
    constexpr char olderPrefix[]="bridge hardware recovery-panic-previous ";
    const bool older=Prefix(command,olderPrefix);
    if(older || Prefix(command,priorPrefix)) {
        const char* p=command+(older?sizeof(olderPrefix)-1:sizeof(priorPrefix)-1);uint32_t offset=0,count=0;
        if(!Decimal(p,offset) || *p++!=' ' || !Decimal(p,count) || *p || count>512) {
            DebugChannelSend("{\"recovery_panic_error\":\"Expected offset and byte count 0..512\"}\n");return true;
        }
        GuardPanicSnapshot snapshot{};
        if(!(older?GuardRecoveryPanicPreviousSnapshot(snapshot):GuardRecoveryPanicSnapshot(snapshot))) {
            DebugChannelSend("{\"recovery_panic_error\":\"No validated panic record\"}\n");return true;
        }
        if(offset>snapshot.bytes || count>snapshot.bytes-offset) {
            DebugChannelSend("{\"recovery_panic_error\":\"Invalid byte range\"}\n");return true;
        }
        Reply r;r.text("{\"recovery_panic_api\":1,");r.field("sequence",snapshot.sequence);
        r.text(",");r.field("bytes",snapshot.bytes);r.text(",");r.field("crc32",snapshot.crc);
        r.text(",");r.field("offset",offset);r.text(",\"hex\":\"");
        const char* hex="0123456789abcdef";for(uint32_t i=0;i<count;++i){const auto b=snapshot.data[offset+i];r.append(hex[b>>4]);r.append(hex[b&15]);}
        r.text("\"}\n");r.send();return true;
    }
#endif
    constexpr char pciPrefix[]="bridge hardware pci ";
    if(!strcmp(command,"bridge hardware dell-i2c-id start")) {
        uint32_t value=0;const auto result=DellI2cIdProbe(value);Reply r;r.text("{\"dell_i2c_id_api\":1,\"result\":");
        r.quoted(I2cProbeResult(result));
        if(result==DellI2cIdResult::Identified || result==DellI2cIdResult::Unknown){r.text(",");r.field("value",value);}
        r.text(",\"pci_extent_ownership\":\"unresolved\"}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware dell-i2c-id status")) {
        const auto status=DellI2cIdProbeStatus();Reply r;r.text("{\"dell_i2c_id_api\":1,\"started\":");r.text(status.started?"true":"false");
        r.text(",\"completed\":");r.text(status.completed?"true":"false");
        if(status.completed){r.text(",\"result\":");r.quoted(I2cProbeResult(status.result));
            if(status.result==DellI2cIdResult::Identified || status.result==DellI2cIdResult::Unknown){r.text(",");r.field("value",status.value);}}
        r.text(",\"pci_extent_ownership\":\"unresolved\"}\n");r.send();return true;
    }
#if !defined(BROOK_HOST_TEST)
    if(!strcmp(command,"bridge hardware clock-compare")) {
        // Read-only: one adjacent sample of the clock applications see
        // (monotonic_ns is CLOCK_MONOTONIC; monotonic_ms is ApicTickCount(),
        // both invariant-TSC derived when clock_source is "tsc"), the raw
        // native TSC scaled by its observed calibration, and the admitted Dell
        // HDA drain clock. Two samples bracketed by host time give each rate.
        const auto& tsc=NativeTscObserveRead();uint32_t lo=0,hi=0,aux=0;
        const uint64_t monotonic=ElapsedClockNs();
        const uint64_t ticks=ApicTickCount(),raw=ApicRawBspIrqCount();
        __asm__ volatile("rdtscp; lfence":"=a"(lo),"=d"(hi),"=c"(aux)::"memory");
        const uint64_t cycles=(uint64_t(hi)<<32)|lo,drain=HdaRuntimeDrainClockNs();
        const uint64_t tscNs=tsc.consistent && tsc.khz?cycles/tsc.khz*1000000ULL+cycles%tsc.khz*1000000ULL/tsc.khz:0;
        Reply r;r.text("{\"clock_compare_api\":1,");r.field("monotonic_ms",ticks);r.text(",");r.field("bsp_irqs",raw);
        r.text(",");r.field("tsc",cycles);r.text(",");r.field("tsc_khz",tsc.consistent?tsc.khz:0u);r.text(",");r.field("tsc_ns",tscNs);
        r.text(",");r.field("hda_drain_ns",drain);r.text(",");r.field("cpu",aux&0xfff);
        const auto clock=TscClockReadStatus();
        r.text(",");r.field("monotonic_ns",monotonic);r.text(",\"clock_source\":");r.quoted(ClockSourceName(clock.kind));
        r.text(",");r.field("clock_tsc_khz",clock.khz);
        r.text(",\"scope\":\"adjacent reads, not simultaneous; tsc_ns uses the boot PIT calibration\"}\n");r.send();return true;
    }
#endif
    if(!strcmp(command,"bridge hardware native-tsc-observe")) {
        const auto& o=NativeTscObserveRead();Reply r;r.text("{\"native_tsc_observe_api\":1,\"requested\":");r.text(o.requested?"true":"false");
        r.text(",\"attempted\":");r.text(o.attempted?"true":"false");r.text(",\"capable\":");r.text(o.capable?"true":"false");
        r.text(",\"measured\":");r.text(o.measured?"true":"false");r.text(",\"consistent\":");r.text(o.consistent?"true":"false");
        r.text(",");r.field("khz",o.khz);r.text(",");r.field("pit_ticks",o.pitTicks);r.text(",\"samples\":[");
        for(unsigned i=0;i<5;++i){if(i)r.text(",");r.number(o.samples[i]);}
        r.text("],");r.field("cores_expected",o.coreExpected);r.text(",\"cores\":[");for(uint32_t i=0;i<o.coreCount && i<63;++i){if(i)r.text(",");const auto& c=o.cores[i];
            r.text("{");r.field("cpu",c.cpu);r.text(",");r.field("result",uint8_t(c.result));r.text(",");r.field("exchanges",c.exchanges);
            r.text(",\"lower\":");if(c.interval.lower<0){r.text("-");r.number(uint64_t(-(c.interval.lower+1))+1);}else r.number(uint64_t(c.interval.lower));
            r.text(",\"upper\":");if(c.interval.upper<0){r.text("-");r.number(uint64_t(-(c.interval.upper+1))+1);}else r.number(uint64_t(c.interval.upper));
            r.text(",");r.field("round_trip",c.interval.roundTrip);r.text("}");}
        r.text("],\"reason\":");r.quoted(o.reason);r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware dell-hda-cache")) {
        const auto& o=HdaObserveRead();Reply r;r.text("{\"hda_cache_api\":1,\"requested\":");r.text(o.cacheRequested?"true":"false");
        r.text(",\"captured\":");r.text(o.cacheCaptured?"true":"false");r.text(",\"match\":");r.text(o.cacheMatch?"true":"false");
        r.text(",");r.field("cpus",o.cacheCpuCount);r.text(",");r.field("pat",o.cachePat);
        r.text(",\"pages_requested\":");r.text(o.cachePagesRequested?"true":"false");
        r.text(",\"mmio_mapping\":");r.text(o.cacheMmioMapping?"true":"false");
        r.text(",\"dma_mappings\":");r.text(o.cacheDmaMappings?"true":"false");
        r.text(",\"page_types\":");r.text(o.cachePageTypes?"true":"false");r.text(",");r.field("pages",o.cachePages);
        r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware dell-hda-queue")) {
        const auto q=HdaRuntimeQueueDiagnostics();Reply r;
        r.text("{\"hda_queue_api\":1,\"enabled\":");r.text(q.enabled?"true":"false");
        r.text(",\"saturated\":");r.text(q.saturated?"true":"false");
        r.text(",");r.field("starts",q.starts);r.text(",");r.field("stops",q.stops);
        r.text(",");r.field("faults",q.faults);r.text(",");r.field("startup_gap_bytes",q.startupGapBytes);
        r.text(",");r.field("later_gap_events",q.laterGapEvents);r.text(",");r.field("later_gap_bytes",q.laterGapBytes);
        r.text(",");r.field("empty_before_bytes",q.emptyBeforeBytes);r.text(",");r.field("empty_after_bytes",q.emptyAfterBytes);
        r.text(",");r.field("published_bytes",q.publishedBytes);r.text(",");r.field("backpressure",q.backpressure);
        r.text(",");r.field("publication_failures",q.publicationFailures);
        const auto faults=HdaRuntimeFaults();r.text(",");r.field("fault_count",faults.count);
        r.text(",");HdaFaultJson(r,"first_fault",faults.first);r.text(",");HdaFaultJson(r,"last_fault",faults.last);
        r.text(",\"scope\":\"boot-opt-in queue observations under the driver lock; reserved gaps include future guard space and overlap empty-tail advances; neither proves source starvation or audible loss; counters persist across epochs\"}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware dell-hda-runtime")) {
        const auto& o=HdaObserveRead();Reply r;r.text("{\"hda_runtime_api\":1,\"requested\":");r.text(o.runtimeRequested?"true":"false");
        r.text(",\"prepared\":");r.text(o.runtimePrepared?"true":"false");r.text(",\"registered\":");r.text(o.runtimeRegistered?"true":"false");
        r.text(",\"audible_enabled\":");r.text(o.runtimeAudibleEnabled?"true":"false");
        r.text(",\"quarantined\":");r.text(__atomic_load_n(&o.runtimeQuarantined,__ATOMIC_ACQUIRE)?"true":"false");
        r.text(",");r.field("pages_validated",__atomic_load_n(&o.runtimePagesValidated,__ATOMIC_RELAXED));
        r.text(",");r.field("page_rejections",__atomic_load_n(&o.runtimePageRejections,__ATOMIC_RELAXED));
        r.text(",");r.field("stops",__atomic_load_n(&o.runtimeStops,__ATOMIC_RELAXED));
        r.text(",");r.field("stop_failures",__atomic_load_n(&o.runtimeStopFailures,__ATOMIC_RELAXED));
        r.text(",");r.field("restarts",__atomic_load_n(&o.runtimeRestarts,__ATOMIC_RELAXED));
        r.text(",");r.field("restart_failures",__atomic_load_n(&o.runtimeRestartFailures,__ATOMIC_RELAXED));
        r.text(",");r.field("restart_refused",__atomic_load_n(&o.runtimeRestartRefused,__ATOMIC_RELAXED));
        {const auto faults=HdaRuntimeFaults();const auto& f=faults.last;
            r.text(",");r.field("fault_count",faults.count);
            r.text(",\"fault_stage\":");r.quoted(HdaDellFaultStageName(f.stage));
            r.text(",\"fault_reason\":");r.quoted(HdaDellFaultReasonName(f.reason));
            r.text(",\"fault_sample_reason\":");r.quoted(HdaDellFaultReasonName(f.sampleReason));
            r.text(",");r.field("fault_sdsts",f.atFault.status);r.text(",");r.field("fault_sdctl",f.atFault.control);
            r.text(",\"clock\":{\"failed\":");r.text(faults.clockFailed?"true":"false");
            r.text(",");r.field("fail_reason",faults.clockFailReason);r.text(",");r.field("fail_cpu",faults.clockFailCpu);
            r.text(",");r.field("last_cpu",faults.clockLastCpu);r.text(",");r.field("fail_tsc",faults.clockFailTsc);
            r.text(",");r.field("fail_last_tsc",faults.clockFailLast);r.text(",");r.field("khz",faults.clockKhz);r.text("}");
            r.text(",");HdaRecoveryJson(r,faults);}
        r.text(",");r.field("failure_stage",HdaRuntimeFailureStage());
        r.text(",");r.field("stream_failure_stage",HdaRuntimeStreamFailureStage());
        r.text(",");r.field("stream_failure_detail",HdaRuntimeStreamFailureDetail());
        r.text(",");r.field("delay_interrupts_off",__atomic_load_n(&o.runtimeDelayInterruptsOff,__ATOMIC_RELAXED));
        const bool shutdown=__atomic_load_n(&o.runtimeShutdown,__ATOMIC_ACQUIRE);
        r.text(",\"shutdown\":");r.text(shutdown?"true":"false");
        r.text(",\"shutdown_ok\":");r.text(shutdown && __atomic_load_n(&o.runtimeShutdownOk,__ATOMIC_RELAXED)?"true":"false");
        r.text(",\"reason\":");r.quoted(o.runtimeReason);r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware dell-hda-stream")) {
        const auto& o=HdaObserveRead();Reply r;r.text("{\"hda_stream_api\":1,\"requested\":");r.text(o.streamRequested?"true":"false");
        r.text(",\"attempted\":");r.text(o.streamAttempted?"true":"false");r.text(",\"finished\":");r.text(o.streamFinished?"true":"false");
        r.text(",\"quarantined\":");r.text(o.streamQuarantined?"true":"false");r.text(",");r.field("submissions",o.streamSubmissions);
        r.text(",");r.field("accepted",o.streamAccepted);r.text(",");r.field("completed",o.streamCompleted);
        r.text(",");r.field("elapsed_ns",o.streamElapsedNs);r.text(",");r.field("command",o.streamCommand);
        r.text(",\"reason\":");r.quoted(o.streamReason);r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware dell-hda-observe")) {
        const auto& o=HdaObserveRead();const auto& s=o.session;
        Reply r;r.text("{\"hda_observe_api\":1,\"requested\":");r.text(o.requested?"true":"false");
        r.text(",\"attempted\":");r.text(o.attempted?"true":"false");r.text(",\"mapped\":");r.text(o.mapped?"true":"false");
        r.text(",\"reason\":");r.quoted(o.reason);r.text(",");r.field("base",o.base);
        r.text(",");r.field("result",uint8_t(s.result));r.text(",");r.field("probe",uint8_t(s.probe));
        r.text(",");r.field("command_before",s.before);r.text(",");r.field("command_enabled",s.enabled);r.text(",");r.field("command_after",s.after);
        r.text(",");r.field("gcap",s.controller.gcap);r.text(",");r.field("gctl",s.controller.gctl);r.text(",");r.field("interrupts",s.controller.interrupts);
        r.text(",");r.field("state",s.controller.state);r.text(",");r.field("corb",s.controller.corb);r.text(",");r.field("rirb",s.controller.rirb);
        r.text(",");r.field("immediate",s.controller.immediate);r.text(",");r.field("streams",s.controller.streams);
        r.text(",");r.field("vendor",s.vendor);r.text(",");r.field("revision",s.revision);r.text(",");r.field("subsystem",s.subsystem);
        r.text(",");r.field("completed",s.completed);r.text(",\"values\":[");
        for(unsigned i=0;i<s.completed && i<12;++i){if(i)r.text(",");r.number(s.values[i]);}
        r.text("],\"state_completed\":");r.number(s.stateCompleted);r.text(",\"state_values\":[");
        for(unsigned i=0;i<s.stateCompleted && i<13;++i){if(i)r.text(",");r.number(s.stateValues[i]);}
        r.text("],\"amp_completed\":");r.number(s.ampCompleted);r.text(",\"amp_values\":[");
        for(unsigned i=0;i<s.ampCompleted && i<6;++i){if(i)r.text(",");r.number(s.ampValues[i]);}
        r.text("],\"format_completed\":");r.text(s.formatCompleted?"true":"false");
        r.text(",");r.field("converter_format",s.converterFormat);
        r.text(",\"tone\":{\"requested\":");r.text(o.toneRequested?"true":"false");
        r.text(",\"attempted\":");r.text(o.toneAttempted?"true":"false");
        r.text(",\"audible\":");r.text(o.toneAudible?"true":"false");
        r.text(",\"finished\":");r.text(o.toneFinished?"true":"false");
        r.text(",\"quarantined\":");r.text(o.toneQuarantined?"true":"false");
        r.text(",");r.field("result",o.toneResult);r.text(",");r.field("admission",o.toneAdmission);
        r.text(",");r.field("page",o.tonePage);r.text(",");r.field("page_status",o.tonePageStatus);
        r.text(",");r.field("prepare_stage",o.tonePrepareStage);r.text(",");r.field("prepare_value",o.tonePrepareValue);r.text(",\"fifo_sampled\":");r.text(o.toneFifoSampled?"true":"false");r.text(",");r.field("fifo_size_raw",o.toneFifoSize);
        r.text(",");r.field("stream_stage",o.toneStreamStage);r.text(",");r.field("stream_detail",o.toneStreamDetail);r.text(",");r.field("command",o.toneCommand);
        r.text(",\"ici_failed\":");r.text(o.toneIciFailed?"true":"false");
        r.text(",");r.field("play_reason",o.tonePlayReason);r.text(",");r.field("play_loops",o.tonePlayLoops);r.text(",");r.field("play_progress",o.tonePlayProgress);r.text(",");r.field("play_elapsed",o.tonePlayElapsed);
        r.text(",");r.field("progress_reason",o.toneProgressReason);r.text(",");r.field("progress_status",o.toneProgressStatus);
        r.text(",");r.field("progress_control",o.toneProgressControl);r.text(",");r.field("progress_first",o.toneProgressFirst);r.text(",");r.field("progress_last",o.toneProgressLast);
        r.text(",\"reason\":");r.quoted(o.toneReason);r.text("}}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware dell-hda-memory")) {
        // Fixed identity only, using immutable boot PCI metadata. This reports
        // mappings and memory types; it never enables, maps or reads the HDA.
        PciBootEntry found{};uint64_t base=0;bool present=false;
        for(uint32_t i=0;i<PciBootInventoryCount();++i) {
            PciBootEntry e{};if(!PciBootInventoryRead(i,&e))continue;
            const auto& d=e.device;
            HdaProbePci p{d.vendorId,d.deviceId,e.subsystemVendor,e.subsystemDevice,
                          d.bus,d.dev,d.fn,d.classCode,d.subclass,d.bar[0],d.bar[1]};
            uint64_t candidate=0;
            if(HdaProbePciCheck(p,candidate)==HdaProbeResult::Ready){found=e;base=candidate;present=true;break;}
        }
        Reply r;r.text("{\"dell_hda_memory_api\":1,\"present\":");r.text(present?"true":"false");
        if(present) {
            r.text(",");r.field("base",base);r.text(",");r.field("boot_command",found.command);
            BootResourceClassification range{};const bool rangeValid=AcpiClassifyBootRange(base,4096,range);
            r.text(",\"boot_range\":{\"valid\":");r.text(rangeValid?"true":"false");
            if(rangeValid){r.text(",");r.field("types",range.types);r.text(",\"unreported\":");r.text(range.unreported?"true":"false");}r.text("}");
            KernelAliasMapping identity{},direct{};const bool aliasValid=VmmInspectKernelAliases(base,identity,direct);
            r.text(",\"aliases\":{\"valid\":");r.text(aliasValid?"true":"false");
            auto emitAlias=[&](const char* name,const KernelAliasMapping& m){r.text(",\"");r.text(name);r.text("\":{\"present\":");r.text(m.present?"true":"false");
                r.text(",\"matches\":");r.text((m.present && m.physical==base)?"true":"false");r.text(",");r.field("page_shift",m.pageShift);r.text(",");r.field("pat_index",m.patIndex);r.text("}");};
            if(aliasValid){emitAlias("identity",identity);emitAlias("direct",direct);}r.text("}");
            CpuMemoryTypeObservation memory{};const bool memoryValid=CpuObserveMemoryTypes(base,base,memory);
            r.text(",\"current_cpu_memory_type\":{\"valid\":");r.text(memoryValid?"true":"false");
            if(memoryValid){r.text(",");r.field("cpu",memory.cpu);r.text(",");r.field("pat_slot0",memory.patSlot0);
                r.text(",\"cr0_cd\":");r.text(memory.cacheDisabled?"true":"false");r.text(",\"cr0_nw\":");r.text(memory.notWriteThrough?"true":"false");
                r.text(",");r.field("mtrr_type",memory.page0.type);r.text(",");r.field("matched_types",memory.page0.matchedTypes);}
            r.text("}");
        }
        r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware dell-audio-firmware")) {
        DellAudioFirmwareSnapshot s{};const char* reason=nullptr;const bool ok=DellAudioProbeRead(s,reason);
        Reply r;r.text("{\"dell_audio_firmware_api\":1,\"ok\":");r.text(ok?"true":"false");
        r.text(",\"reason\":");r.quoted(reason?reason:"unavailable");
        if(ok) {
            r.text(",");r.field("osys",s.osys);r.text(",");r.field("pch_generation",s.pchGeneration);
            r.text(",");r.field("codec_selection",s.codecSelection);r.text(",");r.field("i2c_mode",s.i2cMode);
            r.text(",");r.field("i2c_irq",s.i2cIrq);r.text(",");r.field("dsp_base0",s.dspBase0);
            r.text(",");r.field("dsp_base1",s.dspBase1);r.text(",");r.field("dsp_irq",s.dspIrq);
            r.text(",");r.field("i2c_base0",s.i2cBase0);r.text(",");r.field("i2c_base1",s.i2cBase1);
            r.text(",\"mmio_map_valid\":");r.text(AcpiMmioMapValid()?"true":"false");
            r.text(",\"i2c_base0_mmio_declared\":");r.text((s.i2cBase0 && !(s.i2cBase0&4095) && AcpiMmioContains(s.i2cBase0,4096))?"true":"false");
            r.text(",\"i2c_base1_mmio_declared\":");r.text((s.i2cBase1 && !(s.i2cBase1&4095) && AcpiMmioContains(s.i2cBase1,4096))?"true":"false");
            BootResourceClassification c0{},c1{};
            const bool v0=s.i2cBase0 && !(s.i2cBase0&4095) && AcpiClassifyBootRange(s.i2cBase0,4096,c0);
            const bool v1=s.i2cBase1 && !(s.i2cBase1&4095) && AcpiClassifyBootRange(s.i2cBase1,4096,c1);
            r.text(",\"i2c_base0_boot_map\":{\"valid\":");r.text(v0?"true":"false");
            r.text(",");r.field("types",c0.types);r.text(",\"unreported\":");r.text(c0.unreported?"true":"false");r.text("}");
            r.text(",\"i2c_base1_boot_map\":{\"valid\":");r.text(v1?"true":"false");
            r.text(",");r.field("types",c1.types);r.text(",\"unreported\":");r.text(c1.unreported?"true":"false");r.text("}");
            auto aliases=[&](const char* name,uint64_t physical) {
                KernelAliasMapping identity{},direct{};
                const bool valid=VmmInspectKernelAliases(physical,identity,direct);
                r.text(",\"");r.text(name);r.text("\":{\"valid\":");r.text(valid?"true":"false");
                auto emit=[&](const char* field,const KernelAliasMapping& m) {
                    r.text(",\"");r.text(field);r.text("\":{\"present\":");r.text(m.present?"true":"false");
                    r.text(",\"matches\":");r.text((m.present && m.physical==physical)?"true":"false");
                    r.text(",");r.field("page_shift",m.pageShift);r.text(",");r.field("pat_index",m.patIndex);r.text("}");
                };
                if(valid){emit("identity",identity);emit("direct",direct);}r.text("}");
            };
            aliases("i2c_base0_kernel_aliases",s.i2cBase0);aliases("i2c_base1_kernel_aliases",s.i2cBase1);
            CpuMemoryTypeObservation memory{};const bool known=CpuObserveMemoryTypes(s.i2cBase0,s.i2cBase1,memory);
            r.text(",\"current_cpu_memory_types\":{\"valid\":");r.text(known?"true":"false");
            if(known) {
                r.text(",");r.field("cpu",memory.cpu);r.text(",");r.field("pat_slot0",memory.patSlot0);
                r.text(",\"cr0_cd\":");r.text(memory.cacheDisabled?"true":"false");
                r.text(",\"cr0_nw\":");r.text(memory.notWriteThrough?"true":"false");
                auto emit=[&](const char* field,const MtrrPageObservation& page) {
                    r.text(",\"");r.text(field);r.text("\":{");r.field("mtrr_type",page.type);
                    r.text(",");r.field("matched_types",page.matchedTypes);r.text(",\"used_default\":");r.text(page.usedDefault?"true":"false");r.text("}");
                };
                emit("base0",memory.page0);emit("base1",memory.page1);
            }
            r.text("}");
        }
        r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-gop-plan")) {
        const auto& s=BdwGopObserveRead();Reply r;r.text("{\"gpu_gop_plan_api\":1,\"prepare_requested\":");r.text(s.prepareRequested?"true":"false");
        r.text(",\"prepare_built\":");r.text(s.prepareBuilt?"true":"false");
        r.text(",\"render_prepare_requested\":");r.text(s.renderPrepareRequested?"true":"false");
        r.text(",\"render_prepare_built\":");r.text(s.renderPrepareBuilt?"true":"false");
        r.text(",\"retain_requested\":");r.text(s.retainRequested?"true":"false");
        r.text(",\"retain_built\":");r.text(s.retainBuilt?"true":"false");
        r.text(",\"render_retain_requested\":");r.text(s.renderRetainRequested?"true":"false");
        r.text(",\"render_retain_built\":");r.text(s.renderRetainBuilt?"true":"false");
        r.text(",\"inventory\":");r.text(s.inventory?"true":"false");
        r.text(",\"planned\":");r.text(s.planned?"true":"false");
        if(s.planned){r.text(",");r.field("first_index",s.plan.firstIndex);r.text(",");r.field("last_index",s.plan.lastIndex);
            r.text(",");r.field("first_pte_address",s.plan.firstAddress);r.text(",");r.field("last_pte_address",s.plan.lastAddress);}
        r.text(",\"probe_requested\":");r.text(s.probeRequested?"true":"false");
        r.text(",\"power_d0\":");r.text(s.powerD0?"true":"false");
        r.text(",\"pages_admitted\":");r.text(s.pagesAdmitted?"true":"false");
        r.text(",\"sampled\":");r.text(s.sampled?"true":"false");
        if(s.sampled){r.text(",");r.field("first_pte_raw",s.firstPte);r.text(",");r.field("last_pte_raw",s.lastPte);}
        r.text(",\"ack_requested\":");r.text(s.ackRequested?"true":"false");
        r.text(",\"ack_page_admitted\":");r.text(s.ackPageAdmitted?"true":"false");
        r.text(",\"ack_sampled\":");r.text(s.ackSampled?"true":"false");
        if(s.ackSampled){r.text(",");r.field("ack_raw",s.ackRaw);}
        r.text(",\"wake_requested\":");r.text(s.wakeRequested?"true":"false");
        r.text(",\"other_engines_requested\":");r.text(s.otherEnginesRequested?"true":"false");
        r.text(",\"other_engine_pages_admitted\":");r.text(s.otherEnginePagesAdmitted?"true":"false");
        r.text(",\"other_engines_sampled\":");r.text(s.wake.otherEnginesSampled?"true":"false");
        r.text(",\"other_engines_quiescent\":");r.text(s.wake.otherEnginesQuiescent?"true":"false");
        if(s.otherEnginesRequested){r.text(",\"engines\":[");
            constexpr const char* names[]={"rcs0","vcs0","vecs0"};
            for(unsigned i=0;i<3;++i){if(i)r.text(",");const auto& e=s.wake.other[i];
                r.text("{\"name\":\"");r.text(names[i]);r.text("\",\"sampled\":");r.text(e.sampled?"true":"false");
                r.text(",\"quiescent\":");r.text(e.submissionQuiescent?"true":"false");
                if(e.sampled){r.text(",");r.field("head",e.first.head);r.text(",");r.field("tail",e.first.tail);
                    r.text(",");r.field("mode",e.first.mode);r.text(",");r.field("start",e.first.start);
                    r.text(",");r.field("control",e.first.control);r.text(",");r.field("execlist_lo",e.first.execlistLo);
                    r.text(",");r.field("execlist_hi",e.first.execlistHi);r.text(",");r.field("submission_mode",e.first.submissionMode);
                    r.text(",");r.field("head2",e.second.head);r.text(",");r.field("tail2",e.second.tail);
                    r.text(",");r.field("mode2",e.second.mode);r.text(",");r.field("start2",e.second.start);
                    r.text(",");r.field("control2",e.second.control);r.text(",");r.field("execlist_lo2",e.second.execlistLo);
                    r.text(",");r.field("execlist_hi2",e.second.execlistHi);r.text(",");r.field("submission_mode2",e.second.submissionMode);}
                r.text("}");}r.text("]");}
        r.text(",\"wake_pages_admitted\":");r.text(s.wakePagesAdmitted?"true":"false");
        r.text(",\"wake_clock_ready\":");r.text(s.wakeClockReady?"true":"false");
        r.text(",\"wake_aliases\":");r.text(s.wakeAliases?"true":"false");
        r.text(",\"wake_clock\":");r.text(s.wakeClock?"true":"false");
        r.text(",\"wake_acquired\":");r.text(s.wake.acquired?"true":"false");
        r.text(",\"wake_thread_ready\":");r.text(s.wake.threadReady?"true":"false");
        r.text(",\"wake_sampled\":");r.text(s.wake.sampled?"true":"false");
        r.text(",\"wake_idle\":");r.text(s.wake.idle?"true":"false");
        r.text(",\"wake_submission_quiescent\":");r.text(s.wake.submissionQuiescent?"true":"false");
        r.text(",\"wake_clear_delivered\":");r.text(s.wakeClearDelivered?"true":"false");
        r.text(",\"wake_released\":");r.text(s.wake.released?"true":"false");
        r.text(",\"pat_requested\":");r.text(s.patRequested?"true":"false");
        r.text(",\"pat_page_admitted\":");r.text(s.patPageAdmitted?"true":"false");
        r.text(",\"pat_sampled\":");r.text(s.wake.patSampled?"true":"false");
        if(s.wake.patSampled){r.text(",");r.field("pat_lo",s.wake.patLo);r.text(",");r.field("pat_hi",s.wake.patHi);}
        if(s.wake.sampled){r.text(",");r.field("bcs_head",s.wake.first.head);r.text(",");r.field("bcs_tail",s.wake.first.tail);r.text(",");r.field("bcs_mode",s.wake.first.mode);
            r.text(",");r.field("bcs_head2",s.wake.second.head);r.text(",");r.field("bcs_tail2",s.wake.second.tail);r.text(",");r.field("bcs_mode2",s.wake.second.mode);
            r.text(",");r.field("bcs_start",s.wake.first.start);r.text(",");r.field("bcs_control",s.wake.first.control);
            r.text(",");r.field("bcs_start2",s.wake.second.start);r.text(",");r.field("bcs_control2",s.wake.second.control);
            r.text(",");r.field("bcs_execlist_lo",s.wake.first.execlistLo);r.text(",");r.field("bcs_execlist_hi",s.wake.first.execlistHi);
            r.text(",");r.field("bcs_execlist_lo2",s.wake.second.execlistLo);r.text(",");r.field("bcs_execlist_hi2",s.wake.second.execlistHi);
            r.text(",");r.field("bcs_submission_mode",s.wake.first.submissionMode);r.text(",");r.field("bcs_submission_mode2",s.wake.second.submissionMode);}
        r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-forcewake-live")) {
        BdwForcewakeAdapterResult p{};const bool sampled=BdwObserveForcewakeLive(p);Reply r;
        r.text("{\"gpu_forcewake_live_api\":1,\"sampled\":");r.text(sampled?"true":"false");
        r.text(",\"requested\":");r.text(p.sequence.requested?"true":"false");
        r.text(",\"released\":");r.text(p.sequence.released?"true":"false");
        if(sampled){r.text(",");r.field("pat_lo",p.sequence.patLo);r.text(",");r.field("pat_hi",p.sequence.patHi);}
        r.text(",\"ownership\":false}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-engines-live") || !strcmp(command,"bridge hardware gpu-scope-check")) {
        const bool scope=!strcmp(command,"bridge hardware gpu-scope-check");
        BdwForcewakeAdapterResult p{};const bool sampled=BdwObserveForcewakeLive(p,true,scope);Reply r;
        r.text("{\"gpu_engines_live_api\":1,\"sampled\":");r.text(sampled?"true":"false");
        r.text(",\"requested\":");r.text(p.sequence.requested?"true":"false");
        r.text(",\"released\":");r.text(p.sequence.released?"true":"false");
        if(scope){r.text(",\"scope_attempted\":");r.text(p.sequence.workAttempted?"true":"false");
            r.text(",\"scope_complete\":");r.text(p.sequence.workComplete?"true":"false");}
        r.text(",\"other_engines_quiescent\":");r.text(p.sequence.otherEnginesQuiescent?"true":"false");
        if(sampled){r.text(",");r.field("pat_lo",p.sequence.patLo);r.text(",");r.field("pat_hi",p.sequence.patHi);
            r.text(",\"bcs_idle\":");r.text(p.sequence.submissionQuiescent?"true":"false");r.text(",\"other\":[");
            for(unsigned i=0;i<3;++i){if(i)r.text(",");const auto& e=p.sequence.other[i];r.text("{");
                r.field("head",e.first.head);r.text(",");r.field("tail",e.first.tail);r.text(",");r.field("mode",e.first.mode);
                r.text(",");r.field("control",e.first.control);r.text(",");r.field("submission_mode",e.first.submissionMode);
                r.text(",\"quiescent\":");r.text(e.submissionQuiescent?"true":"false");r.text("}");}r.text("]");}
        r.text(",\"ownership\":false}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-clear-prior-scratch")) {
        BdwForcewakeAdapterResult p{};BdwScratchTakeoverResult s{};const bool ok=BdwClearPriorScratch(p,s);Reply r;
        r.text("{\"gpu_clear_prior_scratch_api\":1,\"completed\":");r.text(ok?"true":"false");
        r.text(",\"forcewake_requested\":");r.text(p.sequence.requested?"true":"false");
        r.text(",\"pte_attempted\":");r.text(s.attempted?"true":"false");
        r.text(",\"pte_complete\":");r.text(s.complete?"true":"false");r.text(",");r.field("stage",s.stage);
        r.text(",\"wake_released\":");r.text(p.sequence.released?"true":"false");r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-bcs-copy-test")) {
        BdwForcewakeAdapterResult p{};BdwBcsRunResult s{};const bool ok=BdwTestBcsCopy(p,s);Reply r;
        r.text("{\"gpu_bcs_copy_api\":1,\"completed\":");r.text(ok?"true":"false");
        r.text(",\"attempted\":");r.text(s.attempted?"true":"false");
        r.text(",\"reset_before\":");r.text(s.resetBefore?"true":"false");
        r.text(",\"pat_established\":");r.text(s.patEstablished?"true":"false");
        r.text(",\"pat_uncertain\":");r.text(s.patUncertain?"true":"false");
        r.text(",\"copy_complete\":");r.text(s.copyComplete?"true":"false");
        r.text(",\"context_complete\":");r.text(s.contextComplete?"true":"false");
        r.text(",");r.field("ggtt_write_attempts",s.ggttWriteAttempts);
        r.text(",");r.field("port_write_attempts",s.portWriteAttempts);
        r.text(",\"engine_sampled\":");r.text(s.engineSampled?"true":"false");
        if(s.engineSampled){r.text(",");r.field("head",s.head);r.text(",");r.field("tail",s.tail);
            r.text(",");r.field("mode",s.mode);r.text(",");r.field("control",s.control);
            r.text(",");r.field("acthd",s.acthd);r.text(",");r.field("ipeir",s.ipeir);
            r.text(",");r.field("ipehr",s.ipehr);r.text(",");r.field("execlist",s.execlist);}
        r.text(",\"reset_after\":");r.text(s.resetAfter?"true":"false");
        r.text(",\"pat_restored\":");r.text(s.patRestored?"true":"false");
        r.text(",\"wake_released\":");r.text(p.sequence.released?"true":"false");r.text("}\n");r.send();return true;
    }
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    if(!strcmp(command,"bridge hardware gpu-rcs-worker-test") || !strcmp(command,"bridge hardware gpu-rcs-context-test")){
        const bool contexts=!strcmp(command,"bridge hardware gpu-rcs-context-test");
        BdwWorkerProbeResult s{};bool pending=false;const bool ok=SubmissionDebugRun(s,pending,contexts);Reply r;
        r.text("{\"gpu_rcs_worker_api\":1,\"completed\":");r.text(ok?"true":"false");
        r.text(",\"worker_pending\":");r.text(pending?"true":"false");
        r.text(",");r.field("submitted",s.submitted);r.text(",");r.field("completed_jobs",s.completed);
        r.text(",\"failed\":");r.text(s.failed?"true":"false");
        r.text(",");r.field("value0",s.values[0]);r.text(",");r.field("value1",s.values[1]);
        if(contexts){r.text(",");r.field("value2",s.values[2]);r.text(",");r.field("context_hash0",s.contextHashes[0]);
            r.text(",");r.field("context_hash1",s.contextHashes[1]);}
        r.text("}\n");r.send();return true;
    }
#endif
    if(!strcmp(command,"bridge hardware gpu-rcs-run-test") || !strcmp(command,"bridge hardware gpu-rcs-rebase-test") ||
       !strcmp(command,"bridge hardware gpu-rcs-repeat-test")) {
        const bool rebased=!strcmp(command,"bridge hardware gpu-rcs-rebase-test");
        const bool repeat=!strcmp(command,"bridge hardware gpu-rcs-repeat-test");
        BdwForcewakeAdapterResult p{};BdwRenderRunResult s{};bool pending=false;
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
        const bool ok=RcsDebugRun(repeat?2:rebased?1:0,p,s,pending);
#else
        const bool ok=repeat?BdwTestRenderRepeat(p,s):rebased?BdwTestRebasedRenderRun(p,s):BdwTestRenderRun(p,s);
#endif
        Reply r;
        r.text("{\"gpu_rcs_run_api\":1,\"completed\":");r.text(ok?"true":"false");
        r.text(",\"worker_pending\":");r.text(pending?"true":"false");
        r.text(",\"rebased\":");r.text(rebased?"true":"false");
        r.text(",\"repeat\":");r.text(repeat?"true":"false");
        r.text(",\"admitted\":");r.text(s.admitted?"true":"false");
        r.text(",");r.field("preflight_stage",s.preflightStage);r.text(",");r.field("dma_status",s.dmaStatus);
        r.text(",\"attempted\":");r.text(s.attempted?"true":"false");
        r.text(",\"gt_ready\":");r.text(s.transaction.gtReady?"true":"false");
        r.text(",\"pat_ready\":");r.text(s.transaction.patReady?"true":"false");
        r.text(",\"breadcrumb\":");r.text(s.breadcrumb?"true":"false");
        r.text(",\"context_complete\":");r.text(s.contextComplete?"true":"false");
        r.text(",\"context_saved\":");r.text(s.transaction.contextSaved?"true":"false");
        r.text(",\"ggtt_clear_requested\":");r.text(s.ggttClearRequested?"true":"false");
        r.text(",\"ggtt_cleared\":");r.text(s.ggttCleared?"true":"false");
        r.text(",\"gem_release_requested\":");r.text(s.gemReleaseRequested?"true":"false");
        r.text(",\"gem_released\":");r.text(s.gemReleased?"true":"false");
        r.text(",\"external_tables_requested\":");r.text(s.externalTablesRequested?"true":"false");
        r.text(",\"gem_queued\":");r.text(s.gemQueued?"true":"false");
        r.text(",\"gem_ioctl_decoded\":");r.text(s.gemIoctlDecoded?"true":"false");
        r.text(",\"gem_ioctl_submitted\":");r.text(s.gemIoctlSubmitted?"true":"false");
        r.text(",\"poll_yield_requested\":");r.text(s.pollYieldRequested?"true":"false");
        r.text(",\"poll_yield_failed\":");r.text(s.pollYieldFailed?"true":"false");
        r.text(",");r.field("poll_yields",s.pollYields);
        r.text(",\"reset_complete\":");r.text(s.transaction.resetComplete?"true":"false");
        r.text(",\"pat_restored\":");r.text(s.transaction.patRestored?"true":"false");
        r.text(",\"uncertain\":");r.text(s.transaction.uncertain?"true":"false");
        r.text(",");r.field("ggtt_write_attempts",s.ggttWriteAttempts);r.text(",");r.field("port_write_attempts",s.portWriteAttempts);
        r.text(",");r.field("context_hash",s.contextHash);r.text(",\"engine_sampled\":");r.text(s.engineSampled?"true":"false");
        if(s.engineSampled){r.text(",");r.field("head",s.head);r.text(",");r.field("tail",s.tail);
            r.text(",");r.field("mode",s.mode);r.text(",");r.field("control",s.control);
            r.text(",");r.field("acthd",s.acthd);r.text(",");r.field("ipeir",s.ipeir);
            r.text(",");r.field("ipehr",s.ipehr);r.text(",");r.field("execlist",s.execlist);}
        r.text(",\"wake_released\":");r.text(p.sequence.released?"true":"false");r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-rcs-gt-init")) {
        BdwForcewakeAdapterResult p{};BdwRenderGtInitResult s{};const bool ok=BdwTestRenderGtInit(p,s);Reply r;
        r.text("{\"gpu_rcs_gt_init_api\":1,\"completed\":");r.text(ok?"true":"false");
        r.text(",");r.field("status",unsigned(s.status));r.text(",");r.field("stage",s.stage);
        r.text(",");r.field("writes",s.writes);r.text(",");r.field("l3_before",s.l3Before);r.text(",");r.field("l3_after",s.l3After);
        r.text(",\"topology_sampled\":");r.text(s.topologySampled?"true":"false");
        if(s.topologySampled){r.text(",");r.field("selector",s.selector);r.text(",");r.field("fuse2",s.fuse2);}
        r.text(",\"dop_restored\":");r.text(s.dopRestored?"true":"false");
        r.text(",\"wake_released\":");r.text(p.sequence.released?"true":"false");r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-rcs-reset-test")) {
        BdwForcewakeAdapterResult p{};BdwRcsResetTestResult s{};const bool ok=BdwTestRcsReset(p,s);Reply r;
        r.text("{\"gpu_rcs_reset_api\":1,\"completed\":");r.text(ok?"true":"false");
        r.text(",\"attempted\":");r.text(s.attempted?"true":"false");
        r.text(",\"reset_complete\":");r.text(s.complete?"true":"false");
        r.text(",\"reset_uncertain\":");r.text(s.uncertain?"true":"false");
        r.text(",\"wake_released\":");r.text(p.sequence.released?"true":"false");r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-pat-transition-test")) {
        BdwForcewakeAdapterResult p{};BdwPatTestResult s{};const bool ok=BdwTestPrivatePat(p,s);Reply r;
        r.text("{\"gpu_pat_transition_api\":1,\"completed\":");r.text(ok?"true":"false");
        r.text(",\"attempted\":");r.text(s.attempted?"true":"false");
        r.text(",\"reset_complete\":");r.text(s.resetComplete?"true":"false");
        r.text(",\"established\":");r.text(s.established?"true":"false");
        r.text(",\"restored\":");r.text(s.restored?"true":"false");
        r.text(",");r.field("before_lo",s.beforeLo);r.text(",");r.field("before_hi",s.beforeHi);
        r.text(",");r.field("active_lo",s.activeLo);r.text(",");r.field("active_hi",s.activeHi);
        r.text(",");r.field("after_lo",s.afterLo);r.text(",");r.field("after_hi",s.afterHi);
        r.text(",\"wake_released\":");r.text(p.sequence.released?"true":"false");r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-bcs-init-test")) {
        BdwForcewakeAdapterResult p{};BdwBcsInitResult s{};const bool ok=BdwTestBcsInitialize(p,s);Reply r;
        r.text("{\"gpu_bcs_init_api\":1,\"completed\":");r.text(ok?"true":"false");
        r.text(",\"forcewake_requested\":");r.text(p.sequence.requested?"true":"false");
        r.text(",\"init_attempted\":");r.text(s.attempted?"true":"false");
        r.text(",\"reset_complete\":");r.text(s.resetComplete?"true":"false");
        r.text(",\"init_complete\":");r.text(s.initComplete?"true":"false");
        r.text(",");r.field("stage",s.stage);
        r.text(",\"wake_released\":");r.text(p.sequence.released?"true":"false");
        r.text(",\"dma_submission\":false}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-scratch-takeover")) {
        BdwForcewakeAdapterResult p{};BdwScratchTakeoverResult s{};const bool ok=BdwTestScratchTakeover(p,s);Reply r;
        r.text("{\"gpu_scratch_takeover_api\":1,\"completed\":");r.text(ok?"true":"false");
        r.text(",\"forcewake_requested\":");r.text(p.sequence.requested?"true":"false");
        r.text(",\"reset_attempted\":");r.text(s.resetAttempted?"true":"false");
        r.text(",\"reset_complete\":");r.text(s.resetComplete?"true":"false");
        r.text(",\"pte_attempted\":");r.text(s.attempted?"true":"false");
        r.text(",\"pte_complete\":");r.text(s.complete?"true":"false");
        r.text(",");r.field("stage",s.stage);
        r.text(",\"wake_released\":");r.text(p.sequence.released?"true":"false");
        r.text(",\"dma_submission\":false}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-tlb-test")) {
        BdwForcewakeAdapterResult p{};const bool ok=BdwTestTlbInvalidate(p);Reply r;
        r.text("{\"gpu_tlb_test_api\":1,\"completed\":");r.text(ok?"true":"false");
        r.text(",\"forcewake_requested\":");r.text(p.sequence.requested?"true":"false");
        r.text(",\"tlb_attempted\":");r.text(p.sequence.tlbAttempted?"true":"false");
        r.text(",\"tlb_complete\":");r.text(p.sequence.tlbComplete?"true":"false");
        r.text(",\"wake_released\":");r.text(p.sequence.released?"true":"false");
        r.text(",\"dma_authorized\":false}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-bcs-reset-test")) {
        BdwForcewakeAdapterResult p{};const bool ok=BdwTestBcsReset(p);Reply r;
        r.text("{\"gpu_bcs_reset_test_api\":1,\"completed\":");r.text(ok?"true":"false");
        r.text(",\"forcewake_requested\":");r.text(p.sequence.requested?"true":"false");
        r.text(",\"reset_attempted\":");r.text(p.sequence.resetAttempted?"true":"false");
        r.text(",\"reset_complete\":");r.text(p.sequence.resetComplete?"true":"false");
        r.text(",\"csb_sampled\":");r.text(p.sequence.resetCsbSampled?"true":"false");
        if(p.sequence.resetCsbSampled){r.text(",");r.field("csb_ptr",p.sequence.resetCsb);}
        r.text(",\"wake_released\":");r.text(p.sequence.released?"true":"false");
        r.text(",\"dma_authorized\":false}\n");r.send();return true;
    }
    constexpr char surveyPrefix[]="bridge hardware gpu-ggtt-survey ";
    if(length>=sizeof(surveyPrefix)-1 && !strncmp(command,surveyPrefix,sizeof(surveyPrefix)-1)) {
        const char* p=command+sizeof(surveyPrefix)-1;uint32_t first=0;bool valid=*p!=0;
        while(*p){if(*p<'0' || *p>'9' || first>(1048576u-uint32_t(*p-'0'))/10u){valid=false;break;}
            first=first*10+uint32_t(*p++-'0');}
        BdwGgttSurvey s{};const bool sampled=valid && BdwObserveGgttSurvey(first,s);Reply r;
        r.text("{\"gpu_ggtt_survey_api\":1,\"sampled\":");r.text(sampled?"true":"false");
        if(sampled){r.text(",");r.field("first",s.first);r.text(",");r.field("zero",s.zero);
            r.text(",");r.field("valid",s.valid);r.text(",");r.field("other",s.other);}
        r.text(",\"ownership\":false}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-display-state")) {
        BdwDisplaySnapshot p{};const bool sampled=BdwObserveDisplayLive(p);Reply r;
        r.text("{\"gpu_display_state_api\":1,\"sampled\":");r.text(sampled?"true":"false");
        if(sampled){r.text(",\"pipes\":[");for(unsigned i=0;i<3;++i){if(i)r.text(",");r.text("[");
            for(unsigned j=0;j<BdwDisplaySnapshot::PrimaryOffset;++j){if(j)r.text(",");r.number(p.regs[i][j]);}r.text("]");}r.text("]");
            r.text(",\"primary_offsets\":[");for(unsigned i=0;i<3;++i){if(i)r.text(",");r.number(p.regs[i][BdwDisplaySnapshot::PrimaryOffset]);}r.text("]");}
        r.text(",\"ownership\":false}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-gop-ggtt")) {
        BdwGopPtes p{};const bool sampled=BdwObserveGopPtesLive(p);Reply r;
        r.text("{\"gpu_gop_ggtt_api\":1,\"sampled\":");r.text(sampled?"true":"false");
        if(sampled){r.text(",");r.field("first",p.first);r.text(",");r.field("last",p.last);}
        r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-gop-backing")) {
        BdwGopBacking backing{};const bool sampled=BdwObserveGopBacking(backing);Reply r;
        r.text("{\"gpu_gop_backing_api\":1,\"sampled\":");r.text(sampled?"true":"false");
        if(sampled){r.text(",");r.field("bytes",backing.bytes);}
        r.text(",\"ownership\":false}\n");r.send();return true;
    }
#if defined(BROOK_SSD_AUTO_RECOVERY) && !defined(BROOK_HOST_TEST)
    if(!strcmp(command,"bridge hardware gpu-scanout-fair on") || !strcmp(command,"bridge hardware gpu-scanout-fair off") ||
       !strcmp(command,"bridge hardware gpu-scanout-fair status")) {
        const bool set=strcmp(command,"bridge hardware gpu-scanout-fair status")!=0;
        const bool wanted=!strcmp(command,"bridge hardware gpu-scanout-fair on");
        const bool ok=!set || BdwSetScanoutFairness(wanted);Reply r;
        uint64_t pending=0,bypass=0,noEligible=0,forced=0;
        BdwScanoutFairnessCounts(pending,bypass,noEligible,forced);
        r.text("{\"gpu_scanout_fair_api\":1,\"ok\":");r.text(ok?"true":"false");
        r.text(",\"enabled\":");r.text(BdwScanoutFairness()?"true":"false");
        r.text(",");r.field("pending_observations",pending);r.text(",");r.field("bypasses",bypass);
        r.text(",");r.field("no_eligible",noEligible);r.text(",");r.field("forced_service",forced);
        r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-bcs-present")) {
        const auto stats=BdwReadScanoutCopyStats();Reply r;
        uint64_t queued=0,presented=0;BdwOffscreenPresentCounts(queued,presented);
        r.text("{\"gpu_bcs_present_api\":1,\"enabled\":");r.text(BdwBcsScanoutEnabled()?"true":"false");
        r.text(",");r.field("requested",stats.requested);
        r.text(",");r.field("submitted",stats.submitted);
        r.text(",");r.field("full",stats.full);
        r.text(",");r.field("partial",stats.partial);
        r.text(",");r.field("stage",stats.lastStage);
        r.text(",");r.field("queued",queued);
        r.text(",");r.field("queued_presented",presented);
        r.text("}\n");r.send();return true;
    }
#endif
    if(!strcmp(command,"bridge hardware gpu-bcs-ggtt")) {
        BdwGgttReservedPtes p{};const bool sampled=BdwObserveReservedGgtt(p);Reply r;
        r.text("{\"gpu_bcs_ggtt_api\":1,\"sampled\":");r.text(sampled?"true":"false");
        if(sampled){r.text(",\"entries\":[");for(unsigned i=0;i<8;++i){if(i)r.text(",");r.number(p.values[i]);}r.text("]");}
        r.text(",\"ownership\":false}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-rcs-ggtt")) {
        BdwRcsGgttPtes p{};const bool sampled=BdwObserveRcsGgtt(p);Reply r;
        r.text("{\"gpu_rcs_ggtt_api\":1,\"sampled\":");r.text(sampled?"true":"false");
        if(sampled){r.text(",\"entries\":[");for(unsigned i=0;i<29;++i){if(i)r.text(",");r.number(p.values[i]);}r.text("]");}
        r.text(",\"ownership\":false,\"atomic_snapshot\":false}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-bcs-dma-check")) {
        const uint32_t status=BdwInspectPrivateBcsDma();Reply r;
        r.text("{\"gpu_bcs_dma_snapshot_api\":1,\"snapshot_ok\":");r.text(!status?"true":"false");
        r.text(",");r.field("status",status);
        r.text(",\"dma_authorized\":false}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-rcs-dma-check")) {
        const uint32_t status=BdwInspectPrivateRenderDma();Reply r;
        r.text("{\"gpu_rcs_dma_snapshot_api\":1,\"snapshot_ok\":");r.text(!status?"true":"false");
        r.text(",");r.field("status",status);
        r.text(",\"dma_authorized\":false}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware gpu-vtd") || !strcmp(command,"bridge hardware gpu-vtd sample")) {
        if(!strcmp(command,"bridge hardware gpu-vtd sample"))DellGpuVtdObserveDeferred();
        const auto& s=DellGpuVtdObserveRead();Reply r;
        r.text("{\"gpu_vtd_api\":1,\"requested\":");r.text(s.requested?"true":"false");
        r.text(",\"deferred\":");r.text(s.deferred?"true":"false");
        r.text(",\"deferred_attempted\":");r.text(s.deferredAttempted?"true":"false");
        r.text(",\"topology\":");r.text(s.topology?"true":"false");
        r.text(",\"inventory\":");r.text(s.inventory?"true":"false");
        r.text(",\"boot_map\":");r.text(s.bootMap?"true":"false");
        r.text(",\"boot_map_classified\":");r.text(s.bootMapClassified?"true":"false");
        if(s.bootMapClassified){r.text(",");r.field("boot_map_types",s.bootMapTypes);
            r.text(",\"boot_map_unreported\":");r.text(s.bootMapUnreported?"true":"false");}
        r.text(",\"audited_dmar_unreported\":");r.text(s.auditedDmarHole?"true":"false");
        r.text(",\"sampled\":");r.text(s.sampled?"true":"false");
        if(s.sampled){r.text(",");r.field("version",s.status.version);r.text(",");r.field("global_status",s.status.globalStatus);
            r.text(",\"translation_enabled\":");r.text((s.status.globalStatus&(1u<<31))?"true":"false");
            r.text(",");r.field("capability",s.status.capability);
            r.text(",\"protected_memory_sampled\":");r.text(s.status.protectedMemorySampled?"true":"false");
            if(s.status.protectedMemorySampled){r.text(",");r.field("protected_memory",s.status.protectedMemory);}}
        r.text("}\n");r.send();return true;
    }
    constexpr char nicPrefix[]="bridge hardware nic";
    if(Prefix(command,nicPrefix) && (!command[sizeof(nicPrefix)-1] || command[sizeof(nicPrefix)-1]==' ')) {
        auto* handler=__atomic_load_n(&g_nicCommand,__ATOMIC_ACQUIRE);
        if(!handler){DebugChannelSend("{\"nic_error\":\"No PCI NIC diagnostic handler is loaded\"}\n");return true;}
        const char* args=command+sizeof(nicPrefix)-1;
        while(*args==' ')++args;
        static char text[3073];
        uint32_t n=handler(args,text,sizeof(text)-1);
        if(n>=sizeof(text))n=sizeof(text)-1;
        text[n]=0;
        Reply r;r.text("{\"nic_api\":1,\"text\":");r.quoted(text);r.text("}\n");r.send();return true;
    }
#if !defined(BROOK_HOST_TEST)
    // RTX display step 2: DP AUX (nouveau auxgm200/padgm200). Only the AUX
    // controller (0x00d930-0x00d95b + ch*0x50) and its pad (0x00d970/0x00d97c
    // + ch*0x50) are written. Subcommands:
    //   nv-aux read <ch> <dpcd-hex> <len 1..16>    native DPCD read
    //   nv-aux edid <ch> <block>                   128-byte EDID block via I2C-over-AUX (0x50)
    //   nv-aux power <ch> d0|d3                     DPCD 0x600 write (sink power state)
#if defined(BROOK_NV_DISPLAY)
    if(NvAuxHandle(command,nullptr,0))return true;
    if(NvDispHandle(command))return true;          // RTX step 3 flips (nv_disp_live.cpp), gated
    constexpr char dpIdlePrefix[]="bridge hardware dp-idle ";
    if(Prefix(command,dpIdlePrefix)){DpIdleCommand(command+sizeof(dpIdlePrefix)-1);return true;}
#endif
    // GPU link ASPM L1 (runtime, volatile; a reset restores firmware state):
    // the first 10de display function (fn0 only; HDA/USB-C/UCSI functions are
    // left as they are) and the root port above it. L1 only, never L0s; only
    // when both ends advertise L1, Common Clock Configuration is already set
    // on both (no retrain), and the endpoint's L1 acceptable latency covers
    // the larger exit latency. Enable: upstream first; disable: downstream
    // first (as Linux). Status reports link and root-port AER state.
    constexpr char aspmPrefix[]="bridge hardware gpu-aspm ";
    if(Prefix(command,aspmPrefix)) {
        const char* arg=command+sizeof(aspmPrefix)-1;
        const bool all=!strcmp(arg,"l1-all");
        const bool on=!strcmp(arg,"l1")||all,off=!strcmp(arg,"off"),status=!strcmp(arg,"status");
        if(!on && !off && !status){DebugChannelSend("{\"aspm_error\":\"Expected l1|l1-all|off|status\"}\n");return true;}
        PciDevice gpu{};bool found=false;
        for(uint32_t i=0;i<PciBootInventoryCount() && !found;++i){PciBootEntry e{};
            if(PciBootInventoryRead(i,&e) && e.device.vendorId==0x10de && e.device.classCode==0x03 && e.device.fn==0){gpu=e.device;found=true;}}
        if(!found){DebugChannelSend("{\"aspm_error\":\"No NVIDIA display function 0\"}\n");return true;}
        auto capOf=[](uint8_t b,uint8_t d,uint8_t f)->uint8_t{
            if(!(PciConfigRead16(b,d,f,6)&0x10))return 0;uint8_t o=PciConfigRead8(b,d,f,0x34)&0xfc;
            for(unsigned g=0;o>=0x40 && g<48;++g){if(PciConfigRead8(b,d,f,o)==0x10)return o;o=PciConfigRead8(b,d,f,uint8_t(o+1))&0xfc;}return 0;};
        uint8_t rb=0,rd=0,rf=0;bool root=false;
        for(uint32_t d=0;d<32 && !root;++d)for(uint32_t f=0;f<8 && !root;++f){
            if((PciConfigRead32(0,uint8_t(d),uint8_t(f),0)&0xffff)==0xffff)continue;
            if((PciConfigRead8(0,uint8_t(d),uint8_t(f),0x0e)&0x7f)==1 && PciConfigRead8(0,uint8_t(d),uint8_t(f),0x19)==gpu.bus){rd=uint8_t(d);rf=uint8_t(f);root=true;}}
        const uint8_t ec=capOf(gpu.bus,gpu.dev,0),rc=root?capOf(rb,rd,rf):0;
        if(!ec || !rc){DebugChannelSend("{\"aspm_error\":\"PCIe capability or root port not found\"}\n");return true;}
        const uint32_t eLinkCap=PciConfigRead32(gpu.bus,gpu.dev,0,uint8_t(ec+0x0c)),rLinkCap=PciConfigRead32(rb,rd,rf,uint8_t(rc+0x0c));
        const uint32_t eDevCap=PciConfigRead32(gpu.bus,gpu.dev,0,uint8_t(ec+0x04));
        uint16_t eCtl=PciConfigRead16(gpu.bus,gpu.dev,0,uint8_t(ec+0x10)),rCtl=PciConfigRead16(rb,rd,rf,uint8_t(rc+0x10));
        auto exitUs=[](uint32_t cap)->uint32_t{const uint32_t v=(cap>>15)&7;return v==7?65:(1u<<v);}; // upper bound
        auto acceptUs=[](uint32_t devcap)->uint32_t{const uint32_t v=(devcap>>9)&7;return v==7?~0u:(1u<<v);};
        const bool l1Both=((eLinkCap>>10)&2)&&((rLinkCap>>10)&2);
        const bool ccc=(eCtl&0x40)&&(rCtl&0x40);
        const uint32_t worst=exitUs(eLinkCap)>exitUs(rLinkCap)?exitUs(eLinkCap):exitUs(rLinkCap);
        const bool latencyOk=acceptUs(eDevCap)>=worst;
        const char* result="status";
        if(on){
            if(!l1Both)result="refused: L1 not supported on both ends";
            else if(!ccc)result="refused: common clock not configured (would need a retrain)";
            else if(!latencyOk)result="refused: L1 exit latency exceeds the endpoint's acceptable latency";
            else{PciConfigWrite16(rb,rd,rf,uint8_t(rc+0x10),uint16_t((rCtl&~3u)|2u));
                 PciConfigWrite16(gpu.bus,gpu.dev,0,uint8_t(ec+0x10),uint16_t((eCtl&~3u)|2u));result="L1 enabled (root port, then GPU fn0)";
                 if(all)for(uint8_t f=2;f<=3;++f){const uint8_t fc=capOf(gpu.bus,gpu.dev,f);if(!fc)continue;
                     const uint16_t c=PciConfigRead16(gpu.bus,gpu.dev,f,uint8_t(fc+0x10));
                     PciConfigWrite16(gpu.bus,gpu.dev,f,uint8_t(fc+0x10),uint16_t((c&~3u)|2u));result="L1 enabled (root port, GPU fn0, fn2, fn3)";}
                 KPrintf("pcie: ASPM L1 enabled on 00:%02x.%u and %02x:%02x.0 (runtime)\n",rd,rf,gpu.bus,gpu.dev);}
        } else if(off){
            for(uint8_t f=2;f<=3;++f){const uint8_t fc=capOf(gpu.bus,gpu.dev,f);if(!fc)continue;
                const uint16_t c=PciConfigRead16(gpu.bus,gpu.dev,f,uint8_t(fc+0x10));
                if((c&3u)==2u)PciConfigWrite16(gpu.bus,gpu.dev,f,uint8_t(fc+0x10),uint16_t(c&~3u));} // firmware left 0
            PciConfigWrite16(gpu.bus,gpu.dev,0,uint8_t(ec+0x10),uint16_t(eCtl&~3u));
            PciConfigWrite16(rb,rd,rf,uint8_t(rc+0x10),uint16_t(rCtl&~3u));result="ASPM disabled (GPU fn2/fn3/fn0, then root port)";
            KPrintf("pcie: ASPM disabled on %02x:%02x.0 and 00:%02x.%u\n",gpu.bus,gpu.dev,rd,rf);
        }
        eCtl=PciConfigRead16(gpu.bus,gpu.dev,0,uint8_t(ec+0x10));rCtl=PciConfigRead16(rb,rd,rf,uint8_t(rc+0x10));
        const uint16_t eSta=PciConfigRead16(gpu.bus,gpu.dev,0,uint8_t(ec+0x12)),rSta=PciConfigRead16(rb,rd,rf,uint8_t(rc+0x12));
        // Error indication: PCIe Device Status (bit 0 correctable, 1 non-fatal, 2 fatal) on both ends.
        Reply r;r.text("{\"aspm_api\":1,\"result\":");r.quoted(result);
        r.text(",");r.field("gpu_link_ctl",eCtl);r.text(",");r.field("gpu_link_sta",eSta);
        r.text(",");r.field("root_link_ctl",rCtl);r.text(",");r.field("root_link_sta",rSta);
        r.text(",");r.field("root_dev_sta",PciConfigRead16(rb,rd,rf,uint8_t(rc+0x0a)));
        r.text(",");r.field("gpu_dev_sta",PciConfigRead16(gpu.bus,gpu.dev,0,uint8_t(ec+0x0a)));
        r.text(",");r.field("gpu_l1_exit_us",exitUs(eLinkCap));r.text(",");r.field("root_l1_exit_us",exitUs(rLinkCap));
        r.text(",");r.field("gpu_l1_accept_us",acceptUs(eDevCap));
        r.text(",\"fn_link_ctl\":[");for(uint8_t f=0;f<=3;++f){const uint8_t fc=capOf(gpu.bus,gpu.dev,f);if(f)r.text(",");
            r.number(fc?PciConfigRead16(gpu.bus,gpu.dev,f,uint8_t(fc+0x10)):0);}
        r.text("]}\n");r.send();return true;
    }
    // NVIDIA display survey: read-only 32-bit reads from BAR0 of the first
    // 10de display function, through a read-only uncached one-page mapping
    // (no VMM_WRITABLE), only inside an allow-list (PMC, fuses, PRAMIN window
    // register, PCI mirror, HDA/SOR caps, PROM, display). Nothing is written.
    constexpr char nvPrefix[]="bridge hardware nv-read ";
    if(Prefix(command,nvPrefix)) {
        const char* p=command+sizeof(nvPrefix)-1;
        auto hex=[&](uint32_t& v)->bool{unsigned n=0;v=0;while(*p==' ')++p;
            while((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')){v=v*16+uint32_t(*p<='9'?*p-'0':*p-'a'+10);++p;if(++n>8)return false;}return n>0;};
        uint32_t offset=0,count=0;
        if(!hex(offset) || !hex(count) || *p || (offset&3) || !count || count>0x100){
            DebugChannelSend("{\"nv_error\":\"Expected <hex dword offset> <hex count 1..100>\"}\n");return true;}
        struct Allowed{uint32_t begin,end;};
        static constexpr Allowed allowed[]={{0x000000,0x001000},{0x001700,0x001704},{0x021c00,0x021d00},
            {0x022500,0x022600},{0x00d000,0x00e000},{0x088000,0x088100},{0x08a100,0x08a200},{0x300000,0x400000},{0x610000,0x700000},
            // rtx-disp probes: VRAM size, BAR1/BAR2 block, PRAMIN window contents (reads only;
            // the 0x1700 window base is never written here).
            {0x100c00,0x100d00},{0xb80f00,0xb81000},{0x700000,0x800000},
            {0x00ea00,0x00f000}}; // VPLL (0xef00-0xef5c, 0xea80)
        const uint32_t end=offset+count*4;bool ok=false;
        for(const auto& a:allowed)if(offset>=a.begin && end<=a.end)ok=true;
        if(!ok){DebugChannelSend("{\"nv_error\":\"Range not in the read-only allow-list\"}\n");return true;}
        PciDevice gpu{};bool found=false;
        for(uint32_t i=0;i<PciBootInventoryCount() && !found;++i){PciBootEntry e{};
            if(PciBootInventoryRead(i,&e) && e.device.vendorId==0x10de && e.device.classCode==0x03){gpu=e.device;found=true;}}
        if(!found){DebugChannelSend("{\"nv_error\":\"No NVIDIA display function\"}\n");return true;}
        const uint16_t cmd=PciConfigRead16(gpu.bus,gpu.dev,gpu.fn,4);
        const uint32_t bar0=PciConfigRead32(gpu.bus,gpu.dev,gpu.fn,0x10);
        const uint64_t base=bar0&~uint64_t(15);
        if((bar0&1) || !base || !(cmd&2)){DebugChannelSend("{\"nv_error\":\"BAR0 unassigned or memory decode off\"}\n");return true;}
        BootResourceClassification c{};
        constexpr uint32_t allowedTypes=(1u<<uint32_t(MemoryType::Reserved))|(1u<<uint32_t(MemoryType::Mmio));
        if(!AcpiClassifyBootRange(base,16u<<20,c) || (c.types&~allowedTypes)){
            DebugChannelSend("{\"nv_error\":\"BAR0 overlaps RAM\"}\n");return true;}
        static VirtualAddress window{};
        if(!window){window=VmmAllocPages(1,VMM_WRITABLE|VMM_NO_EXEC,MemTag::Device,KernelPid);
            if(!window){DebugChannelSend("{\"nv_error\":\"No window\"}\n");return true;}
            const auto leaf=VmmDetachPage(KernelPageTable,window);if(leaf)PmmFreePage(VmmLeafPhysical(leaf));
            __asm__ volatile("invlpg (%0)"::"r"(window.raw()):"memory");}
        Reply r;r.text("{\"nv_api\":1,");r.field("bar0",base);r.text(",");r.field("offset",offset);r.text(",\"values\":[");
        uint32_t mappedPage=~0u;
        for(uint32_t i=0;i<count;++i){
            const uint32_t at=offset+i*4,page=at&~0xfffu;
            if(page!=mappedPage){
                if(mappedPage!=~0u){(void)VmmDetachPage(KernelPageTable,window);__asm__ volatile("invlpg (%0)"::"r"(window.raw()):"memory");}
                if(!VmmMapPage(KernelPageTable,window,PhysicalAddress(base+page),VMM_NO_EXEC|VMM_CACHE_DISABLE|VMM_WRITE_THRU,MemTag::Device,KernelPid)){
                    mappedPage=~0u;break;}
                mappedPage=page;
            }
            const uint32_t v=reinterpret_cast<volatile uint32_t*>(window.raw())[(at&0xfff)/4];
            if(i)r.text(",");r.number(v);
        }
        if(mappedPage!=~0u){(void)VmmDetachPage(KernelPageTable,window);__asm__ volatile("invlpg (%0)"::"r"(window.raw()):"memory");}
        r.text("]}\n");r.send();return true;
    }
    constexpr char systemBenchPrefix[]="bridge hardware usb-system-bench ";
    if(Prefix(command,systemBenchPrefix)) {
        const char* p=command+sizeof(systemBenchPrefix)-1;
        uint32_t kib=0,mib=0;
        const bool random=Prefix(p,"rand ");
        if(random)p+=5;
        else if(Prefix(p,"seq "))p+=4;
        else p="";
        if(!Decimal(p,kib) || *p!=' ' || !Decimal(++p,mib) || *p ||
           kib<4 || kib>4096 || (kib&(kib-1)) || (random && kib!=4) ||
           !mib || mib>256 || (mib*1024)%kib) {
            DebugChannelSend("{\"usb_system_bench_error\":\"Expected seq KiB MiB or rand 4 MiB (MiB 1..256)\"}\n");
            return true;
        }
        const auto usb=UsbSystemRead();
        const uint32_t bytes=kib*1024,pages=bytes/4096;
        const uint64_t extent=usb.sectors*512,total=uint64_t(mib)*1024*1024;
        if(!usb.mounted || usb.uncertain || total>extent) {
            DebugChannelSend("{\"usb_system_bench_error\":\"No healthy mounted USB system extent\"}\n");
            return true;
        }
        auto buffer=VmmAllocPages(pages);
        if(!buffer) {DebugChannelSend("{\"usb_system_bench_error\":\"No buffer\"}\n");return true;}
        auto now=[]()->uint64_t {const auto ns=AudioClockNowNs();return ns?ns:ApicTickCount()*1000000;};
        UsbStorageTiming before{},after{};const bool timed=UsbSystemTiming(&before);
        const auto start=now();
        uint64_t done=0,maxNs=0,state=0x5e120abbdc1e600dull;
        int error=0;
        while(done<total) {
            state^=state<<13;state^=state>>7;state^=state<<17;
            const uint64_t at=random?(state%(extent/bytes))*bytes:done;
            const auto began=now();
            const int n=UsbSystemBenchRead(at,reinterpret_cast<void*>(buffer.raw()),bytes);
            const auto ended=now();
            if(ended>=began && ended-began>maxNs)maxNs=ended-began;
            if(n!=int(bytes)){error=n<0?n:-5;break;}
            done+=n;
        }
        const auto end=now();
        VmmFreePages(buffer,pages);
        Reply r;r.text("{\"usb_system_bench_api\":1,");
        r.field("bytes",done);r.text(",");r.field("requests",done/bytes);r.text(",");
        r.field("duration_ns",end>=start?end-start:0);r.text(",");r.field("max_request_ns",maxNs);
        r.text(",\"errno\":");r.number(uint64_t(-error));
        if(timed && UsbSystemTiming(&after)) {
            r.text(",");r.field("commands",after.commands-before.commands);
            r.text(",");r.field("cbw_ns",after.command.totalNs-before.command.totalNs);
            r.text(",");r.field("data_ns",after.data.totalNs-before.data.totalNs);
            r.text(",");r.field("csw_ns",after.status.totalNs-before.status.totalNs);
        }
        r.text("}\n");r.send();return true;
    }
    // Read-only sequential read benchmark of the boot stick (whole disk from
    // LBA 0, 4 MiB requests): sizes the live-store copy. Nothing is written.
    constexpr char benchPrefix[]="bridge hardware usb-bench ";
    if(Prefix(command,benchPrefix)) {
        const char* p=command+sizeof(benchPrefix)-1;uint32_t mib=0;
        if(!Decimal(p,mib) || *p || !mib || mib>4096){DebugChannelSend("{\"usb_bench_error\":\"Expected MiB 1..4096\"}\n");return true;}
        const int disk=BootStickDiskIndex();
        char name[6];
        BootStickUsbDiskName(disk<0?0:unsigned(disk),name);
        Device* dev=disk>=0?DeviceFind(name):nullptr;
        if(!dev || !dev->ops || !dev->ops->read){DebugChannelSend("{\"usb_bench_error\":\"Boot stick disk not found\"}\n");return true;}
        constexpr uint64_t chunk=4ull<<20;
        auto buffer=VmmAllocPages(chunk/4096);
        if(!buffer){DebugChannelSend("{\"usb_bench_error\":\"No buffer\"}\n");return true;}
        const uint64_t total=uint64_t(mib)<<20,start=ApicTickCount();uint64_t done=0;int error=0;
        while(done<total){const int got=dev->ops->read(dev,done,reinterpret_cast<void*>(buffer.raw()),chunk);if(got!=int(chunk)){error=got;break;}done+=chunk;}
        const uint64_t ms=ApicTickCount()-start;
        VmmFreePages(buffer,chunk/4096);
        Reply r;r.text("{\"usb_bench_api\":1,\"device\":");r.quoted(name);r.text(",");r.field("bytes",done);
        r.text(",");r.field("ms",ms);r.text(",");r.field("kb_per_s",ms?done/ms:0);r.text(",\"error\":");
        if(error<0){r.text("-");r.number(uint64_t(-error));}else r.number(uint64_t(error));r.text("}\n");r.send();return true;
    }
    // End-to-end mixer proof on a USB audio sink: an explicit feature-unit
    // level (same gating as "audio usb override"), mixer output unmuted at
    // 100% for the duration, one 1 s 440 Hz -12 dBFS tone submitted as an
    // ordinary mixer stream, then the previous control and minimum restored.
    constexpr char mixerTonePrefix[]="bridge hardware audio mixer-tone ";
    if(Prefix(command,mixerTonePrefix)) {
        auto* handler=__atomic_load_n(&g_usbCommand,__ATOMIC_ACQUIRE);
        if(!handler){DebugChannelSend("{\"audio_error\":\"No USB audio handler is loaded\"}\n");return true;}
        static char args[160],text[3073];
        auto call=[&](const char* a,const char* b)->uint32_t{
            uint32_t n=0;for(const char* q=a;*q && n+1<sizeof(args);)args[n++]=*q++;
            for(const char* q=b;q && *q && n+1<sizeof(args);)args[n++]=*q++;args[n]=0;
            uint32_t len=handler(args,text,sizeof(text)-1);if(len>=sizeof(text))len=sizeof(text)-1;text[len]=0;return len;};
        call("audio usb override ",command+sizeof(mixerTonePrefix)-1);
        bool overrideOn=false;for(uint32_t i=0;text[i];++i)if(!strncmp(text+i,"override on",11)){overrideOn=true;break;}
        if(!overrideOn){Reply r;r.text("{\"audio_api\":1,\"text\":");r.quoted(text);r.text("}\n");r.send();return true;}
        const uint32_t snapshot=AudioOutputControlSnapshot();
        const uint32_t previous=snapshot&~AudioUserUnmuteBit;
        const bool userUnmuted=(snapshot&AudioUserUnmuteBit)!=0;
        (void)AudioSetOutputControl(BROOK_AUDIO_DEFAULT);
        const uint64_t start=ApicTickCount();
        const AudioStreamId stream=AudioMixerOpen();
        uint32_t submitted=0;
        if(stream) {
            Uac2Tone tone;tone.Start(440,44100,44100,441,8192);
            static int16_t chunk[441*2];
            uint32_t pending=0,offset=0;
            while((tone.Active() || pending) && ApicTickCount()-start<3000) {
                if(!pending){pending=0;while(pending<441 && tone.Active()){const int16_t v=tone.NextSample();chunk[pending*2]=v;chunk[pending*2+1]=v;++pending;}offset=0;}
                const uint32_t accepted=AudioMixerSubmit(chunk+offset*2,pending,stream);
                submitted+=accepted;offset+=accepted;pending-=accepted;
                if(pending){SchedulerSleepMs(5);}
            }
        }
        const uint64_t submittedTick=ApicTickCount();
        SchedulerSleepMs(60); // last mixer batch reaches the sink
        call("audio usb drain","");
        if(stream)(void)AudioMixerClose(stream);
        // Restore exactly, including the user's explicit unmute authorization.
        if(userUnmuted)(void)AudioSetUserOutputControl(previous,true);
        else (void)AudioSetOutputControl(previous);
        KPrintf("audio: mixer-tone %u frames submitted ticks %u..%u, control restored %x\n",submitted,
                unsigned(start),unsigned(submittedTick),previous);
        call("audio usb override off","");
        call("audio usb status","");
        Reply r;r.text("{\"audio_api\":1,");r.field("stream",stream?1u:0u);r.text(",");r.field("submitted_frames",submitted);
        r.text(",");r.field("start_tick",start);r.text(",");r.field("submitted_tick",submittedTick);
        r.text(",\"text\":");r.quoted(text);r.text("}\n");r.send();return true;
    }
#endif
    constexpr char audioPrefix[]="bridge hardware audio";
    if(Prefix(command,audioPrefix) && (!command[sizeof(audioPrefix)-1] || command[sizeof(audioPrefix)-1]==' ')) {
        auto* handler=__atomic_load_n(&g_usbCommand,__ATOMIC_ACQUIRE);
        if(!handler){DebugChannelSend("{\"audio_error\":\"No USB audio handler is loaded\"}\n");return true;}
        static char args[160];const char* rest=command+sizeof(audioPrefix)-1;
        uint32_t n=0;for(const char* a="audio ";*a;)args[n++]=*a++;
        while(*rest && n+1<sizeof(args))args[n++]=*rest++;
        args[n]=0;
        static char text[3073];
        uint32_t len=handler(args,text,sizeof(text)-1);
        if(len>=sizeof(text))len=sizeof(text)-1;
        text[len]=0;
        Reply r;r.text("{\"audio_api\":1,\"text\":");r.quoted(text);r.text("}\n");r.send();return true;
    }
    constexpr char usbPrefix[]="bridge hardware usb";
    if(Prefix(command,usbPrefix) && (!command[sizeof(usbPrefix)-1] || command[sizeof(usbPrefix)-1]==' ')) {
        auto* handler=__atomic_load_n(&g_usbCommand,__ATOMIC_ACQUIRE);
        if(!handler){DebugChannelSend("{\"usb_error\":\"No USB diagnostic handler is loaded\"}\n");return true;}
        const char* args=command+sizeof(usbPrefix)-1;
        while(*args==' ')++args;
        static char text[3073];
        uint32_t n=handler(args,text,sizeof(text)-1);
        if(n>=sizeof(text))n=sizeof(text)-1;
        text[n]=0;
        Reply r;r.text("{\"usb_api\":1,\"text\":");r.quoted(text);r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware inventory-info")) {
        Reply r;r.text("{\"hw_inventory_api\":1,\"captured\":");r.text(HwInventoryCaptured()?"true":"false");
        r.text(",");r.field("bytes",HwInventoryBytes());r.text(",");r.field("lines",HwInventoryLines());
        r.text(",\"truncated\":");r.text(HwInventoryOverflow()?"true":"false");
        r.text(",\"max_chunk\":4096}\n");r.send();return true;
    }
    constexpr char inventoryPrefix[]="bridge hardware inventory ";
    if(Prefix(command,inventoryPrefix)) {
        const char* p=command+sizeof(inventoryPrefix)-1;uint32_t offset=0,count=0;
        if(!Decimal(p,offset) || *p++!=' ' || !Decimal(p,count) || *p || !count || count>4096) {
            DebugChannelSend("{\"hw_inventory_error\":\"Expected byte offset and count 1..4096\"}\n");return true;
        }
        if(!HwInventoryCaptured()) {
            DebugChannelSend("{\"hw_inventory_error\":\"Inventory not captured (no /etc/hw-inventory marker)\"}\n");return true;
        }
        static char chunk[4097];
        const uint32_t total=HwInventoryBytes();
        if(offset>total){DebugChannelSend("{\"hw_inventory_error\":\"Offset beyond inventory\"}\n");return true;}
        const uint32_t n=HwInventoryRead(offset,chunk,count+1);
        Reply r;r.text("{\"hw_inventory_api\":1,");r.field("offset",offset);r.text(",");r.field("bytes",n);
        r.text(",");r.field("total",total);r.text(",");r.field("next_offset",offset+n);
        r.text(",\"more\":");r.text(offset+n<total?"true":"false");r.text(",\"text\":");r.quoted(chunk);r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware acpi-info")) {
        Reply r;r.text("{\"acpi_api\":1,");r.field("tables",AcpiInventoryCount());r.text("}\n");r.send();return true;
    }
    constexpr char acpiPrefix[]="bridge hardware acpi ";
    if(Prefix(command,acpiPrefix)) {
        const char* p=command+sizeof(acpiPrefix)-1;uint32_t index=0,offset=0,count=0;uint8_t data[512];AcpiSdtHeader header{};
        if(!Decimal(p,index) || *p++!=' ' || !Decimal(p,offset) || *p++!=' ' || !Decimal(p,count) || *p || count>sizeof(data)) {
            DebugChannelSend("{\"acpi_error\":\"Expected index, offset and byte count 0..512\"}\n");return true;
        }
        if(!AcpiInventoryRead(index,offset,data,count,header)) {
            DebugChannelSend("{\"acpi_error\":\"Table unavailable or invalid range\"}\n");return true;
        }
        Reply r;r.text("{\"acpi_api\":1,");r.field("tables",AcpiInventoryCount());r.text(",");r.field("index",index);
        r.text(",");r.field("offset",offset);r.text(",");r.field("length",header.length);r.text(",\"hex\":\"");
        const char* hex="0123456789abcdef";for(uint32_t i=0;i<count;++i){r.append(hex[data[i]>>4]);r.append(hex[data[i]&15]);}
        r.text("\"}\n");r.send();return true;
    }
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    if(!strcmp(command,"bridge hardware cache-qemu-test")) {
        uint64_t before=0,after=0;__asm__ volatile("pushfq; popq %0":"=r"(before)::"memory");
        CpuMemoryTypeObservation o{};const bool ok=CpuObserveMemoryTypes(0x80000000,0x80001000,o);
        __asm__ volatile("pushfq; popq %0":"=r"(after)::"memory");
        Reply r;r.text("{\"cache_qemu_test\":1,\"valid\":");r.text(ok?"true":"false");
        r.text(",\"if_preserved\":");r.text(((before^after)&(1ull<<9))?"false":"true");
        if(ok){r.text(",");r.field("cpu",o.cpu);r.text(",");r.field("pat_slot0",o.patSlot0);
            r.text(",");r.field("mtrr0",o.page0.type);r.text(",");r.field("mtrr1",o.page1.type);}
        r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware ahci-qemu-install begin")) {
        DebugChannelSend(SsdQemuInstallBegin()?"{\"install_test\":\"started\"}\n":"{\"install_test\":\"refused\"}\n");return true;
    }
    if(!strcmp(command,"bridge hardware ahci-qemu-install status")) {
        const auto s=SsdQemuInstallStatus();Reply r;r.text("{\"install_test\":1,");r.field("state",s.state);
        r.text(",");r.field("stage",s.stage);r.text(",");r.field("operations",s.operations);
        r.text(",\"error\":");if(s.error<0){r.text("-");r.number(-int64_t(s.error));}else r.number(s.error);
        r.text(",\"paused\":");r.text(s.paused?"true":"false");r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware ahci-qemu-install resume")) {
        DebugChannelSend(SsdQemuInstallResume()?"{\"install_test\":\"resumed\"}\n":"{\"install_test\":\"not_paused\"}\n");return true;
    }
    if(!strcmp(command,"bridge hardware ahci-qemu-write-test")) {
        const int result=AhciDisposableQemuTest();
        DebugChannelSend(result==0?"{\"ahci_qemu_write\":\"ok\"}\n":"{\"ahci_qemu_write\":\"failed\"}\n");
        return true;
    }
#endif
#if defined(BROOK_SSD_INSTALLER)
    constexpr const char* installPrefix="bridge hardware ssd-install begin ";
    if(Prefix(command,installPrefix) || !strcmp(command,"bridge hardware ssd-install begin")) {
        const char* args=command+strlen("bridge hardware ssd-install begin");
        bool allowed=false;
        if(*args++==' ' && strlen(args)==65 && args[32]==' ') {
            char boot[33]{};memcpy(boot,args,32);
            auto* key=VfsOpen("/etc/ssd-install-key",VFS_O_STREAM);
            if(key) {
                VnodeStat st{};char expected[32]{};uint64_t offset=0;
                if(!VfsStat(key,&st) && !st.isDir && !st.isSymlink && st.size==32 &&
                   VfsRead(key,expected,32,&offset)==32 && DebugEpochMatches(boot)) {
                    unsigned diff=0;for(unsigned i=0;i<32;++i)diff|=unsigned(expected[i]^args[33+i]);
                    allowed=diff==0;
                }
                VfsRelease(key);
            }
        }
        DebugChannelSend(allowed && SsdQemuInstallBegin()?"{\"ssd_install\":\"started\"}\n":"{\"ssd_install\":\"refused\"}\n");return true;
    }
    if(!strcmp(command,"bridge hardware ssd-install status")) {
        const auto s=SsdQemuInstallStatus();Reply r;r.text("{\"ssd_install\":1,");r.field("state",s.state);
        r.text(",");r.field("stage",s.stage);r.text(",");r.field("operations",s.operations);
        r.text(",\"error\":");if(s.error<0){r.text("-");r.number(-int64_t(s.error));}else r.number(s.error);
        r.text("}\n");r.send();return true;
    }
#endif
    if(!strcmp(command,"bridge hardware ahci-ro")) {
        AhciPortRegisters registers{};
        const auto s=AhciReadOnlyGetStatus(&registers);Reply r;
        r.text("{\"ahci_ro_api\":1,\"requested\":");r.text(s.requested?"true":"false");
        r.text(",\"ready\":");r.text(s.ready?"true":"false");r.text(",\"quarantined\":");r.text(s.quarantined?"true":"false");
        r.text(",\"reason\":");r.quoted(s.reason);r.text(",");r.field("port",s.port);
        r.text(",");r.field("sectors",s.sectors);r.text(",");r.field("logical_sector_bytes",s.logicalSectorBytes);
        r.text(",");r.field("physical_sector_bytes",s.physicalSectorBytes);r.text(",\"model\":");r.quoted(s.model);
        r.text(",\"serial\":");r.quoted(s.serial);
        r.text(",\"port_registers\":");
        if(registers.available) {
            r.text("{");r.field("pxcmd",registers.cmd);r.text(",");r.field("pxsctl",registers.sctl);
            r.text(",");r.field("pxssts",registers.ssts);r.text(",");r.field("pxserr",registers.serr);r.text("}");
        } else r.text("null");
        const auto irq=AhciReadOnlyIrqStatus();const auto w=AhciReadOnlyIoStats();
        r.text(",\"completion\":{\"irq_configured\":");r.text(irq.configured?"true":"false");
        r.text(",\"irq_active\":");r.text(irq.active?"true":"false");r.text(",\"irq_reason\":");r.quoted(irq.reason);
        r.text(",");r.field("vector",irq.vector);r.text(",");r.field("apic",irq.apic);r.text(",");r.field("spin_us",AhciReadOnlySpinUs());
        r.text(",");r.field("waits",w.waits);r.text(",");r.field("spin_completions",w.spinCompletions);
        r.text(",");r.field("sleeping_waits",w.sleepingWaits);r.text(",");r.field("sleeps",w.sleeps);
        r.text(",");r.field("yields",w.yields);r.text(",");r.field("interrupts",w.interrupts);
        r.text(",");r.field("irq_wakes",w.irqWakes);r.text(",");r.field("irq_timeouts",w.irqTimeouts);
        r.text(",");r.field("irq_skipped",w.irqSkipped);r.text(",");r.field("irq_disabled",w.irqDisabled);
        r.text(",");r.field("wait_ns",w.waitNs);r.text(",");r.field("max_wait_ns",w.maxWaitNs);
        r.text(",");r.field("irq_latency_ns",w.irqLatencyNs);r.text(",");r.field("wake_latency_ns",w.wakeLatencyNs);
        r.text(",");r.field("max_wake_latency_ns",w.maxWakeLatencyNs);r.text("}");
        r.text("}\n");r.send();return true;
    }
    constexpr char spinPrefix[]="bridge hardware ahci-spin-us ";
    if(Prefix(command,spinPrefix)) {
        const char* p=command+sizeof(spinPrefix)-1;uint64_t us=0;
        const bool ok=Decimal64(p,us) && !*p && AhciReadOnlySetSpinUs(us);
        Reply r;r.text("{\"ahci_ro_api\":1,\"applied\":");r.text(ok?"true":"false");
        r.text(",");r.field("spin_us",AhciReadOnlySpinUs());r.text("}\n");r.send();return true;
    }
    constexpr char sectorPrefix[]="bridge hardware ahci-sector ";
    if(Prefix(command,sectorPrefix)) {
        const char* p=command+sizeof(sectorPrefix)-1;uint64_t lba=0;uint8_t sector[512];
        if(!Decimal64(p,lba) || *p){DebugChannelSend("{\"ahci_ro_error\":\"expected sector LBA\"}\n");return true;}
        if(AhciReadOnlySector(lba,sector)!=512){DebugChannelSend("{\"ahci_ro_error\":\"sector read failed\"}\n");return true;}
        Reply r;r.text("{\"ahci_ro_api\":1,");r.field("lba",lba);r.text(",\"hex\":\"");
        const char* hex="0123456789abcdef";for(uint32_t i=0;i<512;++i){r.append(hex[sector[i]>>4]);r.append(hex[sector[i]&15]);}
        r.text("\"}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware ahci")) {
        const auto& a=AhciObserveRead();Reply r;
        r.text("{\"ahci_observe_api\":1,\"requested\":");r.text(a.requested?"true":"false");
        r.text(",\"valid\":");r.text(a.valid?"true":"false");r.text(",\"reason\":");r.quoted(a.reason);
        r.text(",");r.field("controller_count",a.controllerCount);r.text(",");r.field("vendor",a.vendor);
        r.text(",");r.field("device",a.device);r.text(",");r.field("bus",a.bus);
        r.text(",");r.field("slot",a.slot);r.text(",");r.field("function",a.function);
        r.text(",");r.field("bar",a.bar);r.text(",");r.field("capability",a.capability);
        r.text(",");r.field("implemented",a.implemented);r.text(",");r.field("version",a.version);
        r.text(",\"ports\":[");
        for (uint32_t i=0;i<a.portCount;++i) {
            const auto& p=a.ports[i];if(i)r.text(",");r.text("{");r.field("index",p.index);
            r.text(",");r.field("status",p.status);r.text(",");r.field("signature",p.signature);
            r.text(",");r.field("command",p.command);r.text(",");r.field("task_file",p.taskFile);
            r.text(",");r.field("issue",p.issue);r.text(",");r.field("active",p.active);
            r.text(",");r.field("interrupt_enable",p.interruptEnable);r.text("}");
        }
        r.text("]}\n");r.send();return true;
    }
    constexpr char backlightPrefix[]="bridge hardware backlight ";
    if(Prefix(command,backlightPrefix)){
        const char* p=command+sizeof(backlightPrefix)-1;bool ok=true;
        if(!strcmp(p,"status")){}
        else if(!strcmp(p,"on"))ok=DisplayPowerSetOn(true);
        else if(Prefix(p,"off ")){p+=4;uint32_t seconds=0;
            ok=Decimal(p,seconds) && !*p && seconds<=60 && DisplayPowerSetOn(false,seconds);}
        else if(Prefix(p,"idle ")){p+=5;uint32_t seconds=0;
            ok=Decimal(p,seconds) && !*p && seconds<=86400 && DisplayPowerSetIdle(seconds);}
        else ok=false;
        const auto status=DisplayPowerRead();Reply r;
        r.text("{\"backlight_api\":1,\"ok\":");r.text(ok?"true":"false");
        r.text(",\"mapped\":");r.text(status.mapped?"true":"false");
        r.text(",\"available\":");r.text(status.available?"true":"false");
        r.text(",\"on\":");r.text(status.on?"true":"false");
        r.text(",\"state_uncertain\":");r.text(status.uncertain?"true":"false");
        r.text(",");r.field("idle_seconds",status.idleSeconds);r.text(",");r.field("last_activity_ms",status.lastActivityMs);
        r.text(",");r.field("restore_at_ms",status.restoreAtMs);r.text(",\"reason\":");r.quoted(status.reason);
        auto registers=[&](const BacklightRegisters& v){r.text("{");r.field("cpu_control",v.cpuControl);
            r.text(",");r.field("cpu_duty",v.cpuDuty);r.text(",");r.field("pch_control",v.pchControl);
            r.text(",");r.field("pch_duty",v.pchDuty);r.text("}");};
        r.text(",\"boot_registers\":");registers(status.original);r.text(",\"current_registers\":");registers(status.current);
        // Active idle inhibitors (Wayland zwp_idle_inhibit_manager_v1 via
        // waylandd). "inhibited" is the policy's last once-a-second poll.
        IdleInhibitSnapshot inhibit{};WmIdleInhibitRead(&inhibit);
        r.text(",\"idle_inhibit\":{\"inhibited\":");r.text(status.inhibited?"true":"false");
        r.text(",");r.field("checked_ms",status.inhibitCheckedMs);
        r.text(",");r.field("windows",inhibit.activeWindows);r.text(",");r.field("inhibitors",inhibit.activeInhibitors);
        r.text(",");r.field("hidden_windows",inhibit.hiddenWindows);r.text(",");r.field("hidden_inhibitors",inhibit.hiddenInhibitors);
        r.text(",\"clients\":[");
        for(uint32_t i=0;i<inhibit.reported;++i){const auto& c=inhibit.clients[i];if(i)r.text(",");
            r.text("{");r.field("wm",c.wmId);r.text(",");r.field("pid",c.pid);r.text(",");r.field("count",c.count);
            r.text(",\"visible\":");r.text(c.visible?"true":"false");
            r.text(",\"app_id\":");r.quoted(c.appId);r.text(",\"title\":");r.quoted(c.title);r.text("}");}
        r.text("]}");
        r.text(",\"scope\":\"backlight PWM only; panel link and system remain running\"}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware pch-watchdog-status")) {
        const uint32_t count=PciBootInventoryCount();PciBootEntry match{};unsigned found=0;
        if(PciBootInventorySeen()!=count){DebugChannelSend("{\"pch_watchdog_error\":\"incomplete boot inventory\"}\n");return true;}
        for(uint32_t i=0;i<count;++i) {
            PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry)){DebugChannelSend("{\"pch_watchdog_error\":\"inventory unavailable\"}\n");return true;}
            if(entry.device.bus==0 && entry.device.dev==31 && entry.device.fn==0 &&
               entry.device.vendorId==0x8086 && entry.device.deviceId==0x9cc3 && entry.pchWatchdog.captured) {match=entry;++found;}
        }
        const auto& pm=AcpiGetPmTimerInfo();PchWatchdogStatus status{};
        if(found!=1 || !PchWatchdogReadStatus(match.pchWatchdog,pm.available,pm.port,
                [](uint8_t offset){return PciConfigRead32(0,31,0,offset);},
                [](uint16_t port){
#if defined(BROOK_HOST_TEST)
                    return PchWatchdogTestRead16(port);
#else
                    return inw(port);
#endif
                },[](uint16_t port){
#if defined(BROOK_HOST_TEST)
                    return PchWatchdogTestRead32(port);
#else
                    return inl(port);
#endif
                },status)) {
            DebugChannelSend("{\"pch_watchdog_error\":\"status admission failed\"}\n");return true;
        }
        Reply r;r.text("{\"pch_watchdog_status_api\":1,");r.field("pmbase",status.base);
        r.text(",");r.field("control",status.control);r.text(",");r.field("status2",status.status2);
        r.text(",");r.field("timer",status.timer);r.text(",");r.field("smi_enable",status.smiEnable);r.text("}\n");r.send();return true;
    }
    if(!strcmp(command,"bridge hardware pch-watchdog-pci")) {
        const uint32_t count=PciBootInventoryCount();
        PciBootEntry match{};unsigned found=0;
        if(PciBootInventorySeen()!=count){DebugChannelSend("{\"pch_watchdog_error\":\"incomplete boot inventory\"}\n");return true;}
        for(uint32_t i=0;i<count;++i) {
            PciBootEntry entry{};if(!PciBootInventoryRead(i,&entry)){DebugChannelSend("{\"pch_watchdog_error\":\"inventory unavailable\"}\n");return true;}
            if(entry.device.bus==0 && entry.device.dev==31 && entry.device.fn==0 &&
               entry.device.vendorId==0x8086 && entry.device.deviceId==0x9cc3 && entry.pchWatchdog.captured) {match=entry;++found;}
        }
        PchWatchdogInventory live{};
        if(found!=1 || !PchWatchdogReadStable(match.pchWatchdog,
                [](uint8_t offset){return PciConfigRead32(0,31,0,offset);},live)) {
            DebugChannelSend("{\"pch_watchdog_error\":\"identity or stable configuration unavailable\"}\n");return true;
        }
        Reply r;r.text("{\"pch_watchdog_pci_api\":1,");r.field("boot_raw_40",match.pchWatchdog.pci40);
        r.text(",");r.field("boot_raw_44",match.pchWatchdog.pci44);
        r.text(",");r.field("live_raw_40",live.pci40);r.text(",");r.field("live_raw_44",live.pci44);
        r.text(",\"changed\":");r.text((live.pci40!=match.pchWatchdog.pci40 || live.pci44!=match.pchWatchdog.pci44)?"true":"false");
        uint32_t rcba=0;
        const bool rcbaStable=PchWatchdogReadRcba(match.pchWatchdog,
            [](uint8_t offset){return PciConfigRead32(0,31,0,offset);},rcba);
        r.text(",\"rcba_config_stable\":");r.text(rcbaStable?"true":"false");
        if(rcbaStable){r.text(",");r.field("rcba_config",rcba);}
        r.text("}\n");r.send();return true;
    }
    if(Prefix(command,pciPrefix)){
        const char* p=command+sizeof(pciPrefix)-1;uint32_t cursor=0,limit=0;
        const uint32_t count=PciBootInventoryCount();
        if(!Decimal(p,cursor) || *p++!=' ' || !Decimal(p,limit) || *p || !limit || limit>16 || cursor>count){
            DebugChannelSend("{\"pci_error\":\"Expected inventory cursor and count 1..16\"}\n");return true;
        }
        const uint32_t end=cursor+((count-cursor)<limit?(count-cursor):limit);
        Reply r;r.text("{\"pci_inventory_api\":1,\"source\":\"boot_snapshot\",");r.field("total",count);
        r.text(",");r.field("seen",PciBootInventorySeen());
        r.text(",\"truncated\":");r.text(PciBootInventorySeen()>count?"true":"false");
        r.text(",");r.field("next_cursor",end);r.text(",\"devices\":[");
        for(uint32_t i=cursor;i<end;++i){
            PciBootEntry entry{};PciBootInventoryRead(i,&entry);const auto& d=entry.device;
            if(i!=cursor)r.text(",");r.text("{");r.field("bus",d.bus);r.text(",");r.field("device",d.dev);
            r.text(",");r.field("function",d.fn);r.text(",");r.field("vendor_id",d.vendorId);r.text(",");r.field("device_id",d.deviceId);
            r.text(",");r.field("class",d.classCode);r.text(",");r.field("subclass",d.subclass);r.text(",");r.field("prog_if",d.progIf);
            r.text(",");r.field("revision",entry.revision);r.text(",");r.field("header_type",entry.headerType);
            r.text(",");r.field("command",entry.command);r.text(",");r.field("subsystem_vendor",entry.subsystemVendor);
            r.text(",");r.field("subsystem_device",entry.subsystemDevice);r.text(",\"raw_bars\":[");
            for(unsigned b=0;b<6;++b){if(b)r.text(",");r.number(d.bar[b]);}r.text("]");
            if(entry.pchWatchdog.captured){
                r.text(",\"pch_watchdog_pci\":{");r.field("raw_40",entry.pchWatchdog.pci40);
                r.text(",");r.field("raw_44",entry.pchWatchdog.pci44);r.text("}");
            }
            if(entry.bdwGraphics.captured){
                r.text(",\"bdw_firmware_graphics\":{");r.field("graphics_control_raw",entry.bdwGraphics.graphicsControl);
                r.text(",");r.field("stolen_base_raw",entry.bdwGraphics.stolenBase);
                r.text(",");r.field("aperture_control_raw",entry.bdwGraphics.apertureControl);r.text("}");
                const auto range=BdwDecodeStolenRange(entry.bdwGraphics);
                r.text(",\"bdw_stolen_range\":{\"valid\":");r.text(range.valid?"true":"false");
                r.text(",");r.field("base",range.base);r.text(",");r.field("size",range.size);
                r.text(",");r.field("end_exclusive",range.end);
                BootResourceClassification classification{};
                const bool classified=range.valid && range.size<=0xffffffffu &&
                    AcpiClassifyBootRange(range.base,static_cast<uint32_t>(range.size),classification);
                r.text(",\"boot_map_classified\":");r.text(classified?"true":"false");
                if(classified){r.text(",");r.field("boot_map_types",classification.types);
                    r.text(",\"boot_map_unreported\":");r.text(classification.unreported?"true":"false");}
                r.text("}");
            }
            r.text("}");
        }
        r.text("],\"more\":");r.text(end<count?"true":"false");r.text("}\n");r.send();return true;
    }
    constexpr char powerPrefix[]="bridge hardware power ";
    if(Prefix(command,powerPrefix)){
        const char* p=command+sizeof(powerPrefix)-1;char id[33]{},boot[33]{},verb[16]{};
        bool valid=ReadId(p,id) && *p++==' ';
        unsigned bytes=0;
        if(valid){while(*p && *p!=' ' && bytes<sizeof(verb)-1)verb[bytes++]=*p++;}
        valid=valid && bytes && *p++==' ' && ReadId(p,boot) && !*p;
        if(!valid){DebugChannelSend("{\"power_error\":\"Expected request_id verb boot_id\"}\n");return true;}
        if(!DebugEpochMatches(boot)){DebugChannelSend("{\"boot_error\":\"Guest boot changed; attach explicitly again\"}\n");return true;}
        if(strcmp(verb,"shutdown") && strcmp(verb,"restart") && strcmp(verb,"power_status") && strcmp(verb,"cancel") && strcmp(verb,"commit") && strcmp(verb,"force_apps")){
            DebugChannelSend("{\"power_error\":\"Unsupported power operation\"}\n");return true;
        }
        char state[24]{};int error=0;ShutdownCommand(id,verb,state,&error);
        Reply r;r.text("{\"power_api\":1,\"request_id\":");r.quoted(id);r.text(",\"operation\":");r.quoted(verb);
        r.text(",\"state\":");r.quoted(state);r.text(",\"error\":");
        if(error<0){r.text("-");r.number(static_cast<uint64_t>(-int64_t(error)));}else r.number(error);
        ShutdownBlockerReport blockers{};ShutdownRequestBlockers(id,&blockers);
        r.text(",\"status\":");r.quoted(blockers.status);
        r.text(",\"stage\":");r.quoted(blockers.stage);
        r.text(",\"force_apps_available\":");r.text(ShutdownCanForceApps(id)?"true":"false");
        r.text(",");r.field("blocker_count",blockers.count);
        r.text(",");r.field("next_signal_ms",blockers.nextSignalMs);
        r.text(",\"blockers\":[");
        for(uint32_t i=0;i<blockers.listed;++i){
            if(i)r.text(",");
            r.text("{");r.field("pid",blockers.entries[i].pid);r.text(",\"name\":");r.quoted(blockers.entries[i].name);
            r.text(",\"window\":");r.text(blockers.entries[i].window?"true":"false");
            r.text(",\"service\":");r.text(blockers.entries[i].service?"true":"false");r.text("}");
        }
        r.text("]}\n");r.send();return true;
    }
    constexpr const char* logs="bridge hardware logs ";
    if(!Prefix(command,logs))return false;
    const char* p=command+21;uint32_t cursor=0,limit=0;
    if(!Decimal(p,cursor) || *p++!=' ' || !Decimal(p,limit) || *p || !limit || limit>64){
        DebugChannelSend("{\"logs_error\":\"Expected uint32 cursor and line count 1..64\"}\n");return true;
    }
    char lines[64][161]{};uint32_t total=0,dropped=0;
    const auto requested=cursor;const int count=DebugOverlayReadRange(&cursor,&lines[0][0],limit,sizeof(lines[0]),&total,&dropped);
    if(count<0){DebugChannelSend("{\"logs_error\":\"Future or invalid log cursor\"}\n");return true;}
    Reply r;r.text("{\"logs_api\":1,");r.field("requested_cursor",requested);
    r.text(",");r.field("next_cursor",cursor);r.text(",");r.field("total_lines",total);
    r.text(",");r.field("dropped_lines",dropped);r.text(",\"line_columns\":160,\"lines\":[");
    for(int i=0;i<count;++i){if(i)r.text(",");r.quoted(lines[i]);}
    r.text("],\"more\":");r.text(cursor<total?"true":"false");r.text("}\n");r.send();return true;
}
}

extern "C" void DebugNicCommandSetHandler(uint32_t (*handler)(const char* args, char* out, uint32_t capacity))
{
    __atomic_store_n(&brook::g_nicCommand, handler, __ATOMIC_RELEASE);
}
extern "C" void DebugUsbCommandSetHandler(uint32_t (*handler)(const char* args, char* out, uint32_t capacity))
{
    __atomic_store_n(&brook::g_usbCommand, handler, __ATOMIC_RELEASE);
}
