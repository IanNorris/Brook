#include "debug_hardware.h"
#include "debug_epoch.h"
#include "debug_overlay.h"
#include "bridge_reply.h"
#include "build_info.h"
#include "audio.h"
#include "smp.h"
#include "shutdown.h"
#include "net.h"
#include "string.h"
#include "fs/compressed_vfs.h"
#include "vfs.h"
#include "pci.h"
#include "display_power.h"
#include "ahci_observe.h"
#include "ahci_readonly.h"
#include "writable_desktop.h"
namespace brook {
namespace {
using Reply=BridgeReplyBuffer<DebugChannelSend>;
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
void PowerCaps(Reply& r){
    const auto power=ShutdownGetPowerCapabilities();
    r.text("\"graceful_quiescence\":");r.text(power.gracefulQuiescence?"true":"false");
    r.text(",\"reset\":");r.text(power.reset?"true":"false");r.text(",\"power_off\":");r.text(power.powerOff?"true":"false");
    r.text(",\"reset_method\":");r.quoted(power.resetMethod);r.text(",\"power_off_method\":");r.quoted(power.powerOffMethod);
    r.text(",\"reset_reason\":");r.quoted(power.resetReason);r.text(",\"power_off_reason\":");r.quoted(power.powerOffReason);
}

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
        r.text(",");r.field("logical_cpus",SmpGetCpuCount());
        r.text(",\"audio_device\":");r.text(AudioAvailable()?"true":"false");
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
        r.text(",\"power_api\":1,\"power\":{");PowerCaps(r);r.text("}");
        VnodeStat helper{},optin{};
        auto* binary=VfsOpen("/boot/HWBULK.ELF",VFS_O_STREAM);
        auto* marker=VfsOpen("/etc/hardware-bulk",VFS_O_STREAM);
        const bool bulk=binary&&marker&&!VfsStat(binary,&helper)&&!helper.isDir&&helper.size>0&&
            !VfsStat(marker,&optin)&&!optin.isDir&&optin.size>0;
        if(binary)VfsClose(binary);
        if(marker)VfsClose(marker);
        r.text(",\"completed_frame_copy_api\":1,\"bulk_helper_api\":");r.number(bulk?1:0);
        r.text(",\"panic_transport\":false,\"raw_frame_transfer\":");r.text(bulk?"true":"false");r.text("}\n");r.send();return true;
    }
    constexpr char pciPrefix[]="bridge hardware pci ";
#if defined(BROOK_AHCI_WRITE_TEST_HOOKS)
    if(!strcmp(command,"bridge hardware ahci-qemu-write-test")) {
        const int result=AhciDisposableQemuTest();
        DebugChannelSend(result==0?"{\"ahci_qemu_write\":\"ok\"}\n":"{\"ahci_qemu_write\":\"failed\"}\n");
        return true;
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
            for(unsigned b=0;b<6;++b){if(b)r.text(",");r.number(d.bar[b]);}r.text("]}");
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
