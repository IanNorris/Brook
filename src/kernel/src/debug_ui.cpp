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
    char id[33],appId[33];bool launch,coalesced;uint32_t catalogGeneration;uint64_t launchToken;Phase phase;uint32_t windowId,restoreTarget,closeMethod;
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
void SerializeLaunchRecord(Json& json,const LaunchRecord& launch,bool sampleLiveness) {
    const uint64_t token=launch.token;
    json.text("{\"token\":\"");json.number(token);json.text("\",\"state\":");
    json.string(LaunchHistory::State(launch,ApicTickCount()));
    json.boolean("started",launch.started);json.boolean("dispatch_finished",launch.dispatchFinished);
    json.text(",\"dispatch_succeeded\":");
    if(launch.dispatchFinished)json.text(launch.dispatchSucceeded?"true":"false");else json.text("null");
    json.text(",\"raw_dispatch_result\":");
    if(launch.rawResultKnown)json.signedNumber(launch.rawResult);else json.text("null");
    json.boolean("window_observed",launch.windowObserved);json.boolean("existing_window",launch.existingWindow);
    json.text(",\"window\":");
    if(launch.windowObserved&&launch.windowId&&launch.windowIncarnation) {
        json.text("{\"window_id\":");json.number(launch.windowId);
        json.text(",\"incarnation\":\"");json.number(launch.windowIncarnation);json.text("\"}");
    }else json.text("null");
    json.text(",\"correlation\":");json.string(!launch.windowObserved?"none":launch.viaToken?"launch_tag":"app_id_presence");
    json.field("requested_tick_ms",launch.requested);
    json.text(",\"window_observed_tick_ms\":");if(launch.windowObserved)json.number(launch.windowTick);else json.text("null");
    json.field("window_observation_deadline_tick_ms",launch.requested+LaunchFeedbackTable::TimeoutMs);
    json.text(",\"started_tick_ms\":");if(launch.startedTick)json.number(launch.startedTick);else json.text("null");
    json.text(",\"dispatch_tick_ms\":");if(launch.dispatchTick)json.number(launch.dispatchTick);else json.text("null");
    json.boolean("cancelled",launch.cancelled);json.boolean("bridge_only",launch.bridgeOnly);
    if(sampleLiveness)json.boolean("has_live_launch_tagged_processes",SchedulerLaunchHasLiveProcesses(token));
    else json.text(",\"has_live_launch_tagged_processes\":null");
    json.text("}");
}

