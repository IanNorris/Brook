#include "debug_hardware.h"
#include "debug_epoch.h"
#include "debug_overlay.h"
#include "bridge_reply.h"
#include "build_info.h"
#include "audio.h"
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
#include "ahci_observe.h"
#include "ahci_readonly.h"
#include "writable_desktop.h"
#include "ssd_install_qemu.h"
#include "acpi.h"
#include "dell_audio_probe.h"
#include "dell_i2c_probe.h"
#include "intel_vtd_observe.h"
#include "intel_bdw_scanout.h"
#include "intel_bdw_bootstrap_gate.h"
#include "hda_observe.h"
#include "native_tsc_observe.h"
#include "kprintf.h"
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
#endif
#if defined(BROOK_SSD_GUARDED_DESKTOP)
#include "ac_shutdown_record.h"
#include "guard_checker_runner.h"
#endif
#include "../../drivers/intel_hda/hda_probe.h"
namespace brook {
#if defined(BROOK_HOST_TEST)
uint16_t PchWatchdogTestRead16(uint16_t port);
uint32_t PchWatchdogTestRead32(uint16_t port);
#endif
// Same newline-terminated initrd marker used by HWSTATUS.ELF. A missing or
// malformed marker reads as false (unknown) rather than a build identity.
bool HardwarePayloadIdRead(char* id,uint32_t capacity) {
    if(!id || capacity<2)return false;
    id[0]=0;
    bool valid=false;
    if(auto* file=VfsOpen("/etc/hardware-payload-id",VFS_O_STREAM)) {
        VnodeStat stat{};
        uint64_t offset=0;
        if(!VfsStat(file,&stat) && !stat.isDir && !stat.isSymlink &&
           stat.size>=2 && stat.size<=capacity &&
           VfsRead(file,id,stat.size,&offset)==static_cast<int>(stat.size) &&
           id[stat.size-1]=='\n') {
            valid=true;
            for(uint64_t i=0;i+1<stat.size;++i) {
                const char c=id[i];
                if((c>='a' && c<='z') || (c>='A' && c<='Z') ||
                   (c>='0' && c<='9') || c=='-' || c=='_' || c=='.')continue;
                valid=false;
                break;
            }
            id[stat.size-1]=0;
        }
        VfsRelease(file);
    }
    if(!valid)id[0]=0;
    return valid;
}
namespace {
using Reply=BridgeReplyBuffer<DebugChannelSend>;

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
        DebugHandleCommand(body,length-46);return true;
    }
    if(!strcmp(command,"bridge hardware caps")){
        char boot[33];DebugEpochCopy(boot);
        Reply r;r.text("{\"hardware_api\":1,\"boot_guard\":true,\"logs_api\":1,\"boot_id\":");r.quoted(boot);
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
        const auto s=AhciReadOnlyGetStatus();Reply r;
        r.text("{\"ahci_ro_api\":1,\"requested\":");r.text(s.requested?"true":"false");
        r.text(",\"ready\":");r.text(s.ready?"true":"false");r.text(",\"quarantined\":");r.text(s.quarantined?"true":"false");
        r.text(",\"reason\":");r.quoted(s.reason);r.text(",");r.field("port",s.port);
        r.text(",");r.field("sectors",s.sectors);r.text(",");r.field("logical_sector_bytes",s.logicalSectorBytes);
        r.text(",");r.field("physical_sector_bytes",s.physicalSectorBytes);r.text(",\"model\":");r.quoted(s.model);
        r.text(",\"serial\":");r.quoted(s.serial);
        r.text("}\n");r.send();return true;
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
        if(strcmp(verb,"shutdown") && strcmp(verb,"restart") && strcmp(verb,"power_status") && strcmp(verb,"cancel") && strcmp(verb,"commit")){
            DebugChannelSend("{\"power_error\":\"Unsupported power operation\"}\n");return true;
        }
        char state[24]{};int error=0;ShutdownCommand(id,verb,state,&error);
        Reply r;r.text("{\"power_api\":1,\"request_id\":");r.quoted(id);r.text(",\"operation\":");r.quoted(verb);
        r.text(",\"state\":");r.quoted(state);r.text(",\"error\":");
        if(error<0){r.text("-");r.number(static_cast<uint64_t>(-int64_t(error)));}else r.number(error);
        r.text(",\"status\":");r.quoted(ShutdownRequestStatus(id));r.text("}\n");r.send();return true;
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
