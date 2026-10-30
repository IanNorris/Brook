#include "debug_hardware.h"
#include "debug_epoch.h"
#include "debug_overlay.h"
#include "bridge_reply.h"
#include "build_info.h"
#include "audio.h"
#include "smp.h"
#include "net.h"
#include "string.h"
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
        r.text(",\"panic_transport\":false,\"raw_frame_transfer\":false}\n");r.send();return true;
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
