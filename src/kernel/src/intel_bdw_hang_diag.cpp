// GPU hang diagnostics: per-job breadcrumb ring, last failure capture and
// debug-bridge commands. See intel_bdw_hang_diag.h and
// docs/gpu-hang-diagnostics.md. Nothing here touches the device: the
// capture itself is taken inside the render critical section
// (intel_bdw_critical.cpp) and only published from here.
#include "intel_bdw_hang_diag.h"
#include "intel_bdw_scanout.h"
#include "bridge_reply.h"
#include "kprintf.h"
#include "net.h"
#include "native_tsc_observe.h"
#include "string.h"

namespace brook {
namespace {
using Reply=BridgeReplyBuffer<DebugChannelSend>;
BdwHangCrumbRing<128> g_ring;
BdwHangCapture g_scratch;   // written by the critical section (worker thread)
struct Store {uint32_t seq=0,pad=0;BdwHangCapture c{};};
Store g_store[2];           // 0 last failure, 1 last reference
BdwHangCapture g_readCopy;  // debug-channel copy, guarded by g_readBusy
uint32_t g_readBusy=0,g_referenceArmed=0,g_failures=0,g_captures=0,g_lateCompletions=0;

void Publish(unsigned slot,const BdwHangCapture& c){
    auto& s=g_store[slot];
    const uint32_t seq=__atomic_load_n(&s.seq,__ATOMIC_RELAXED);
    __atomic_store_n(&s.seq,seq+1,__ATOMIC_RELAXED);__atomic_thread_fence(__ATOMIC_RELEASE);
    BdwHangCopyWords(s.c,c);
    __atomic_store_n(&s.seq,seq+2,__ATOMIC_RELEASE);
}
bool ReadStore(unsigned slot,BdwHangCapture& out){
    const auto& s=g_store[slot];
    for(unsigned attempt=0;attempt<8;++attempt){
        const uint32_t a=__atomic_load_n(&s.seq,__ATOMIC_ACQUIRE);
        if(!a)return false;
        if(a&1)continue;
        BdwHangCopyWords(out,s.c);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(__atomic_load_n(&s.seq,__ATOMIC_RELAXED)==a)return true;
    }
    return false;
}
bool Prefix(const char* s,const char* p){while(*p)if(*s++!=*p++)return false;return true;}
uint32_t Us(uint64_t t,uint64_t base,uint64_t khz){
    if(!t || !khz || t<base)return 0;const uint64_t us=(t-base)*1000/khz;return us>0xffffffffu?0xffffffffu:uint32_t(us);
}
bool ParseUnsigned(const char* p,uint32_t& out){
    if(!p || *p<'0' || *p>'9')return false;
    uint32_t v=0;for(;*p>='0' && *p<='9';++p){v=v*10+uint32_t(*p-'0');if(v>100000)return false;}
    if(*p)return false;out=v;return true;
}
void SendCapture(unsigned slot){
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&g_readBusy,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)){
        DebugChannelSend("{\"error\":\"hang capture read busy\"}\n");return;}
    if(!ReadStore(slot,g_readCopy)){
        __atomic_store_n(&g_readBusy,0,__ATOMIC_RELEASE);
        DebugChannelSend(slot?"{\"kind\":\"none\",\"scope\":\"no reference capture yet; arm with gpu hang reference arm\"}\n":
            "{\"kind\":\"none\",\"scope\":\"no failed render job since boot\"}\n");return;}
    Reply r;BdwHangCaptureJson(r,g_readCopy);
    __atomic_store_n(&g_readBusy,0,__ATOMIC_RELEASE);
    r.text("\n");r.send();
}
}

BdwHangCapture* BdwHangScratchCapture(){return &g_scratch;}
bool BdwHangTakeReferenceRequest(){return __atomic_exchange_n(&g_referenceArmed,0u,__ATOMIC_ACQ_REL)!=0;}

