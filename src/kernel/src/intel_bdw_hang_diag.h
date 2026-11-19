#pragma once
#include <stdint.h>

// GPU hang diagnostics for the Broadwell RCS user-job path (Dell XPS 13
// 9343, HD 5500). Three parts, see docs/gpu-hang-diagnostics.md:
//  * a per-job breadcrumb ring, always on (BdwHangCrumbRing);
//  * a failure capture taken inside the forcewake critical section, after a
//    failed run and before its engine reset (BdwHangCaptureDevice), reads only;
//  * JSON formatting and a heuristic verdict for the debug bridge.
// Header-only so host tests can drive it with fake MMIO.
namespace brook {

inline uint64_t BdwHangTsc(){
    uint32_t lo,hi;__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)::"memory");return (uint64_t(hi)<<32)|lo;
}

// Runtime gate (default on) for the ring's "started" marker: the CS-stall
// PIPE_CONTROL between the context workaround and MI_BATCH_BUFFER_START also
// posts a qword write of the job nonce to completion+16. The final
// breadcrumb (completion+0) then gives an unambiguous before/after pair.
// "gpu hang marker off" restores the previous plain barrier for later jobs.
constexpr uint32_t BdwHangStartedOffset=16;
inline uint32_t& BdwHangMarkerFlag(){static uint32_t flag=1;return flag;}
inline bool BdwHangStartedMarkerEnabled(){return __atomic_load_n(&BdwHangMarkerFlag(),__ATOMIC_RELAXED)!=0;}
inline void BdwHangSetStartedMarker(bool on){__atomic_store_n(&BdwHangMarkerFlag(),on?1u:0u,__ATOMIC_RELAXED);}

// Engine state sampled just before the first and after the last ELSP dword.
struct BdwSubmitSample {
    uint64_t tsc=0;
    uint32_t execlistLo=0,execlistHi=0,csbPtr=0,rcState=0,coreStatus=0,fwAck=0;
    bool valid=false;
};
struct BdwSubmitSampler {virtual void sampleBefore(BdwSubmitSample&)=0;virtual void sampleAfter(BdwSubmitSample&)=0;};
// The longest poll yield of a run, split by phase (TSC ticks): unlock, the
// sleep or spin slice itself, re-taking the page-table lock, re-audit. The
// APIC tick delta shows whether timer interrupts ran meanwhile; the last
// context switch into this CPU names who held it before the worker resumed.
struct BdwHangYieldDiag {
    uint64_t totalTicks=0,unlockTicks=0,waitTicks=0,lockTicks=0,auditTicks=0,fromRanTicks=0,offsetTicks=0;
    uint32_t apicTicks=0,switches=0,fromPid=0;bool spin=false,fromKernel=false,fromIdle=false,valid=false;
};
struct BdwHangEngineBrief {uint32_t head=0,tail=0,mode=0,control=0,execlistLo=0,execlistHi=0,ringMode=0;};

// Filled inside the critical section for every attempted render job. All
// fields come from values the submission path already reads, plus the two
// ELSP samples (eight MMIO reads) and TSC reads.
struct BdwRenderJobDiag {
    uint64_t originTsc=0,initEndTsc=0,elspTsc=0,firstPollTsc=0,lastPollTsc=0,breadcrumbTsc=0,
        firstCsbTsc=0,runEndTsc=0,maxPollGapTicks=0;
    BdwSubmitSample before{},after{};
    uint64_t descriptor=0;
    uint32_t ringHead=0,ringTail=0; // CTX_RING_HEAD/TAIL in the image at ELSP time
    // initializeExeclists: 0 ok, 1 precondition, 2 not quiescent, 3 takeover
    // reset not acknowledged, 4 execlist init (execInit*), 5 render state.
    uint8_t initStage=0,execInitStage=0;uint32_t execInitValue=0;
    // BdwBcsResetResult+1 for the takeover and stop resets (0 = not run).
    uint8_t takeoverReset=0,stopReset=0;
    uint8_t portWrites=0;bool marker=false,captured=false;
    uint8_t grace=0; // deadline grace used: 1 completion check, 2 retirement check
    uint32_t yields=0;BdwHangYieldDiag longest{};
    uint8_t deadline=0; // why the render critical section stopped waiting: 1 budget, 2 clock, 3 2000 ms
    uint32_t seen=0,polls=0; // last breadcrumb value read and polls made
    uint32_t nonQuiescent=0;BdwHangEngineBrief busy{}; // last non-quiescent observation
};

