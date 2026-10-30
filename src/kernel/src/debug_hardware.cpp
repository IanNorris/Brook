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
        r.text(",\"build_id\":");r.quoted(BuildGitHash());r.text(",\"build_date\":");r.quoted(BuildDate());
        r.text(",");r.field("logical_cpus",SmpGetCpuCount());
        r.text(",\"audio_device\":");r.text(AudioAvailable()?"true":"false");
        const auto snapshot=CompressedVfsAppSnapshotInfo();
        r.text(",\"app_snapshot_api\":1,\"app_snapshot\":{\"configured\":");r.text(snapshot.configured?"true":"false");
        r.text(",\"ready\":");r.text(snapshot.ready?"true":"false");
        r.text(",\"source_closed\":");r.text(snapshot.sourceClosed?"true":"false");
        r.text(",");r.field("bytes",snapshot.bytes);r.text(",");r.field("error",uint32_t(snapshot.error));
        r.text(",\"source\":");r.quoted(snapshot.source);r.text("}");
        r.text(",\"power_api\":1,\"power\":{");PowerCaps(r);r.text("}");
        r.text(",\"panic_transport\":false,\"raw_frame_transfer\":false}\n");r.send();return true;
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