void SerializeLaunch(Json& json,uint64_t token) {
    LaunchRecord launch{};
    if(!token||!WmSnapshotLaunch(token,&launch)){json.text("null");return;}
    SerializeLaunchRecord(json,launch,true);
}
void Observe(Json& json,uint32_t mask) {
    json.text("{\"observation_api\":1,\"windows\":");
    if(mask&1) {
        auto* windows=static_cast<WmDebugWindow*>(kmalloc(sizeof(WmDebugWindow)*WM_MAX_WINDOWS));
        if(!windows){json.text("{\"error\":\"Snapshot allocation failed\"}");}
        else {
            const auto begin=ApicTickCount();uint64_t end=0;
            const auto count=WmSnapshotDebugWindows(windows,WM_MAX_WINDOWS,&end);
            json.text("{\"snapshot_started_tick_ms\":");json.number(begin);json.field("snapshot_ended_tick_ms",end);
            json.text(",\"state_revision\":null,\"frame_correspondence\":\"unverified\",\"process_liveness\":\"rolling after WM snapshot\",\"windows\":[");
            for(uint32_t i=0;i<count;++i){if(i)json.text(",");SerializeWindow(json,windows[i]);}
            json.text("]}");kfree(windows);
        }
    }else json.text("null");
    json.text(",\"processes\":");
    if(mask&2) {
        auto* tasks=static_cast<DebugTaskSnapshot*>(kmalloc(sizeof(DebugTaskSnapshot)*MAX_PROCESSES));
        if(!tasks)json.text("{\"error\":\"Snapshot allocation failed\"}");
        else {
            uint32_t total=0;uint64_t begin=0,end=0;
            const auto count=SchedulerSnapshotDebugTasks(tasks,MAX_PROCESSES,&total,&begin,&end);
            json.text("{\"snapshot_started_tick_ms\":");json.number(begin);json.field("snapshot_ended_tick_ms",end);
            json.field("total",total);json.field("capacity",MAX_PROCESSES);json.boolean("truncated",count<total);
            json.text(",\"consistency\":\"Registry membership and incarnations pinned; task state, credentials and CPU counters are rolling\",\"tasks\":[");
            for(uint32_t i=0;i<count;++i) {
                const auto& r=tasks[i];if(i)json.text(",");
                json.text("{\"pid\":");json.number(r.pid);json.text(",\"incarnation\":\"");json.number(r.incarnation);json.text("\"");
                json.field("tgid",r.tgid);json.text(",\"group_incarnation\":\"");json.number(r.groupIncarnation);json.text("\"");
                json.field("parent_tgid",r.parentPid);json.field("uid",r.uid);json.field("euid",r.euid);json.field("gid",r.gid);json.field("egid",r.egid);
                json.text(",\"launch_id\":\"");json.number(r.launchId);json.text("\",\"name\":");json.string(r.name);
                const char* states[]={"ready","running","blocked","stopped","terminated"};
                json.text(",\"state\":");const auto state=static_cast<uint32_t>(r.state);json.string(state<5?states[state]:"unknown");
                json.boolean("kernel",r.kernel);json.text(",\"running_on_cpu\":");json.signedNumber(r.runningOnCpu);
                json.field("user_ticks",r.userTicks);json.field("system_ticks",r.sysTicks);json.text("}");
            }json.text("]}");kfree(tasks);
        }
    }else json.text("null");
    json.text(",\"launches\":");
    if(mask&4) {
        auto* launches=static_cast<LaunchRecord*>(kmalloc(sizeof(LaunchRecord)*LaunchHistory::Capacity));
        if(!launches)json.text("{\"error\":\"Snapshot allocation failed\"}");
        else {
            const auto begin=ApicTickCount();const auto count=WmSnapshotLaunches(launches,LaunchHistory::Capacity);const auto end=ApicTickCount();
            json.text("{\"snapshot_started_tick_ms\":");json.number(begin);json.field("snapshot_ended_tick_ms",end);
            json.field("capacity",LaunchHistory::Capacity);json.text(",\"scope\":\"Bridge-tracked launch tokens retained until reboot; process liveness not sampled here\",\"launches\":[");
            for(uint32_t i=0;i<count;++i){if(i)json.text(",");SerializeLaunchRecord(json,launches[i],false);}
            json.text("]}");kfree(launches);
        }
    }else json.text("null");
    json.text("}");
}