void BdwHangNoteRenderJob(const BdwHangJobInfo& job,const BdwRenderRunResult& run,bool ok,bool recovered){
    if(!run.attempted)return; // refused before any device access
    const auto& d=run.diag;
    BdwHangBreadcrumb b{};
    b.pid=job.pid;b.ctx=job.ctx;b.objects=job.objects;b.pages=job.pages;
    b.batch=job.batch;b.batchLen=uint32_t(job.batchLen>0xffffffffu?0xffffffffu:job.batchLen);
    b.nonce=job.nonce;b.seen=d.seen;b.polls=d.polls;
    b.descriptor=d.descriptor;b.prepareTsc=job.prepareTsc;
    b.engine=0;b.outcome=ok?BdwHangOk:recovered?BdwHangRecovered:BdwHangUncertain;
    b.runFailSite=run.runFailSite;b.submitSite=run.submitSite;b.initStage=d.initStage;b.csbSite=run.csbSite;
    b.portWrites=d.portWrites;b.flags=uint8_t((d.captured?1:0)|(d.marker?2:0)|(d.grace?4:0));
    b.ringHead=d.ringHead;b.ringTail=d.ringTail;
    b.execlistBefore=d.before.execlistLo;b.csbPtrBefore=d.before.csbPtr;
    b.execlistAfter=d.after.execlistLo;b.execlistHiAfter=d.after.execlistHi;b.csbPtrAfter=d.after.csbPtr;
    b.fwAck=d.before.fwAck;b.rcState=d.before.rcState;b.coreStatus=d.before.coreStatus;
    const uint64_t base=job.prepareTsc,khz=job.khz;
    b.elspUs=Us(d.elspTsc,base,khz);b.firstPollUs=Us(d.firstPollTsc,base,khz);b.breadcrumbUs=Us(d.breadcrumbTsc,base,khz);
    b.csbUs=Us(d.firstCsbTsc,base,khz);b.doneUs=Us(d.runEndTsc,base,khz);
    b.maxPollGapUs=khz?uint32_t(d.maxPollGapTicks*1000/khz):0;
    const auto& y=d.longest;auto us32=[&](uint64_t t){const uint64_t v=khz?t*1000/khz:0;return v>0xffffffffu?0xffffffffu:uint32_t(v);};
    b.yieldUs=us32(y.totalTicks);b.yieldWaitUs=us32(y.waitTicks);b.yieldFromPid=y.switches?y.fromPid:0;b.yieldFromRanUs=us32(y.fromRanTicks);
    g_ring.Append(b);b.job=uint32_t(g_ring.Count());
    BdwHangCapture* c=d.captured?run.hangCapture:nullptr;
    if(c){
        c->kind=ok?2:1;c->khz=khz;c->diag=d;
        c->pid=job.pid;c->ctx=job.ctx;c->objects=job.objects;c->pages=job.pages;
        c->nonce=job.nonce;c->seen=d.seen;c->polls=d.polls;
        c->outcome=b.outcome;c->runFailSite=run.runFailSite;c->submitSite=run.submitSite;c->csbSite=run.csbSite;
        c->csbEvents=run.csbEvents;c->csbEvent=run.csbEvent;
        c->batch=job.batch;c->batchLen=job.batchLen;c->batchBytes=job.batchBytes;
        for(unsigned i=0;i<8;++i)c->batch8[i]=job.batch8[i];
        const uint64_t count=g_ring.Count();
        const uint64_t first=count>BdwHangCrumbsInCapture?count-BdwHangCrumbsInCapture:0;
        c->crumbCount=0;
        for(uint64_t i=first;i<count;++i)if(g_ring.Read(i,c->crumbs[c->crumbCount]))++c->crumbCount;
        Publish(ok?1:0,*c);__atomic_fetch_add(&g_captures,1,__ATOMIC_RELAXED);
    }
    if(ok && d.grace){
        // gen 440: the poll was starved past the deadline but the job had
        // completed; it retired normally after one more check.
        const uint32_t late=__atomic_add_fetch(&g_lateCompletions,1,__ATOMIC_RELAXED);
        if(late<=16 || !(late&(late-1)))
            KPrintf("gpu-submit: late completion accepted pid=%u job=%u grace=%u dl=%u yield_us=%u wait_us=%u lock_us=%u ticks=%u from=%u%s ran_us=%u n=%u\n",
                job.pid,b.job,unsigned(d.grace),unsigned(d.deadline),b.yieldUs,b.yieldWaitUs,us32(y.lockTicks),y.apicTicks,
                b.yieldFromPid,y.fromKernel?"k":"",b.yieldFromRanUs,late);
    }
    if(ok)return;
    const uint32_t n=__atomic_add_fetch(&g_failures,1,__ATOMIC_RELAXED);
    if(n>16 && (n&(n-1)))return;
    // Compact summary for the persisted log tail (lastlog); 160 columns max.
    if(c){
        auto reg=[&](uint32_t offset){return BdwHangReg32(*c,offset);};
        KPrintf("gpu-hang: job=%u verdict=%s init=%u/%u/%x resets=%u/%u elsp_writes=%u marker=%u started=%x crumb=%x/%x\n",
            b.job,BdwHangVerdict(*c),unsigned(d.initStage),unsigned(d.execInitStage),d.execInitValue,
            unsigned(d.takeoverReset),unsigned(d.stopReset),unsigned(d.portWrites),unsigned(d.marker),
            c->completion[BdwHangStartedOffset/4],c->completion[0],c->nonce);
        KPrintf("gpu-hang:  head=%x tail=%x acthd=%x ipehr=%x bb=%x instdone=%x execlist=%x/%x csbptr=%x fault=%x err=%x\n",
            reg(0x2034),reg(0x2030),reg(0x2074),reg(0x2068),reg(0x2140),reg(0x206c),reg(0x2234),reg(0x2238),
            reg(0x23a0),reg(0x4094),reg(0x40a0));
        KPrintf("gpu-hang:  pre-elsp execlist=%x csbptr=%x ack=%x rc=%x core=%x post=%x/%x us elsp=%u poll=%u gap=%u end=%u dl=%u n=%u\n",
            d.before.execlistLo,d.before.csbPtr,d.before.fwAck,d.before.rcState,d.before.coreStatus,
            d.after.execlistLo,d.after.csbPtr,b.elspUs,b.firstPollUs,b.maxPollGapUs,b.doneUs,unsigned(d.deadline),n);
        KPrintf("gpu-hang:  longest yield us=%u wait=%u lock=%u audit=%u spin=%u ticks=%u switches=%u from=%u%s ran_us=%u at=%u\n",
            b.yieldUs,b.yieldWaitUs,us32(y.lockTicks),us32(y.auditTicks),unsigned(y.spin),y.apicTicks,y.switches,
            b.yieldFromPid,y.fromKernel?"k":"",b.yieldFromRanUs,us32(y.offsetTicks));
    }else KPrintf("gpu-hang: job=%u no capture init=%u/%u/%x elsp_writes=%u n=%u\n",
        b.job,unsigned(d.initStage),unsigned(d.execInitStage),d.execInitValue,unsigned(d.portWrites),n);
}