// Linux i915 gen8 register offsets (drivers/gpu/drm/i915/i915_reg.h,
// v4.4..v5.4, RENDER_RING_BASE 0x2000). Only pages the render critical
// section already audits as UC are listed: 0x0000, 0x2000, 0x4000, 0x9000,
// 0xa000, 0x130000, 0x138000. Not captured (pages not in the audited set):
// GT FIFO (GTFIFODBG 0x120000, GT_FIFO_FREE_ENTRIES 0x120008), slice/
// sampler/row INSTDONE (0x7100, 0xe160, 0xe164), GAB_CTL 0x24000.
struct BdwHangReg {uint32_t offset;const char* name;};
inline constexpr BdwHangReg BdwHangRegs[]={
    {0x2030,"RING_TAIL"},{0x2034,"RING_HEAD"},{0x2038,"RING_START"},{0x203c,"RING_CTL"},
    {0x2074,"RING_ACTHD"},{0x205c,"RING_ACTHD_UDW"},{0x2078,"RING_DMA_FADD"},{0x2060,"RING_DMA_FADD_UDW"},
    {0x2064,"RING_IPEIR"},{0x2068,"RING_IPEHR"},
    {0x206c,"RING_INSTDONE"}, // GEN7_INSTDONE_1 aliases this offset on gen7+
    {0x2070,"RING_INSTPS"},{0x20c0,"RING_INSTPM"},
    {0x20b0,"RING_EIR"},{0x20b4,"RING_EMR"},{0x20b8,"RING_ESR"},{0x2024,"PGTBL_ER"},
    {0x2140,"RING_BBADDR"},{0x2168,"RING_BBADDR_UDW"},{0x2110,"RING_BBSTATE"},
    {0x2114,"RING_SBBADDR"},{0x211c,"RING_SBBADDR_UDW"},{0x2118,"RING_SBBSTATE"},
    {0x2180,"CCID"},{0x2244,"RING_CONTEXT_CONTROL"},
    {0x2234,"EXECLIST_STATUS_LO"},{0x2238,"EXECLIST_STATUS_HI"},{0x23a0,"RING_CONTEXT_STATUS_PTR"},
    {0x229c,"RING_MODE_GEN7"}, // GFX_MODE for RCS
    {0x209c,"RING_MI_MODE"},{0x2080,"RING_HWS_PGA"},{0x2098,"RING_HWSTAM"},{0x20a8,"RING_IMR"},
    {0x2094,"RING_NOPID"},{0x2050,"RING_PSMI_CTL"},{0x20d0,"RING_RESET_CTL"},
    {0x2358,"RING_TIMESTAMP"},{0x235c,"RING_TIMESTAMP_UDW"},
    {0x2270,"GEN8_RING_PDP_UDW0"},{0x2274,"GEN8_RING_PDP_LDW0"},{0x2278,"GEN8_RING_PDP_UDW1"},{0x227c,"GEN8_RING_PDP_LDW1"},
    {0x2280,"GEN8_RING_PDP_UDW2"},{0x2284,"GEN8_RING_PDP_LDW2"},{0x2288,"GEN8_RING_PDP_UDW3"},{0x228c,"GEN8_RING_PDP_LDW3"},
    {0x4094,"GEN8_RING_FAULT_REG"},{0x4b10,"GEN8_FAULT_TLB_DATA0"},{0x4b14,"GEN8_FAULT_TLB_DATA1"},
    {0x40a0,"ERROR_GEN6"},{0x40b0,"DONE_REG"},{0x4090,"GAM_ECOCHK"},
    {0x40e0,"GEN8_PRIVATE_PAT_LO"},{0x40e4,"GEN8_PRIVATE_PAT_HI"},
    {0x941c,"GEN6_GDRST"},{0x0fdc,"GEN8_MCR_SELECTOR"},
    {0xa188,"FORCEWAKE_MT"},{0x130044,"FORCEWAKE_ACK_HSW"},
    {0xa090,"GEN6_RC_CONTROL"},{0xa094,"GEN6_RC_STATE"},{0x138060,"GEN6_GT_CORE_STATUS"},{0x138108,"GEN6_GT_GFX_RC6"},
    {0xa01c,"GEN6_RPSTAT1"},{0xa008,"GEN6_RPNSWREQ"},
};
constexpr unsigned BdwHangRegCount=sizeof(BdwHangRegs)/sizeof(BdwHangRegs[0]);
// RING_CONTEXT_STATUS_BUF_LO/HI(base,i)=base+0x370+8*i(+4), six entries.
constexpr uint32_t BdwHangCsbRegBase=0x2370;

