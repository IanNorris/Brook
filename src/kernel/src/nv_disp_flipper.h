#pragma once
// RTX 2080 resident display driver, hot path: page flips on one window channel
// without the Plan executor or per-step logging (design: notes
// rtx2080-dual-monitor-design.md §3). Proven on hardware by the step-3 runs:
// SET_PRESENT_CONTROL 0 (no MPI wait under the GOP head), NON_TEARING, image
// state + window-only UPDATE; completion = GET == PUT, ARM offset/ISO == the
// submission, METHOD_EXEC idle and UPD_STATE IDLE (window notifiers only reach BEGUN).
//
// Guard by construction: the flipper writes only its channel's PUT, the BAR1 flush
// register and its head's EVT_STAT_HEAD_TIMING (W1C of VBLANK), and VRAM inside its
// own ring. Anything else is refused and fails the flipper.
#include "nv_disp_plan.h"

namespace brook::nvdisp {

enum class FlipState:uint8_t{Off,Idle,Armed,Failed};
enum class FlipFail:uint8_t{None,Guard,Exception,Timeout,Flush,Bounds};
struct FlipStats{uint32_t presents=0,completes=0,replaced=0,wraps=0,lastLatUs=0,maxLatUs=0;uint64_t vblanks=0;};

constexpr uint32_t RegHeadTimingEvt(uint32_t h){return 0x611800+h*4;} // EVT_STAT_HEAD_TIMING, bit 2 VBLANK (W1C)
constexpr uint32_t kEvtVblank=0x4;

// Backend: rd, wr, vrd, vwr, fence, nowUs, relax (as the runners' backends).
template<typename B> struct NvWindowFlipper{
    static constexpr uint32_t kMaxBuffers=2;
    static constexpr uint32_t kWatchdogUs=250000;   // >> 3 frames at 60 Hz
    static constexpr uint32_t kRingBytes=kPushBytes;
    B* b=nullptr;
    uint32_t chid=0,window=0,head=0,handle=kHandleIsoPitch;
    uint64_t ring=0;uint32_t put=0;
    Surface surf[kMaxBuffers]{};
    FlipState state=FlipState::Off;FlipFail fail=FlipFail::None;
    int8_t front=0,armed=-1,queued=-1;
    uint64_t armedAtUs=0;uint32_t armedTarget=0,armedIso=0;
    FlipStats st{};

    void Init(B* backend,uint32_t window_,uint32_t head_,uint64_t ringVram,uint32_t putBytes,const Surface& a,const Surface& bb,int8_t frontBuf){
        b=backend;window=window_;chid=ChidWndw(window_);head=head_;ring=ringVram;put=putBytes;
        surf[0]=a;surf[1]=bb;front=frontBuf;armed=queued=-1;state=FlipState::Idle;fail=FlipFail::None;st=FlipStats{};}
    bool Live()const{return state==FlipState::Idle||state==FlipState::Armed;}
    bool Allowed(uint32_t a)const{return a==RegPut(chid)||a==kRegBar1Flush||a==RegHeadTimingEvt(head);}
    void Fail(FlipFail f){if(state!=FlipState::Failed){fail=f;state=FlipState::Failed;}}
    void Wr(uint32_t a,uint32_t v){if(!Allowed(a)){Fail(FlipFail::Guard);return;}b->wr(a,v);}

    // Vblank counter from the latched event bit (polled; the IRQ path comes later).
    void TickVblank(){if(b->rd(RegHeadTimingEvt(head))&kEvtVblank){Wr(RegHeadTimingEvt(head),kEvtVblank);++st.vblanks;}}

    // Copy n words into the ring at PUT (JUMP 0 first if they do not fit), flush, kick.
    // Only called with nothing in flight (GET == PUT), so the JUMP is safe.
    bool Kick(const uint32_t* w,uint32_t n){
        const uint32_t bytes=n*4;
        if(bytes+8>kRingBytes){Fail(FlipFail::Bounds);return false;}
        if(put+bytes+4>kRingBytes){b->vwr(ring+put,PushJump(0));put=0;++st.wraps;}
        for(uint32_t i=0;i<n;++i)b->vwr(ring+put+i*4,w[i]);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);b->fence();
        Wr(kRegBar1Flush,1);
        bool flushed=false;for(uint32_t t=0;t<20000;++t)if(!(b->rd(kRegBar1Flush)&0x2)){flushed=true;break;}
        if(!flushed){Fail(FlipFail::Flush);return false;}
        put+=bytes;Wr(RegPut(chid),put);
        return state!=FlipState::Failed;
    }
    bool ArmWords(const uint32_t* w,uint32_t n,uint32_t iso,uint32_t offset){
        if(!Kick(w,n))return false;
        armedIso=iso;armedTarget=offset;armedAtUs=b->nowUs();state=FlipState::Armed;return true;}
    void Arm(int8_t buf){
        Push p;PushWindowFlip(p,surf[buf],handle,0);PushWindowUpdate(p,window);
        if(ArmWords(p.words,p.count,handle,uint32_t(surf[buf].vram>>8)))armed=buf;
    }
    // Present buffer `buf` at the next vblank. 0 = armed now, 1 = queued (mailbox:
    // replaces an older queued frame), -1 = not live.
    int Present(int8_t buf){
        if(buf<0||buf>=int8_t(kMaxBuffers))return -1;
        if(state==FlipState::Idle){++st.presents;Arm(buf);return state==FlipState::Armed?0:-1;}
        if(state==FlipState::Armed){++st.presents;if(queued>=0)++st.replaced;queued=buf;return 1;}
        return -1;
    }
    bool Complete(){
        return b->rd(RegGet(chid))==put&&b->rd(RegArm(chid,c57e::SET_OFFSET0))==armedTarget&&
               b->rd(RegArm(chid,c57e::SET_CONTEXT_DMA_ISO0))==armedIso&&
               (b->rd(RegChnStatus(chid))&(kWinMethodExecBusy|0xf00))==0x100;}
    // Non-blocking; call every compositor loop / driver tick.
    FlipState Poll(){
        if(state!=FlipState::Armed)return state;
        if(b->rd(kRegExcWin)&(1u<<window)){Fail(FlipFail::Exception);return state;}
        const uint64_t now=b->nowUs();
        if(Complete()){
            const uint32_t lat=uint32_t(now-armedAtUs);st.lastLatUs=lat;if(lat>st.maxLatUs)st.maxLatUs=lat;++st.completes;
            if(armed>=0)front=armed;armed=-1;state=FlipState::Idle;
            if(queued>=0){const int8_t q=queued;queued=-1;Arm(q);}
            return state;}
        if(now-armedAtUs>kWatchdogUs)Fail(FlipFail::Timeout);
        return state;
    }
    // Back buffer for the compositor: the one neither on screen nor armed.
    int8_t Back()const{for(int8_t i=0;i<int8_t(kMaxBuffers);++i)if(i!=front&&i!=armed)return i;return -1;}
    // Synchronous submission for teardown (restore / hide): waits for anything in
    // flight, kicks the words, waits until ARM shows (iso, offset). Bounded.
    bool SubmitWait(const uint32_t* w,uint32_t n,uint32_t iso,uint32_t offset,uint32_t timeoutUs){
        const uint64_t t0=b->nowUs();
        while(state==FlipState::Armed&&b->nowUs()-t0<timeoutUs){(void)Poll();b->relax();}
        if(state!=FlipState::Idle)return false;
        queued=-1;
        if(!ArmWords(w,n,iso,offset))return false;
        while(state==FlipState::Armed&&b->nowUs()-t0<timeoutUs){(void)Poll();b->relax();}
        armed=-1;
        return state==FlipState::Idle;
    }
};
} // namespace brook::nvdisp