bool BdwHangHandleCommand(const char* cmd){
    if(!Prefix(cmd,"gpu hang") || (cmd[8] && cmd[8]!=' '))return false;
    const char* rest=cmd+8;
    if(!*rest || !strcmp(rest," status")){
        Reply r;r.text("{");r.field("jobs",g_ring.Count());r.text(",");r.field("failures",__atomic_load_n(&g_failures,__ATOMIC_RELAXED));
        r.text(",");r.field("captures",__atomic_load_n(&g_captures,__ATOMIC_RELAXED));
        r.text(",");r.field("late_completions",__atomic_load_n(&g_lateCompletions,__ATOMIC_RELAXED));
        r.text(",\"marker\":");r.text(BdwHangStartedMarkerEnabled()?"true":"false");
        r.text(",\"reference_armed\":");r.text(__atomic_load_n(&g_referenceArmed,__ATOMIC_RELAXED)?"true":"false");
        r.text(",\"scope\":\"RCS user jobs; gpu hang last = failure capture taken before the engine reset; gpu hang ring [n] = last n per-job breadcrumbs (max 128); gpu hang reference arm|show = capture the next job even if healthy; gpu hang marker on|off = started marker in later rings; docs/gpu-hang-diagnostics.md\"}\n");
        r.send();return true;
    }
    if(!strcmp(rest," last")){SendCapture(0);return true;}
    if(!strcmp(rest," reference arm")){__atomic_store_n(&g_referenceArmed,1u,__ATOMIC_RELEASE);
        DebugChannelSend("{\"reference_armed\":true,\"scope\":\"the next attempted render job is captured after its run, before its reset\"}\n");return true;}
    if(!strcmp(rest," reference") || !strcmp(rest," reference show")){SendCapture(1);return true;}
    if(!strcmp(rest," marker on") || !strcmp(rest," marker off") || !strcmp(rest," marker status")){
        if(!strcmp(rest," marker on"))BdwHangSetStartedMarker(true);
        else if(!strcmp(rest," marker off"))BdwHangSetStartedMarker(false);
        Reply r;r.text("{\"marker\":");r.text(BdwHangStartedMarkerEnabled()?"true":"false");
        r.text(",\"scope\":\"applies to rings built after this command; on = the pre-batch CS stall also writes the nonce to completion+16\"}\n");
        r.send();return true;
    }
    if(!strcmp(rest," ring") || Prefix(rest," ring ")){
        uint32_t want=16;
        if(rest[5] && (!ParseUnsigned(rest+6,want) || !want)){DebugChannelSend("{\"error\":\"usage: gpu hang ring [n]\"}\n");return true;}
        if(want>BdwHangCrumbRing<128>::Capacity)want=BdwHangCrumbRing<128>::Capacity;
        const uint64_t count=g_ring.Count();const uint64_t first=count>want?count-want:0;
        const auto& tsc=NativeTscObserveRead();
        Reply r;r.text("{");r.field("jobs",count);r.text(",");r.field("khz",tsc.capable && tsc.measured && tsc.consistent?tsc.khz:0);
        r.text(",\"crumbs\":[");bool any=false;
        for(uint64_t i=first;i<count;++i){BdwHangBreadcrumb b{};if(!g_ring.Read(i,b))continue;
            if(any)r.text(",");any=true;BdwHangCrumbJson(r,b);}
        r.text("],\"scope\":\"oldest first; us=[elsp,first_poll,breadcrumb,first_csb,run_end,max_poll_gap] after worker prepare start; elsp_before=[EXECLIST_STATUS_LO,CSB_PTR]; elsp_after=[LO,HI,CSB_PTR]; flags 1=captured 2=started marker\"}\n");
        r.send();return true;
    }
    DebugChannelSend("{\"error\":\"usage: gpu hang [status|last|ring [n]|reference arm|reference|marker on|off|status]\"}\n");
    return true;
}
}