// Per-job record. Times are microseconds after prepareTsc (0 = not reached).
struct BdwHangBreadcrumb {
    uint32_t job=0,pid=0,ctx=0,nonce=0;
    uint32_t objects=0,pages=0,batchLen=0,seen=0;
    uint64_t batch=0,descriptor=0,prepareTsc=0;
    uint8_t engine=0,outcome=0,runFailSite=0,submitSite=0;
    uint8_t initStage=0,csbSite=0,portWrites=0,flags=0; // flags: 1 captured, 2 marker, 4 late (deadline grace)
    uint32_t ringHead=0,ringTail=0;
    uint32_t execlistBefore=0,csbPtrBefore=0,execlistAfter=0,execlistHiAfter=0,csbPtrAfter=0;
    uint32_t fwAck=0,rcState=0,coreStatus=0,polls=0;
    uint32_t elspUs=0,firstPollUs=0,breadcrumbUs=0,csbUs=0,doneUs=0,maxPollGapUs=0;
    // Longest yield (us) and its sleep part; pid that held the CPU meanwhile.
    uint32_t yieldUs=0,yieldWaitUs=0,yieldFromPid=0,yieldFromRanUs=0;
};
enum BdwHangOutcome : uint8_t {BdwHangOk=0,BdwHangRecovered=1,BdwHangUncertain=2};
inline const char* BdwHangOutcomeName(uint8_t o){
    return o==BdwHangOk?"ok":o==BdwHangRecovered?"failed_recovered":o==BdwHangUncertain?"failed_uncertain":"?";
}

// Seqlock word copies: one writer (the submission worker), any reader.
template<class T> inline void BdwHangCopyWords(T& out,const T& in){
    static_assert(sizeof(T)%4==0,"word copy");
    auto* d=reinterpret_cast<uint32_t*>(&out);auto* s=reinterpret_cast<const uint32_t*>(&in);
    for(unsigned i=0;i<sizeof(T)/4;++i)__atomic_store_n(d+i,__atomic_load_n(s+i,__ATOMIC_RELAXED),__ATOMIC_RELAXED);
}
template<unsigned N> class BdwHangCrumbRing {
    static_assert(N && !(N&(N-1)),"power of two");
    struct Slot {uint32_t seq=0,pad=0;BdwHangBreadcrumb b{};};
    Slot slots_[N]{};uint64_t count_=0;
public:
    static constexpr unsigned Capacity=N;
    // Single writer. b.job is overwritten with the 1-based record number.
    void Append(BdwHangBreadcrumb b){
        const uint64_t n=__atomic_load_n(&count_,__ATOMIC_RELAXED);
        auto& s=slots_[n&(N-1)];b.job=uint32_t(n+1);
        const uint32_t seq=__atomic_load_n(&s.seq,__ATOMIC_RELAXED);
        __atomic_store_n(&s.seq,seq+1,__ATOMIC_RELAXED);__atomic_thread_fence(__ATOMIC_RELEASE);
        BdwHangCopyWords(s.b,b);
        __atomic_store_n(&s.seq,seq+2,__ATOMIC_RELEASE);
        __atomic_store_n(&count_,n+1,__ATOMIC_RELEASE);
    }
    uint64_t Count()const{return __atomic_load_n(&count_,__ATOMIC_ACQUIRE);}
    // index is 0-based since boot; false once overwritten or while torn.
    bool Read(uint64_t index,BdwHangBreadcrumb& out)const{
        for(unsigned attempt=0;attempt<8;++attempt){
            const uint64_t count=Count();
            if(index>=count || count-index>N)return false;
            const auto& s=slots_[index&(N-1)];
            const uint32_t a=__atomic_load_n(&s.seq,__ATOMIC_ACQUIRE);
            if(a&1)continue;
            BdwHangCopyWords(out,s.b);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if(__atomic_load_n(&s.seq,__ATOMIC_RELAXED)==a && out.job==uint32_t(index+1))return true;
        }
        return false;
    }
};

