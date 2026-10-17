#include "debug_ui.h"
#include "debug_epoch.h"
#include "window.h"
#include "scheduler.h"
#include "compositor.h"
#include "net.h"
#include "apic.h"
#include "spinlock.h"
#include "memory/heap.h"
#include "string.h"

namespace brook {
namespace {
// Tombstones remain for the boot: an old ID can never target another window.
constexpr uint32_t MaxRequests=256,MaxQueued=16;
enum class Phase { Empty,Queued,Dispatched,Completed,Rejected };
struct Request {
    char id[33];Phase phase;uint32_t windowId,restoreTarget,closeMethod;
    uint64_t incarnation,queuedTick,appliedTick;WmDebugAction action;
    int error;WmDebugWindow result;
};
Request requests[MaxRequests]{};
SpinLock requestLock{};
bool userTakeover=false;

class Json {
    char* bytes;uint32_t size=0,capacity;bool overflow=false;
public:
    explicit Json(uint32_t maximum=4096):bytes(static_cast<char*>(kmalloc(maximum))),capacity(maximum) {}
    ~Json(){kfree(bytes);}
    void text(const char* value) {
        if(!bytes)return;
        while(*value) {
            if(size+1>=capacity){overflow=true;return;}
            bytes[size++]=*value++;
        }
    }
    void number(uint64_t value) {
        char buf[24],out[24];uint32_t n=0;
        do{buf[n++]=char('0'+value%10);value/=10;}while(value);
        for(uint32_t i=0;i<n;++i)out[i]=buf[n-i-1];out[n]=0;text(out);
    }
    void signedNumber(int64_t value) {if(value<0){text("-");number(uint64_t(-(value+1))+1);}else number(value);}
    void string(const char* value) {
        text("\"");const char* hex="0123456789abcdef";
        while(*value) {
            uint8_t c=static_cast<uint8_t>(*value++);
            if(c=='"')text("\\\"");else if(c=='\\')text("\\\\");
            else if(c<32){char out[7]={'\\','u','0','0',hex[c>>4],hex[c&15],0};text(out);}
            else {char out[2]={static_cast<char>(c),0};text(out);}
        }text("\"");
    }
    void field(const char* name,uint64_t value) {text(",");string(name);text(":");number(value);}
    void boolean(const char* name,bool value) {text(",");string(name);text(value?":true":":false");}
    void finish() {
        if(!bytes) {DebugChannelSend("{\"ui_error\":\"Response allocation failed\"}\n");return;}
        if(overflow){DebugChannelSend("{\"ui_error\":\"Response exceeds bound\"}\n");return;}
        if(size+2>capacity){DebugChannelSend("{\"ui_error\":\"Response exceeds bound\"}\n");return;}
        text("\n");bytes[size]=0;DebugChannelSend(bytes);
    }
};
bool Decimal(const char* text,uint64_t* out) {
    if(!*text)return false;uint64_t value=0;
    for(;*text;++text) {
        if(*text<'0'||*text>'9'||value>(~uint64_t(0)-uint64_t(*text-'0'))/10)return false;
        value=value*10+uint64_t(*text-'0');
    }*out=value;return true;
}
bool Id(const char* text) {
    for(uint32_t i=0;i<32;++i)if(!text[i]||!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f')))return false;
    return !text[32];
}
void Error(const char* error) {Json json;json.text("{\"ui_error\":");json.string(error);json.text("}");json.finish();}
void ProcessReference(Json& json,uint32_t pid,uint64_t incarnation) {
    json.text("{\"pid\":");json.number(pid);json.text(",\"incarnation\":\"");json.number(incarnation);json.text("\"}");
}
void SerializeWindow(Json& json,const WmDebugWindow& window) {
    json.text("{\"window_id\":");json.number(window.id);
    json.text(",\"incarnation\":\"");json.number(window.incarnation);json.text("\",\"title\":");json.string(window.title);
    json.text(",\"app_id\":");json.string(window.appId);
    json.text(",\"provider\":");ProcessReference(json,window.providerPid,window.providerIncarnation);
    json.boolean("provider_kernel",window.providerKernel);
    json.text(",\"client\":");
    if(window.clientOrigin==WmClientOrigin::Unknown)json.text("null");
    else ProcessReference(json,window.clientPid,window.clientIncarnation);
    json.text(",\"client_origin\":");json.string(window.clientOrigin==WmClientOrigin::NativeProcess?"native_process":
        window.clientOrigin==WmClientOrigin::RegisteredUnixPeer?"provider_registered_unix_origin":"unknown");
    const uint32_t tasks=window.clientPid?SchedulerCountTasks(static_cast<uint16_t>(window.clientPid),window.clientIncarnation):0;
    if(window.clientOrigin==WmClientOrigin::Unknown)json.text(",\"client_group_tasks\":null,\"client_origin_live\":null");
    else {json.field("client_group_tasks",tasks);json.boolean("client_origin_live",tasks!=0);}
    json.field("client_checked_tick_ms",ApicTickCount());
    json.field("launch_id",window.launchId);
    json.text(",\"x\":");json.signedNumber(window.x);json.text(",\"y\":");json.signedNumber(window.y);
    json.field("width",window.width);json.field("height",window.height);
    json.field("outer_width",window.outerWidth);json.field("outer_height",window.outerHeight);
    json.field("buffer_width",window.bufferWidth);json.field("buffer_height",window.bufferHeight);
    json.boolean("visible",window.visible);json.boolean("minimized",window.minimized);
    json.boolean("maximized",window.maximized);json.boolean("focused",window.focused);
    json.boolean("focusable",window.focusable);json.boolean("api_window",window.apiWindow);
    json.text(",\"supports\":[");
    if(window.focusable) {
        json.text("\"focus\",\"minimize\",\"maximize\",\"restore\"");
        if(window.clientOrigin!=WmClientOrigin::Unknown&&!window.providerKernel)json.text(",\"close\"");
    }json.text("]}");
}
const char* PhaseName(Phase phase) {
    switch(phase) {
    case Phase::Queued:return "queued";case Phase::Dispatched:return "dispatched";
    case Phase::Completed:return "completed";case Phase::Rejected:return "rejected";
    case Phase::Empty:return "empty";
    }return "unknown";
}
void Status(const Request& request) {
    Json json;json.text("{\"request_id\":");json.string(request.id);
    json.text(",\"state\":");json.string(PhaseName(request.phase));
    json.field("queued_tick_ms",request.queuedTick);json.field("applied_tick_ms",request.appliedTick);
    json.text(",\"error_code\":");json.signedNumber(request.error);
    json.field("close_method",request.closeMethod);
    // The snapshot describes WM state at dispatch, not a client configure ACK.
    json.text(",\"applied_window\":");
    if(request.phase==Phase::Completed)SerializeWindow(json,request.result);else json.text("null");
    json.text("}");json.finish();
}
} // namespace

void DebugUiPoll() {
    for(uint32_t turn=0;turn<4;++turn) {
        int selected=-1;Request request{};
        SpinLockAcquire(&requestLock);
        for(uint32_t i=0;!userTakeover&&i<MaxRequests;++i)if(requests[i].phase==Phase::Queued) {
            selected=static_cast<int>(i);requests[i].phase=Phase::Dispatched;request=requests[i];break;
        }
        SpinLockRelease(&requestLock);
        if(selected<0)return;
        request.error=WmApplyDebugAction(request.windowId,request.incarnation,request.action,
            request.restoreTarget,&request.result,&request.closeMethod);
        request.appliedTick=ApicTickCount();request.phase=request.error?Phase::Rejected:Phase::Completed;
        SpinLockAcquire(&requestLock);requests[selected]=request;SpinLockRelease(&requestLock);
    }
}

bool DebugUiHandleCommand(const char* command) {
    const char* prefix="bridge ui ";uint32_t p=0;
    while(prefix[p]&&prefix[p]==command[p])++p;
    if(prefix[p])return false;
    char line[224];uint32_t n=0;
    while(command[p]&&n<sizeof(line)-1)line[n++]=command[p++];
    if(command[p]){Error("Command too long");return true;}line[n]=0;
    const char* args[7];uint32_t count=0;char* at=line;
    while(*at&&count<7) {
        args[count++]=at;while(*at&&*at!=' ')++at;
        if(*at){*at++=0;if(!*at){Error("Invalid fields");return true;}}
    }
    if(*at||!count){Error("Invalid fields");return true;}
    if(count==1&&!strcmp(args[0],"caps")) {
        Json json;char boot[33];DebugEpochCopy(boot);json.text("{\"boot_id\":");json.string(boot);json.text(",\"ui_api\":1,\"max_requests\":256,\"max_queued\":16,\"dedup_retention\":\"until_reboot\",\"launcher\":false,\"window_actions\":[\"focus\",\"minimize\",\"maximize\",\"restore\",\"close\"]}");json.finish();return true;
    }
    if(count<2||!DebugEpochMatches(args[count-1])){Error("Stale guest boot");return true;}
    --count;
    if(count==2&&!strcmp(args[0],"takeover")&&(!strcmp(args[1],"0")||!strcmp(args[1],"1"))) {
        uint32_t cancelled=0,inflight=0;const bool enabled=args[1][0]=='1';
        SpinLockAcquire(&requestLock);userTakeover=enabled;
        for(auto& request:requests) {
            if(enabled&&request.phase==Phase::Queued) {request.phase=Phase::Rejected;request.error=-125;++cancelled;}
            if(request.phase==Phase::Dispatched)++inflight;
        }SpinLockRelease(&requestLock);
        Json json;json.text("{\"user_takeover\":");json.text(enabled?"true":"false");
        json.field("queued_cancelled",cancelled);json.field("already_dispatched",inflight);json.text("}");json.finish();return true;
    }
    if(count==1&&!strcmp(args[0],"windows")) {
        auto* windows=static_cast<WmDebugWindow*>(kmalloc(sizeof(WmDebugWindow)*WM_MAX_WINDOWS));
        if(!windows){Error("Snapshot allocation failed");return true;}
        uint64_t tick=0;uint32_t total=WmSnapshotDebugWindows(windows,WM_MAX_WINDOWS,&tick);
        Json json(128*1024);json.text("{\"sample_tick_ms\":");json.number(tick);json.text(",\"windows\":[");
        for(uint32_t i=0;i<total;++i){if(i)json.text(",");SerializeWindow(json,windows[i]);}
        json.text("],\"process_liveness\":\"rolling after window snapshot\"}");kfree(windows);json.finish();return true;
    }
    if(count==3&&!strcmp(args[0],"process")) {
        uint64_t pid,incarnation;
        if(!Decimal(args[1],&pid)||!pid||pid>65535||!Decimal(args[2],&incarnation)||!incarnation) {
            Error("Invalid process reference");return true;
        }
        Json json;json.text("{\"live_group_tasks\":");json.number(SchedulerCountTasks(static_cast<uint16_t>(pid),incarnation));
        json.field("sample_tick_ms",ApicTickCount());json.text("}");json.finish();return true;
    }
    if(count==2&&!strcmp(args[0],"status")&&Id(args[1])) {
        bool found=false;Request request{};
        SpinLockAcquire(&requestLock);
        for(const auto& candidate:requests)if(candidate.phase!=Phase::Empty&&!strcmp(candidate.id,args[1])) {request=candidate;found=true;break;}
        SpinLockRelease(&requestLock);
        if(found)Status(request);else Error("Unknown UI request");return true;
    }
    if(count==6&&!strcmp(args[0],"window")&&Id(args[1])) {
        if(!WmIsActive()){Error("Window manager inactive");return true;}
        uint64_t id,incarnation,action,restore;
        if(!Decimal(args[2],&id)||!id||id>WM_MAX_WINDOWS||!Decimal(args[3],&incarnation)||!incarnation||
           !Decimal(args[4],&action)||action>4||!Decimal(args[5],&restore)||restore>2) {Error("Invalid window request");return true;}
        Request result{};int free=-1;uint32_t queued=0;bool found=false,mismatch=false,taken=false;
        SpinLockAcquire(&requestLock);
        taken=userTakeover;
        for(uint32_t i=0;i<MaxRequests;++i) {
            const auto& candidate=requests[i];
            if(candidate.phase==Phase::Empty){if(free<0)free=static_cast<int>(i);continue;}
            if(candidate.phase==Phase::Queued||candidate.phase==Phase::Dispatched)++queued;
            if(!strcmp(candidate.id,args[1])) {
                result=candidate;found=true;
                mismatch=candidate.windowId!=id||candidate.incarnation!=incarnation||
                    static_cast<uint32_t>(candidate.action)!=action||candidate.restoreTarget!=restore;
            }
        }
        if(!found&&!taken&&free>=0&&queued<MaxQueued) {
            memcpy(result.id,args[1],33);result.phase=Phase::Queued;result.windowId=static_cast<uint32_t>(id);
            result.incarnation=incarnation;result.action=static_cast<WmDebugAction>(action);
            result.restoreTarget=static_cast<uint32_t>(restore);result.queuedTick=ApicTickCount();requests[free]=result;
        }
        SpinLockRelease(&requestLock);
        if(mismatch)Error("Request ID argument mismatch");
        else if(!found&&taken)Error("Human takeover is active");
        else if(!found&&(free<0||queued>=MaxQueued))Error("UI request capacity exceeded");
        else {CompositorWake();Status(result);}
        return true;
    }
    Error("Unknown UI command");return true;
}
}