void Status(const Request& request) {
    Json json;json.text("{\"request_id\":");json.string(request.id);
    json.text(",\"state\":");json.string(PhaseName(request.phase));
    json.field("queued_tick_ms",request.queuedTick);json.field("applied_tick_ms",request.appliedTick);
    json.text(",\"error_code\":");json.signedNumber(request.error);
    json.field("close_method",request.closeMethod);
    json.text(",\"kind\":");json.string(request.launch?"launch":"window");
    json.boolean("coalesced",request.coalesced);
    json.text(",\"launch\":");if(request.launch)SerializeLaunch(json,request.launchToken);else json.text("null");
    // The snapshot describes WM state at dispatch, not a client configure ACK.
    json.text(",\"applied_window\":");
    if(request.phase==Phase::Completed&&!request.launch)SerializeWindow(json,request.result);else json.text("null");
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
        request.error=request.launch?
            WmLauncherActivate(request.appId,request.catalogGeneration,&request.launchToken,&request.coalesced):
            WmApplyDebugAction(request.windowId,request.incarnation,request.action,
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
        Json json;char boot[33];DebugEpochCopy(boot);json.text("{\"boot_id\":");json.string(boot);json.text(",\"ui_api\":1,\"max_requests\":256,\"max_queued\":16,\"dedup_retention\":\"until_reboot\",\"launcher\":true,\"launch_api\":1,\"observation_api\":1,\"frame_epoch_guard\":1,\"apps_api\":1,\"window_actions\":[\"focus\",\"minimize\",\"maximize\",\"restore\",\"close\"]}");json.finish();return true;
    }
    if(count<2||!DebugEpochMatches(args[count-1])){Error("Stale guest boot");return true;}
    --count;
    if(count==2&&!strcmp(args[0],"observe")) {
        uint64_t mask=0;
        if(!Decimal(args[1],&mask)||!mask||mask>7){Error("Invalid observation mask");return true;}
        Json json(1024*1024);Observe(json,static_cast<uint32_t>(mask));json.finish();return true;
    }
    if(count==2&&!strcmp(args[0],"takeover")&&(!strcmp(args[1],"0")||!strcmp(args[1],"1"))) {
        uint32_t cancelled=0,inflight=0;const bool enabled=args[1][0]=='1';
        SpinLockAcquire(&requestLock);userTakeover=enabled;
        for(auto& request:requests) {
            if(enabled&&request.phase==Phase::Queued) {request.phase=Phase::Rejected;request.error=-125;++cancelled;}
            if(request.phase==Phase::Dispatched)++inflight;
        }SpinLockRelease(&requestLock);
        uint32_t launcherCancelled=0,launcherStarted=0;
        if(enabled)WmCancelBridgeQueuedLaunches(&launcherCancelled,&launcherStarted);
        Json json;json.text("{\"user_takeover\":");json.text(enabled?"true":"false");
        json.field("queued_cancelled",cancelled);json.field("already_dispatched",inflight);
        json.field("launcher_queued_cancelled",launcherCancelled);json.field("launcher_already_started_or_shared",launcherStarted);json.text("}");json.finish();return true;
    }
    if(count==1&&!strcmp(args[0],"apps")) {
        auto* apps=static_cast<WmDebugApp*>(kmalloc(sizeof(WmDebugApp)*WM_LAUNCHER_MAX_ITEMS));
        if(!apps){Error("Catalog snapshot allocation failed");return true;}
        uint32_t generation=0;uint64_t built=0;
        const uint32_t total=WmSnapshotDebugApps(apps,WM_LAUNCHER_MAX_ITEMS,&generation,&built);
        Json json(128*1024);json.text("{\"catalog_generation\":");json.number(generation);
        json.boolean("catalog_ready",generation!=0);
        json.field("catalog_built_tick_ms",built);json.field("sample_tick_ms",ApicTickCount());
        json.text(",\"availability_scope\":\"Validated during catalog build; worker revalidates before dispatch\",\"apps\":[");
        for(uint32_t i=0;i<total;++i) {
            const auto& app=apps[i];if(i)json.text(",");
            json.text("{\"app_id\":");json.string(app.id);json.text(",\"title\":");json.string(app.title);
            json.text(",\"desktop_app_id\":");json.string(app.desktopAppId);
            json.text(",\"executable\":");json.string(app.executable);
            json.text(",\"source\":");json.string(app.desktop?"installed_desktop_entry":"validated_shortcut");
            json.boolean("launch_eligible",app.eligible);
            json.text(",\"availability\":");json.string(app.desktop?"executable_validated_at_build":"requirements_validated_at_build");
            json.text("}");
        }
        json.text("]}");kfree(apps);json.finish();return true;
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
    const bool launch=count==4&&!strcmp(args[0],"launch");
    if(((count==6&&!strcmp(args[0],"window"))||launch)&&Id(args[1])) {
        if(!WmIsActive()){Error("Window manager inactive");return true;}
        uint64_t id=0,incarnation=0,action=0,restore=0,generation=0;
        if(launch) {
            if(!Id(args[2])||!Decimal(args[3],&generation)||!generation||generation>0xffffffffULL) {
                Error("Invalid app reference");return true;
            }
        }else if(!Decimal(args[2],&id)||!id||id>WM_MAX_WINDOWS||!Decimal(args[3],&incarnation)||!incarnation||
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
                mismatch=candidate.launch!=launch || (launch?
                    candidate.catalogGeneration!=generation||strcmp(candidate.appId,args[2]):
                    candidate.windowId!=id||candidate.incarnation!=incarnation||
                    static_cast<uint32_t>(candidate.action)!=action||candidate.restoreTarget!=restore);
            }
        }
        if(!found&&!taken&&free>=0&&queued<MaxQueued) {
            memcpy(result.id,args[1],33);result.phase=Phase::Queued;result.launch=launch;
            if(launch){memcpy(result.appId,args[2],33);result.catalogGeneration=static_cast<uint32_t>(generation);}
            result.windowId=static_cast<uint32_t>(id);
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