constexpr unsigned BdwHangCrumbsInCapture=16;
struct BdwHangCapture {
    uint32_t version=1;uint8_t kind=0; // 1 failure, 2 reference (healthy job)
    uint8_t regsValid=0,pagesValid=0,ptesValid=0;
    uint64_t captureTsc=0,khz=0;
    BdwRenderJobDiag diag{};
    // Job identity and the existing failure fields, filled by the worker.
    uint32_t pid=0,ctx=0,objects=0,pages=0,nonce=0,seen=0,polls=0;
    uint8_t outcome=0,runFailSite=0,submitSite=0,csbSite=0;uint32_t csbEvents=0;uint64_t csbEvent=0;
    uint64_t batch=0,batchLen=0;uint32_t batchBytes=0;uint32_t batch8[8]{};
    uint32_t regs[BdwHangRegCount]{};
    uint32_t csbRegs[12]{};
    // Memory, after a CPU cache flush: PPHWSP dwords 0x30..0x3f, LRC register
    // state (context page 1) dwords 0..0x47, the 56-dword ring, HWS dwords
    // 0x10..0x1f (CSB records and producer at 0x7c), completion dwords 0..7.
    uint32_t pphwsp[16]{},lrc[72]{},ring[56]{},hws[16]{},completion[8]{};
    // GGTT PTEs of the 29 leased pages (context 0..19, ring 20, HWS 24,
    // completion 25, batch 28) and each page's expected DMA address.
    uint64_t pte[29]{},dma[29]{};
    uint32_t crumbCount=0,pad=0;BdwHangBreadcrumb crumbs[BdwHangCrumbsInCapture]{};
};

// Zero in place (the capture is too large for a stack temporary).
inline void BdwHangClear(BdwHangCapture& c){
    auto* w=reinterpret_cast<uint32_t*>(&c);for(unsigned i=0;i<sizeof(c)/4;++i)w[i]=0;c.version=1;
}
inline int BdwHangRegIndex(uint32_t offset){
    for(unsigned i=0;i<BdwHangRegCount;++i)if(BdwHangRegs[i].offset==offset)return int(i);
    return -1;
}
inline uint32_t BdwHangReg32(const BdwHangCapture& c,uint32_t offset){
    const int i=BdwHangRegIndex(offset);return i<0?0:c.regs[i];
}

// Io: bool reg(uint32_t offset,uint32_t& v) (absolute MMIO offset, reads
// only), const uint8_t* page(unsigned oneShotIndex) after making it CPU
// visible (nullptr if unavailable), bool pte(unsigned leaseIndex,uint64_t&),
// uint64_t dma(unsigned leaseIndex). Never writes the device.
template<class Io> void BdwHangCaptureDevice(Io& io,BdwHangCapture& c){
    c.captureTsc=BdwHangTsc();
    bool all=true;
    for(unsigned i=0;i<BdwHangRegCount;++i){uint32_t v=0xffffffffu;if(!io.reg(BdwHangRegs[i].offset,v)){all=false;v=0xffffffffu;}c.regs[i]=v;}
    for(unsigned i=0;i<12;++i){uint32_t v=0xffffffffu;if(!io.reg(BdwHangCsbRegBase+4*i,v)){all=false;v=0xffffffffu;}c.csbRegs[i]=v;}
    c.regsValid=all;
    auto words=[](const uint8_t* p,unsigned firstDword,uint32_t* out,unsigned count){
        for(unsigned i=0;i<count;++i)out[i]=__atomic_load_n(reinterpret_cast<const uint32_t*>(p)+firstDword+i,__ATOMIC_RELAXED);
    };
    const uint8_t* pphwsp=io.page(0);const uint8_t* lrc=io.page(1);const uint8_t* ring=io.page(20);
    const uint8_t* hws=io.page(24);const uint8_t* completion=io.page(25);
    if(pphwsp)words(pphwsp,0x30,c.pphwsp,16);
    if(lrc)words(lrc,0,c.lrc,72);
    if(ring)words(ring,0,c.ring,56);
    if(hws)words(hws,0x10,c.hws,16);
    if(completion)words(completion,0,c.completion,8);
    c.pagesValid=pphwsp && lrc && ring && hws && completion;
    bool ptes=true;
    for(unsigned i=0;i<29;++i){c.dma[i]=io.dma(i);uint64_t v=~uint64_t{0};if(!io.pte(i,v)){ptes=false;v=~uint64_t{0};}c.pte[i]=v;}
    c.ptesValid=ptes;
}

// Ring layout (intel_bdw_render_ring.h), dword offsets: 0 PIPE_CONTROL
// invalidate, 6 barrier, 12 context workaround, 32 barrier/started marker,
// 38 MI_ARB_ON_OFF off, 39 MI_BATCH_BUFFER_START, 42 PIPE_CONTROL flush,
// 48 breadcrumb PIPE_CONTROL, 54 MI_ARB_ON_OFF on, 55 MI_NOOP; tail 0xe0.
// RING_HEAD 0xa8 with BBADDR set means "inside the user batch".
constexpr uint32_t BdwHangRingBatchHead=0xa8,BdwHangRingEnd=0xe0;

// Heuristic first reading of a capture; docs/gpu-hang-diagnostics.md lists
// the evidence behind each verdict. Never a substitute for the raw fields.
inline const char* BdwHangVerdict(const BdwHangCapture& c){
    const auto& d=c.diag;
    if(c.kind==2 && c.completion[0]==c.nonce && !c.completion[1])return "completed";
    if(d.portWrites<4)return d.initStage?"execlist_init_failed_not_submitted":"not_submitted";
    if(c.runFailSite==3)return "csb_unexpected_event_after_breadcrumb";
    if(c.runFailSite==4)return "context_complete_not_seen_after_breadcrumb";
    if(c.submitSite==11 || c.submitSite==12)return "breadcrumb_visibility_or_value_mismatch";
    if(c.pagesValid && c.completion[0]==c.nonce && !c.completion[1])return "breadcrumb_late_after_deadline";
    if(!c.regsValid)return "registers_unreadable";
    if((BdwHangReg32(c,0x4094)&1) || BdwHangReg32(c,0x40a0))return "gpu_fault";
    if(BdwHangReg32(c,0x20b0))return "engine_error_eir";
    const uint32_t head=BdwHangReg32(c,0x2034)&0x001ffffcu,tail=BdwHangReg32(c,0x2030)&0x001ffff8u;
    const uint32_t ipehr=BdwHangReg32(c,0x2068);
    const bool started=d.marker && c.pagesValid && c.completion[BdwHangStartedOffset/4]==c.nonce &&
        !c.completion[BdwHangStartedOffset/4+1];
    const uint32_t csbWrite=BdwHangReg32(c,0x23a0)&7u;
    const bool csbEvents=csbWrite!=5u || (c.pagesValid && (c.hws[0xf]&0xffu)!=5u);
    if(head==tail && head==BdwHangRingEnd)return "ring_completed_breadcrumb_missing";
    if(d.marker && !started){
        if(!csbEvents)return (BdwHangReg32(c,0x2234)&(3u<<3))?"context_not_loaded_element_pending":"elsp_lost_no_csb_event";
        return head<BdwHangRingBatchHead?"stalled_in_ring_prelude":"marker_missing_but_past_prelude";
    }
    if((ipehr>>23)==0x1c || (ipehr>>23)==0x03)return "waiting_semaphore_or_event_in_batch";
    if(head==BdwHangRingBatchHead || (BdwHangReg32(c,0x2140)|BdwHangReg32(c,0x2168)))return "hung_in_user_batch";
    if(!csbEvents)return "no_csb_event";
    return "unclassified";
}

// JSON writers: W provides text(const char*), number(uint64_t,bool hex),
// field(const char*,uint64_t) (BridgeReplyBuffer).
template<class W> void BdwHangHex(W& r,uint64_t v){r.text("\"0x");r.number(v,true);r.text("\"");}
template<class W> void BdwHangHexField(W& r,const char* name,uint64_t v){r.text("\"");r.text(name);r.text("\":");BdwHangHex(r,v);}
template<class W> void BdwHangHexArray(W& r,const char* name,const uint32_t* v,unsigned n){
    r.text("\"");r.text(name);r.text("\":[");
    for(unsigned i=0;i<n;++i){if(i)r.text(",");BdwHangHex(r,v[i]);}
    r.text("]");
}
template<class W> void BdwHangYieldJson(W& r,const BdwRenderJobDiag& d,uint64_t khz){
    const auto& y=d.longest;auto us=[&](uint64_t t)->uint64_t{return khz?t*1000/khz:0;};
    r.text(",\"yields\":");r.number(d.yields);r.text(",");r.field("grace",d.grace);
    r.text(",\"longest_yield\":{\"valid\":");r.text(y.valid?"true":"false");
    r.text(",\"spin\":");r.text(y.spin?"true":"false");
    r.text(",");r.field("at_us",us(y.offsetTicks));r.text(",");r.field("total_us",us(y.totalTicks));
    r.text(",");r.field("unlock_us",us(y.unlockTicks));r.text(",");r.field("wait_us",us(y.waitTicks));
    r.text(",");r.field("lock_us",us(y.lockTicks));r.text(",");r.field("audit_us",us(y.auditTicks));
    r.text(",");r.field("apic_ticks",y.apicTicks);r.text(",");r.field("switches",y.switches);
    r.text(",");r.field("from_pid",y.fromPid);r.text(",\"from_kernel\":");r.text(y.fromKernel?"true":"false");
    r.text(",\"from_idle\":");r.text(y.fromIdle?"true":"false");r.text(",");r.field("from_ran_us",us(y.fromRanTicks));r.text("}");
}
template<class W> void BdwHangCrumbJson(W& r,const BdwHangBreadcrumb& b){
    r.text("{");r.field("job",b.job);r.text(",");r.field("pid",b.pid);r.text(",");r.field("ctx",b.ctx);
    r.text(",\"outcome\":\"");r.text(BdwHangOutcomeName(b.outcome));r.text("\",");
    BdwHangHexField(r,"nonce",b.nonce);r.text(",");BdwHangHexField(r,"seen",b.seen);
    r.text(",");r.field("objects",b.objects);r.text(",");r.field("pages",b.pages);
    r.text(",");BdwHangHexField(r,"batch",b.batch);r.text(",");r.field("batch_len",b.batchLen);
    r.text(",");BdwHangHexField(r,"descriptor",b.descriptor);
    r.text(",");BdwHangHexField(r,"ring_head",b.ringHead);r.text(",");BdwHangHexField(r,"ring_tail",b.ringTail);
    r.text(",");r.field("site",b.runFailSite);r.text(",");r.field("submit",b.submitSite);
    r.text(",");r.field("init",b.initStage);r.text(",");r.field("csb_site",b.csbSite);
    r.text(",");r.field("elsp_writes",b.portWrites);r.text(",");r.field("flags",b.flags);
    r.text(",\"elsp_before\":[");BdwHangHex(r,b.execlistBefore);r.text(",");BdwHangHex(r,b.csbPtrBefore);r.text("]");
    r.text(",\"elsp_after\":[");BdwHangHex(r,b.execlistAfter);r.text(",");BdwHangHex(r,b.execlistHiAfter);
    r.text(",");BdwHangHex(r,b.csbPtrAfter);r.text("]");
    r.text(",");BdwHangHexField(r,"fw_ack",b.fwAck);r.text(",");BdwHangHexField(r,"rc_state",b.rcState);
    r.text(",");BdwHangHexField(r,"core_status",b.coreStatus);r.text(",");r.field("polls",b.polls);
    r.text(",\"us\":[");r.number(b.elspUs);r.text(",");r.number(b.firstPollUs);r.text(",");r.number(b.breadcrumbUs);
    r.text(",");r.number(b.csbUs);r.text(",");r.number(b.doneUs);r.text(",");r.number(b.maxPollGapUs);r.text("]");
    r.text(",\"yield\":[");r.number(b.yieldUs);r.text(",");r.number(b.yieldWaitUs);r.text(",");r.number(b.yieldFromPid);
    r.text(",");r.number(b.yieldFromRanUs);r.text("]}");
}
template<class W> void BdwHangCaptureJson(W& r,const BdwHangCapture& c){
    const auto& d=c.diag;
    auto us=[&](uint64_t t)->uint64_t{return c.khz && t>=d.originTsc && d.originTsc?(t-d.originTsc)*1000/c.khz:0;};
    r.text("{\"kind\":\"");r.text(c.kind==1?"failure":c.kind==2?"reference":"none");
    r.text("\",\"verdict\":\"");r.text(BdwHangVerdict(c));r.text("\",");
    r.field("pid",c.pid);r.text(",");r.field("ctx",c.ctx);
    r.text(",\"outcome\":\"");r.text(BdwHangOutcomeName(c.outcome));r.text("\",");
    r.field("objects",c.objects);r.text(",");r.field("pages",c.pages);
    r.text(",");r.field("site",c.runFailSite);r.text(",");r.field("submit",c.submitSite);
    r.text(",");r.field("csb_site",c.csbSite);r.text(",");r.field("csb_events",c.csbEvents);
    r.text(",");BdwHangHexField(r,"csb_event",c.csbEvent);
    r.text(",");BdwHangHexField(r,"nonce",c.nonce);r.text(",");BdwHangHexField(r,"seen_at_deadline",c.seen);
    r.text(",");r.field("polls",c.polls);
    r.text(",\"init\":{");r.field("stage",d.initStage);r.text(",");r.field("exec_stage",d.execInitStage);
    r.text(",");BdwHangHexField(r,"exec_value",d.execInitValue);
    r.text(",");r.field("takeover_reset",d.takeoverReset);r.text(",");r.field("non_quiescent",d.nonQuiescent);
    r.text(",\"busy\":[");BdwHangHex(r,d.busy.head);r.text(",");BdwHangHex(r,d.busy.tail);r.text(",");BdwHangHex(r,d.busy.mode);
    r.text(",");BdwHangHex(r,d.busy.control);r.text(",");BdwHangHex(r,d.busy.execlistLo);r.text(",");BdwHangHex(r,d.busy.execlistHi);
    r.text(",");BdwHangHex(r,d.busy.ringMode);r.text("]}");
    r.text(",");BdwHangHexField(r,"descriptor",d.descriptor);
    r.text(",");BdwHangHexField(r,"ring_head",d.ringHead);r.text(",");BdwHangHexField(r,"ring_tail",d.ringTail);
    r.text(",");r.field("elsp_writes",d.portWrites);r.text(",");r.field("deadline",d.deadline);
    r.text(",");r.field("stop_reset",d.stopReset);r.text(",\"marker\":");r.text(d.marker?"true":"false");
    const BdwSubmitSample* samples[]={&d.before,&d.after};const char* sampleNames[]={"elsp_before","elsp_after"};
    for(unsigned i=0;i<2;++i){const auto& s=*samples[i];
        r.text(",\"");r.text(sampleNames[i]);r.text("\":{\"valid\":");r.text(s.valid?"true":"false");
        r.text(",");BdwHangHexField(r,"execlist_lo",s.execlistLo);r.text(",");BdwHangHexField(r,"execlist_hi",s.execlistHi);
        r.text(",");BdwHangHexField(r,"csb_ptr",s.csbPtr);r.text(",");BdwHangHexField(r,"fw_ack",s.fwAck);
        r.text(",");BdwHangHexField(r,"rc_state",s.rcState);r.text(",");BdwHangHexField(r,"core_status",s.coreStatus);r.text("}");}
    r.text(",\"us_since_origin\":{");r.field("init_end",us(d.initEndTsc));r.text(",");r.field("elsp",us(d.elspTsc));
    r.text(",");r.field("first_poll",us(d.firstPollTsc));r.text(",");r.field("last_poll",us(d.lastPollTsc));
    r.text(",");r.field("breadcrumb",us(d.breadcrumbTsc));r.text(",");r.field("first_csb",us(d.firstCsbTsc));
    r.text(",");r.field("run_end",us(d.runEndTsc));r.text(",");r.field("capture",us(c.captureTsc));
    r.text(",");r.field("max_poll_gap",c.khz?d.maxPollGapTicks*1000/c.khz:0);r.text("}");
    BdwHangYieldJson(r,d,c.khz);
    r.text(",\"breadcrumb\":{");BdwHangHexField(r,"now",c.completion[0]);r.text(",");BdwHangHexField(r,"now_hi",c.completion[1]);
    r.text(",");BdwHangHexField(r,"started",c.completion[BdwHangStartedOffset/4]);
    r.text(",");BdwHangHexField(r,"started_hi",c.completion[BdwHangStartedOffset/4+1]);r.text("}");
    r.text(",\"valid\":{\"regs\":");r.text(c.regsValid?"true":"false");r.text(",\"pages\":");r.text(c.pagesValid?"true":"false");
    r.text(",\"ptes\":");r.text(c.ptesValid?"true":"false");r.text("},\"regs\":{");
    for(unsigned i=0;i<BdwHangRegCount;++i){if(i)r.text(",");BdwHangHexField(r,BdwHangRegs[i].name,c.regs[i]);}
    r.text("},");BdwHangHexArray(r,"csb_regs",c.csbRegs,12);
    r.text(",");BdwHangHexArray(r,"hws_0x40",c.hws,16);
    r.text(",");BdwHangHexArray(r,"pphwsp_0xc0",c.pphwsp,16);
    r.text(",");BdwHangHexArray(r,"lrc_regstate",c.lrc,72);
    r.text(",");BdwHangHexArray(r,"ring",c.ring,56);
    r.text(",");BdwHangHexArray(r,"completion",c.completion,8);
    r.text(",\"batch\":{");BdwHangHexField(r,"addr",c.batch);r.text(",");r.field("len",c.batchLen);
    r.text(",");r.field("sampled_bytes",c.batchBytes);r.text(",");BdwHangHexArray(r,"words",c.batch8,8);r.text("}");
    r.text(",\"ggtt\":[");
    for(unsigned i=0;i<29;++i){if(i)r.text(",");r.text("[");BdwHangHex(r,c.pte[i]);r.text(",");BdwHangHex(r,c.dma[i]);r.text("]");}
    r.text("],\"crumbs\":[");
    for(unsigned i=0;i<c.crumbCount && i<BdwHangCrumbsInCapture;++i){if(i)r.text(",");BdwHangCrumbJson(r,c.crumbs[i]);}
    r.text("]}");
}

// Kernel side (intel_bdw_hang_diag.cpp).
struct BdwRenderRunResult;
struct BdwHangJobInfo {
    uint32_t pid=0,ctx=0,objects=0,pages=0,nonce=0;uint64_t batch=0,batchLen=0;
    uint32_t batchBytes=0,batch8[8]{};uint64_t prepareTsc=0,khz=0;
};
// Storage the critical section fills; nullptr when no capture is wanted.
BdwHangCapture* BdwHangScratchCapture();
// True once after "gpu hang reference": capture the next job even if it succeeds.
bool BdwHangTakeReferenceRequest();
// Record the job's breadcrumb; on failure (or a reference capture) publish
// the capture and print a KLog summary. Never touches the device.
void BdwHangNoteRenderJob(const BdwHangJobInfo& job,const BdwRenderRunResult& run,bool ok,bool recovered);
bool BdwHangHandleCommand(const char* cmd);
}
